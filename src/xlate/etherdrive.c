#include <u.h>
#include <libc.h>
#include <ip.h>
#include <fcall.h>
#include <fis.h>
#include <libcutil.h>
#include "dat.h"
#include "fns.h"

// Copyright © 2010 Coraid, Inc.
// All rights reserved.

typedef	struct 	Timer	Timer;

struct	Timer
{
	int 	cnt;		/* count down counter for timer */
	int	ms;		/* how many milliseconds between invocations */
	QLock	*q;
	Rendez	*r;		/* what to wake up */
};

static	void	getlength(Msg *);
static	void	setlength(Msg *);
static	void	notepath(Msg *);
static	void	tendfound(void);
static	int	rdpaths(void *, void *, int, vlong);
static	void	cleanpaths(void);
static	void	assembler(void);
static	void	tendtags(void);
static	void	tendwork(void);
static	void	tendlost(void);
static	Target *newtarg(int);
static	void	freetarg(Target *);
static	Target *clookuptarg(int, int);
static	int	tagck(Tag *, ulong);
static	int	tagio(Tag *);
static	ulong	rttrto(Target *);
static	void	rttavg(Target *, ulong);
static	ulong	rttdelta(ulong);
static	int	rdtags(void *, void *, int, vlong);
static	int	rdtagio(void *, void *, int, vlong);

enum {
	Nthash = 63,
	Ntags	= 4096,
};

static	Rendez	rdiscover;		/* wakeme when i should discover */
static	Rendez	rclean;		/* wakeme to clean old port paths */
static	Rendez	assy;
static	Rendez	rlost;
static	Rendez	rfound;
static	QLock	qassy;
static	QLock	lostlk;
static	Target	*targs[Nthash];
static	int	ntags;
static	Tag	tags[Ntags];
static	QLock	taglk;
enum {
	TPdisc= 15*1000,
	TPclean= 60*1000,
	TPassy= 5*1000,
	TPlost= 5*1000,
};

static	Timer	wakelist[] = {
	{ TPdisc, TPdisc, &targetlk, &rdiscover },
	{ TPclean, TPclean, &targetlk, &rclean },
	{ TPassy, TPassy, &qassy, &assy },
	{ TPlost, TPlost, &lostlk, &rlost },
};

void
phyinit(void)	/* get physical layer running */
{
	static inited;
	Tag *c;

	if (inited)
		return;
	inited++;
	newfile(targdir, "tags", 0444, 0, rdtags, nowrite, nil);
	newfile(targdir, "tagio", 0444, 0, rdtagio, nowrite, nil);
	for (c = tags; c < &tags[Ntags]; c++)
		c->l = &taglk;
	rdiscover.l = &targetlk;
	rclean.l = &targetlk;
	assy.l = &qassy;
	rlost.l = &lostlk;
	rfound.l = &lostlk;
	if (xlrfork(RFPROC|RFMEM, "assembler") == 0)
		assembler();
	if (xlrfork(RFPROC|RFMEM, "tendtags") == 0)
		tendtags();
	if (xlrfork(RFPROC|RFMEM, "tendworks") == 0)
		tendwork();
	if (xlrfork(RFPROC|RFMEM, "cleanpaths") == 0)
		cleanpaths();
	if (xlrfork(RFPROC|RFMEM, "tendlost") == 0)
		tendlost();
	if (xlrfork(RFPROC|RFMEM, "tendfound") == 0)
		tendfound();
}

static void
aqcreq(AoeQC *a, int shelf, uchar slot)
{
	memset(a, 0, 60);
	memset(a->d, 0xff, 6);
	hnputs(a->t, 0x88a2);
	hnputs(a->shelf, shelf);
	a->slot = slot;
	a->vf = 0x10;
	a->proto = ACQC;
	a->ccmd = AQCREAD;
	hnputl(a->tag, TAGDISC);
}

int
discovertarg(int target)
{
	AoeQC *a;
	char buf[60];
	int i;

	if (target < 0)
		return -1;
	
	a = (AoeQC *)buf;
	aqcreq(a, SH(target), SL(target));

	for (i = 0; i < nsanports; i++) {
		if (sanports[i].fd < 0 || sanports[i].mbps < 1000)
			continue;
		aoesend(sanports[i].fd, buf, sizeof buf);
	}
	return 0;
}

void
discover(int port)	/* we sent id.  Someone else gets the response */
{
	char buf[60];
	AoeQC *a;
	int fd;

	fd = sanports[port].fd;	
	a = (AoeQC *)buf;
	aqcreq(a, 0xffff, 0xff);

	for (;;) {
		if (shutdown)
			xlexits(0);
		if (sanports[port].mbps < 1000) {
			sleep(1000);
			continue;
		}
		aoesend(fd, buf, sizeof buf);
		qlock(&targetlk);
		rsleep(&rdiscover);
		qunlock(&targetlk);
	}
}

void
fdiscover(void)
{
	qlock(&targetlk);
	rwakeupall(&rdiscover);
	qunlock(&targetlk);
}

void
reletarg(Target *t)
{
	qlock(&targetlk);
	if (--t->ref == 0 && t->freeme)
		freetarg(t);
	qunlock(&targetlk);
}

/*
 * Called with targetlk held.
 * The first caller to this must have removed the Target from the
 * target list.  All other users have a reference to the unlinked
 * target that they will release upon final reference removal.
 */
static void
freetarg(Target *t)
{
	/* don't double free, but evict from namespace immediately */
	if (t->freeme == 0)
		deldir(t->dir);
	if (t->ref) {
		t->freeme = 1;
		return;
	}
	free(t->name);
	free(t);
}

int
flushtarg(int target, int force)
{
	Target *t, **tt;
	uint h;

	if (target < 0)
		return -1;

	qlock(&targetlk);
	h = target % Nthash;
	tt = targs + h;
	for (; t = *tt; tt = &t->next) {
		if (t->target == target) {
			if (!force && t->ntmac) {
				uerr("can not flush %T with %d active port connection%s", 
					target, t->ntmac, t->ntmac > 1 ? "s" : "");
				qunlock(&targetlk);
				return -1;
			}
			*tt = t->next;
			freetarg(t);
			break;
		}
	}
	qunlock(&targetlk);
	return 0;
}

int
flushshelf(int target, int force)
{
	Target *t, **tt;
	int i;

	if (target < 0)
		return -1;

	qlock(&targetlk);
	for (i=0; i<Nthash; i++) {
		tt = targs + i;
		while (t = *tt) {
			if (SH(target) != 0xffff && SH(t->target) != SH(target)) {
				tt = &t->next;
				continue;
			}
			if (!force && t->ntmac) {
				tt = &t->next;
				continue;
			}
			*tt = t->next;
			freetarg(t);
		}
	}
	qunlock(&targetlk);
	return 0;
}

int
flush(int target, int force)
{
	if (target < 0)
		return -1;

	if (SL(target) == 0xff)
		return flushshelf(target, force);
	else
		return flushtarg(target, force);
}

static int
isresperror(Aoe *ah)
{
	AoeDisk *ad;

	if (ah->vf & AERR) {
		xincu(&aoeerrs);
		uerr("AoE error");
		return 1;
	}
	if (ah->proto != ACATA)
		return 0;

	ad = (AoeDisk *)ah;

	if (ad->cmd & ERR) {
		xincu(&ataerrs);
		uerr("ATA error");
		return 1;
	}
	return 0;
}

static void
gotrespdisc(Msg *m, Aoe *ah)
{
	if (isresperror(ah))
		return;

	switch (ah->proto) {
	case ACQC:
		notepath(m);
		getlength(m);
		break;
	case ACATA:
		setlength(m);
		break;
	}
}

/*
 * Handle responses.  Since we have to give the message to
 * the waiting process, the Msg must be on the heap, not the stack
 * segment.  And since we give the Msg to the other process we
 * can't give it back to the user.
 */
void
gotresp(Msg *m)	
{
	Aoe *ah;
	ulong tag;
	Tag *t;

	ah = (Aoe *)m->data;
	tag = nhgetl(ah->tag);

	if (tag & TAGDISC) {	/* could be discover response */
		gotrespdisc(m, ah);
		msgfree(m);
		return;
	}
	qlock(&taglk);
	t = &tags[tag & 0xfff];
	if (tagck(t, tag) >= 0) {
		t->rsp = m;
		rwakeup(t);
		qunlock(&taglk);
		return;
	}
	qunlock(&taglk);
	msgfree(m);
}

static void
addtmac(Target *t, uchar *ea, int port)
{
	Tmac *m, *e;

	m = t->tmac;
	e = m + t->ntmac;
	for (; m<e; m++) {
		if (memcmp(m->ea, ea, 6) != 0)
			continue;
		if (m->sanmask == 0)
			assembleck++;
		m->sanmask |= m->recent |= 1<<port;
		m->retried &= ~(1<<port);
		m->retries = 0;
		return;
	}
	if (t->ntmac == Ntmac) {
		if (t->nem < 10) {
			t->nem++;
			xsyslog("out of target address structures for %T, cannot use %E\n", t->target, ea);
		}
		return;
	}
	m->sanmask = m->recent = 1<<port;
	m->retried = 0;
	m->retries = 0;
	memmove(m->ea, ea, 6);
	t->ntmac++;
	assembleck++;
}

static void
notepath(Msg *m)	/* see if we need to add a new member to our list */
{
	AoeQC *aq;
	Target *t;
	int target, lotarg, hitarg;
	
	aq = (AoeQC *)m->data;
	target = nhgets(aq->shelf) << 8 | aq->slot;
	lotarg = shelf << 8;
	hitarg = lotarg + 4096;
	if (lotarg <= target && target < hitarg) {
		if (shelf != -1 && ticks > 60 * 1000 && shutdown == 0) 
			xsyslog("Contact Coraid TAC: Potential EtherSAN address conflict on %T\n", target);
		return;
	}

	t = clookuptarg(target, 1);
	if (t == nil)
		return;
	qlock(t);
	t->nqc = nhgets(aq->len);
	if (t->nqc > 1024)
		t->nqc = 1024;
	memmove(t->qc, aq->conf, t->nqc); /* saving, even though it's too
					     early to know if the mac is good
					   */
	qunlock(t);
	reletarg(t);
}

ulong
gbit32(void *a)
{
	uchar *i;
	ulong j;

	i = a;
	j = i[3]<<24;
	j |= i[2]<<16;
	j |= i[1]<<8;
	j |= i[0];
	return j;
}

vlong
gbit64(void *a)
{
	uchar *i;

	i = a;
	return ((uvlong) gbit32(i+4)<<32)|gbit32(a);
}

int
sncpy(char *b, void *a)
{
	int i;

	idmove(b, a, Nserial);
	i = strlen(b);
	b[i] = '\0';
	return i;
}

static int
sncpynl(char *b, void *a)
{
	int i;

	idmove(b, a, Nserial);
	i = strlen(b);
	b[i++] = '\n';
	b[i] = '\0';
	return i;
}

static int
setsn(Target *t, AoeDisk *a, char *pvsn)
{
	if (pvsn[0]) {
		t->flags |= (TARGpv|TARGckd);
		memcpy(t->serial[0].sn, pvsn, Nserial);
		t->serial[0].stime = time(nil);
		return memcmp(pvsn, a->data + 20, Nserial) == 0;
	}
	if (t->serial[0].stime) {
		if (memcmp(t->serial[0].sn, a->data + 20, Nserial) != 0) {
			t->serial[1] = t->serial[0];
			memcpy(t->serial[0].sn, a->data + 20, Nserial);
			t->serial[0].stime = time(nil); // last seen
		}
	} else {
		memcpy(t->serial[0].sn, a->data + 20, Nserial);
		t->serial[0].stime = time(nil); // first seen
	}
	if (!(t->flags & TARGckd))
		t->flags |= TARGckd;
	return 1;
}

static int
goodsn(Target *t, AoeDisk *a, char *pvsn)
{
	char good[Nserial+1], bad[Nserial+1];

	if (t->flags & TARGpv) {
		if (memcmp(t->serial[0].sn, a->data + 20, Nserial) == 0)
			return 1;
		memcpy(t->serial[1].sn, a->data + 20, Nserial);
		t->serial[1].stime = time(nil);
		sncpy(good, t->serial[0].sn);
		sncpy(bad, t->serial[1].sn);
		xsyslog("PV %T: %E serial number %s does not match %s\n",
			t->target, a->s, bad, good);
		return 0;
	}
	return setsn(t, a, pvsn);
}

static
void
setlength(Msg *m)
{
	AoeDisk *a;
	uvlong length;
	Target *t;
	int targ, found;
	PV *pv;
	char pvsn[Nserial];

	found = 0;
	a = (AoeDisk *)m->data;
	targ = nhgets(a->shelf) << 8 | a->slot;
	t = lookuptarg(targ);
	if (t == nil)
		return;
	pvsn[0] = '\0';
	if (!(t->flags & TARGckd)) {
		// Lookup PV early to avoid deadlock. See bug 4528.
		rlock(&lk);
		pv = targ2pv(targ);
		if (pv) {
			rlock(pv);
			if (targ == pv->targ)
				memcpy(pvsn, pv->sn[0], Nserial);
			else
				memcpy(pvsn, pv->sn[1], Nserial);
			runlock(pv);
		}
		runlock(&lk);
	}
	qlock(t);
	if (goodsn(t, a, pvsn)) {
		length = gbit64((ushort*)a->data + 100);
		if (length == 0)
			length = gbit32((ushort*)a->data + 60);
		length *= 512;
		if (t->length == 0)
			found++;
		t->length = length;
		addtmac(t, a->s, m->port);
		memmove(t->ident, a->data, 512);
	}
	qunlock(t);
	reletarg(t);
	if (found) {
		qlock(&lostlk);
		if (lostpvcnt)
			rwakeup(&rfound);
		qunlock(&lostlk);
	}
}

static void
logstaletmac(Target *t, Tmac *m)
{
	ulong retried, mask, i;
	char *list, *tmp;

	if (m->retries <= TMACtry)
		return;

	xsyslog("Target %T MAC %E retried %ud times\n",
		t->target, m->ea, m->retries);

	retried = m->sanmask & m->retried;

	if (retried) {
		list = nil;
		mask = 1;
		for (i = 0; i < 32 && mask <= retried; ++i) {
			if (mask & retried) {
				if (list) {
					tmp = smprint("%s %s",
						      list, sanports[i].name);
					free(list);
					list = tmp;
				} else
					list = smprint("%s", sanports[i].name);
			}
			mask <<= 1;
		}
		if (list)
			xsyslog("Target %T MAC %E retried %s\n",
				t->target, m->ea, list);
		free(list);
	}
}

static void
clearstaleserial(Target *t)
{
	char bad[Nserial+1];

	if (time(nil) - t->serial[1].stime < TPclean/1000)
		return;
	t->serial[1].stime = 0;
	if (!(t->flags & TARGpv))
		return;
	sncpy(bad, t->serial[1].sn);
	xsyslog("PV target %T serial number conflict %s cleared\n",
		t->target, bad);
}

static void
cleanpaths(void)
{
	Target *t;
	Tmac *m;
	int i, j, lost;

	qlock(&targetlk);
	for (;;) {
		lost = 0;
		if (shutdown) {
			qunlock(&targetlk);
			xlexits(0);
		}
		for (i=0; i<Nthash; i++) {
			for (t = targs[i]; t; t = t->next) {
				qlock(t);
				for (j=0; j<t->ntmac;) {
					m = t->tmac + j;
					m->sanmask = m->recent;
					m->recent = 0;
					if (m->sanmask == 0) {
						t->lasttmac = 0;
						*m = t->tmac[--t->ntmac];
						t->tmacver++;
					} else {
						if (logstale)
							logstaletmac(t, m);
						j++;
					}
				}
				if (t->ntmac == 0) {
					if (t->length != 0)
						lost++;
					t->length = 0;
				}
				if (t->serial[1].stime)
					clearstaleserial(t);
				qunlock(t);
			}
		}	
		if (lost) {
			qlock(&lostlk);
			lostck = lost;
			qunlock(&lostlk);
		}
		rsleep(&rclean);
	}
}

static
void
getlength(Msg *m) /* read length */
{
	AoeDisk *a;

	a = (AoeDisk *) m->data;
	memmove(a->d, a->s, 6);
	m->count = 8192 + sizeof *a;	// validate our jumbo requirement
	a->vf = 0x10;
	a->proto = ACATA;
	a->aflags = 0;
	a->fea = 0;
	a->sectors = 1;
	a->cmd = 0xec;
	hnputl(a->tag, TAGDISC);
	memset(a->lba, 0, sizeof a->lba);
	aoesend(sanports[m->port].fd, m->data, m->count);
}

static
int
rdpaths(void *va, void *a, int count, vlong offset)
{
	char buf[64*1024];
	char *s, *e;
	int targ;
	Target *t;
	Tmac *m, *me;
	
	targ = (int) va;
	t = lookuptarg(targ);
	if (t == nil)
		return 0;
	s = buf;
	e = s+sizeof buf;
	*s = 0;
	qlock(t);
	m = t->tmac;
	me = m + t->ntmac;
	for (; m<me; m++) {
		s = seprint(s, e, "%E 0x%08ulx 0x%08ulx 0x%08ulx %d\n",
			    m->ea, m->sanmask, m->recent,
			    m->retried, m->retries);
	}
	qunlock(t);
	reletarg(t);
	return readstr(offset, a, count, buf);
}

int
rdtarg(void *va, void *a, int count, vlong offset)
{
	char buf[8192];
	int targ;
	uvlong len;
	Target *t;
	PV *pv;

	targ = (int) va;
	len = 0;
	if (t = lookuptarg(targ)) {
		len = t->length;
		reletarg(t);
	}
	if (t == nil) {
		uerr("target %T is currently not available", targ);
	} else if (len == 0) {
		rlock(&lk);
		pv = targ2pv(targ);
		runlock(&lk);
		if (!pv)
			uerr("target %T is currently not available", targ);
	}
	snprint(buf, sizeof buf, "%T %lld\n", targ, len);
	return readstr(offset, a, count, buf);
}

char *
fmtconf(uchar *qc, int nqc)	/* format into static buffer */
{
	static char buf[2048];
	char *p;
	
	for (p = buf; nqc-- > 0; qc++)
		if (' ' <= *qc && *qc <= '~')
			*p++ = *qc;
		else {
			*p++ = '^';
			*p++ = *qc - 0100;
		}
	*p = 0;
	return buf;
}

int
rdident(void *va, void *a, int count, vlong offset)
{
	char buf[512];
	int targ;
	Target *t;

	targ = (int) va;
	t = lookuptarg(targ);
	if (t == nil)
		return 0;
	memmove(buf, t->ident, sizeof t->ident);
	reletarg(t);
	return readmem(offset, a, count, buf, sizeof t->ident);
}

int
rdrtt(void *va, void *a, int count, vlong offset)
{
	char buf[1024];
	Target *t;
	int targ;

	targ = (int) va;
	t = lookuptarg(targ);
	if (t == nil)
		return 0;
	qlock(t);
	snprint(buf, sizeof buf, "%uld (%uld) %uld (%uld)\n",
		t->srtt>>Rttscale, t->srtt, t->sdev>>Rttdscale, t->sdev);
	qunlock(t);
	reletarg(t);
	return readstr(offset, a, count, buf);
}

int
rdconf(void *va, void *a, int count, vlong offset)
{
	char buf[1024];
	Target *t;
	int targ;
	int len;

	targ = (int) va;
	t = lookuptarg(targ);
	if (t == nil)
		return 0;
	qlock(t);
	len = t->nqc;
	if (len > sizeof buf)
		len = sizeof buf;
	memmove(buf, t->qc, len);
	qunlock(t);
	reletarg(t);
	return readmem(offset, a, count, buf, len);
}

void
tagfree(Tag *t)
{
	if (t == nil)
		return;
	qlock(&taglk);
	t->used = 0;
	qunlock(&taglk);
}

Tag *
tagalloc(void)	/* get me a tag to use */
{
	Tag *t, *e;

	qlock(&taglk);
	t = tags;
	e = t + ntags;
	for (; t < e; t++) {
		if (t->used == 0)
			break;
	}
	if (t == e) {
		if (ntags == Ntags) {
			qunlock(&taglk);
			return nil;
		}
		t = &tags[ntags++];
	}
	t->used = 1;
	qunlock(&taglk);
	return t;
}

Msg *
tagsend(Tag *tag, Msg *cmd)	/* send and wait for response */
{
	Msg *rsp;
	Aoe *a;
	Target *t;
	int targ;

	a = (Aoe *)cmd->data;
	targ = nhgets(a->shelf) << 8 | a->slot;
	qlock(&taglk);
	if (tag->maxwait == 0)
		tag->maxwait = iotimeo;
	tag->targ = targ;
	tag->t = nil;
	tag->cmd = cmd;
	tag->rsp = nil;
	tag->nsent = 0;
	tag->starttick = ticks;
	++xmits;
	tagio(tag);
	rsleep(tag);
	tag->cmd = nil;
	rsp = tag->rsp;
	tag->rsp = nil;
	t = tag->t;
	tag->t = nil;
	if (t != nil)
		reletarg(t);
	tag->maxwait = 0;
	qunlock(&taglk);
	return rsp;
}

static
void
tendwork(void)
{
	Timer *w;
	int i;

	for (;;) {
		if (shutdown)
			xlexits(0);
		for (i = 0; i < nelem(wakelist); i++) {
			w = wakelist + i;
			if (w->cnt-- <= 0) {
				qlock(w->q);
				rwakeupall(w->r);
				qunlock(w->q);
				w->cnt = w->ms;
			}
		}
		sleep(1);
	}
}

static
void
tagretry(Tag *tag)
{
	int n;
	Target *t;
	Tmac *m;

	n = (tag->nsent - 1) % Ntsent;
	t = tag->t;

	if (tag->sent[n].tmacver == t->tmacver) {
		qlock(t);
		m = &t->tmac[tag->sent[n].tmi];
		if (tag->sent[n].tmacver == t->tmacver) {
			m->retried |= (1<<tag->sent[n].port);
			if (m->retries < 255)
				if (++m->retries == 255)
					rexmitlims++;
		}
		qunlock(t);
	}
}


static void
tagfailsyslog(Tag *t, ulong tk)
{
	xsyslog("AoE send failure %T %uld %uld %uld %ud %uld %uld %p\n",
		t->targ, tk, t->starttick, t->maxwait, t->nsent,
		t->rto, t->crto, t->t);
	if (t->t)
		xsyslog("AoE send target %s %uld %uld %ud %ud\n",
			t->t->name, t->t->srtt, t->t->sdev,
			t->t->ntmac, t->t->lasttmac);
}

static
void
tendtags(void)		/* watch for tag entry timeouts */
{
	Tag *t;
	ulong tk, stk;

	for (;;) {
		/* no - we need to service tags for xresproc
		if (shutdown)
			xlexits(0);
		*/
		qlock(&taglk);
		tk = ticks;
		for (t = tags; t < &tags[ntags]; t++) {
			if (t->cmd == nil)
				continue;
			if (tk - t->starttick > t->maxwait) {
				xmitfails++;
				if (tagdebug)
					tagfailsyslog(t, tk);
				rwakeup(t);
				continue;
			}
			/* Target was never set, force retry immediately */
			if (t->t == nil) {
				tagio(t);
				continue;
			}
			stk = t->sent[(t->nsent-1)%Ntsent].tick;
			if (tk - stk > t->crto) {
				++rexmits;
				tagretry(t);
				tagio(t);
			}
		}
		qunlock(&taglk);
		sleep(1);
	}
}

static int
rdtags(void *, void *a, int count, vlong offset)
{
	char buf[8192], *s, *e;
	Tag *t;
	ulong tk;
	int n;

	s = buf;
	e = buf + sizeof buf;
	*s = 0;
	qlock(&taglk);
	tk = ticks;
	for (t = tags; t < &tags[ntags]; t++) {
		if (t->cmd == nil)
			continue;
		n = t->nsent;
		s = seprint(s, e, "%08ulx %ud %ld %ld %ld %ld %p %p\n",
			n ? t->sent[(n-1)%Ntsent].tag : 0, n, t->crto,
			t->rto, tk - t->starttick, t->maxwait, t->cmd, t->rsp);
	}
	qunlock(&taglk);
	return readstr(offset, a, count, buf);
}

static int
rdtagio(void *, void *a, int count, vlong offset)
{
	char buf[8192], *s, *e;
	Tag *t;
	AoeDisk *ad;
	int n;

	s = buf;
	e = buf + sizeof buf;
	*s = 0;
	qlock(&taglk);
	for (t = tags; t < &tags[ntags]; t++) {
		if (t->cmd == nil)
			continue;
		ad = (AoeDisk *) t->cmd->data;
		if (ad->proto != 0)
			continue;
		n = t->nsent;
		s = seprint(s, e, "%08ulx %d %E %02ux %ullx %d\n",
			n ? t->sent[(n-1)%Ntsent].tag : 0, t->cmd->port,
			ad->d, ad->cmd, getlba(ad->lba), ad->sectors);
	}
	qunlock(&taglk);
	return readstr(offset, a, count, buf);
}

static Target *
tag2targ(Tag *tag)
{
	Target *t;

	t = tag->t;
	if (t) {
		if (t->freeme == 0)
			return t;
		reletarg(t);
	}
	return tag->t = clookuptarg(tag->targ, 0);
}

static int
tagio(Tag *tag)
{
	Aoe *a;
	Target *t;
	Msg *m;
	int n;
	ulong xt;

	m = tag->cmd;
	a = (Aoe *)m->data;
	t = tag2targ(tag);
	if (t == nil)
		return -1;
	qlock(t);
	if (tag->nsent == 0)
		tag->rto = rttrto(t);
	n = tag->nsent;
	if (n > Ntsent)
		n = Ntsent;
	tag->crto = tag->rto << n;
	if (tag->crto < Rtomin)
		tag->crto = Rtomin;
	if (tag->crto > Rtomax)
		tag->crto = Rtomax;
	n = tag->nsent % Ntsent;
	tag->sent[n].tag = 0;
	tag->sent[n].tick = ticks;
	tag->nsent++;
	/*
	 * we route after we set up all rto/crto settings and increment
	 * nsent so that if we temporarily lose the target (and return -1)
	 * we'll still make retransmit attempts properly.
	 */
	m->port = route(t, a->d, -1);
	if (m->port < 0) {
		qunlock(t);
		return -1;
	}
	tag->sent[n].tmi = t->lasttmac;
	tag->sent[n].tmacver = t->tmacver;
	tag->sent[n].port = m->port;
	xt = (tag-tags) | (++t->taggen<<12) & ~TAGDISC;
	qunlock(t);

	tag->sent[n].tag = xt;
	hnputl(a->tag, xt);

	aoesend(sanports[m->port].fd, m->data, m->count);
	return 0;
}

static int
tagck(Tag *tag, ulong intag)
{
	int i, n;
	ulong xtick;
	Target *t;

	if (tag->cmd == nil || tag->t == nil)
		return -1;
	n = tag->nsent % Ntsent;
	i = (tag->nsent > Ntsent) ? n : 0;
	xtick = tag->sent[i].tick;
	do {
		if (tag->sent[i].tag == intag) {
			++recvs;
			t = tag->t;
			qlock(t);
			rttavg(t, rttdelta(xtick));
			if (t->tmacver ==  tag->sent[i].tmacver) {
				t->tmac[tag->sent[i].tmi].retries = 0;
				t->tmac[tag->sent[i].tmi].retried &=
					~(1<<tag->sent[i].port);
			}
			qunlock(t);
			return 0;
		}
		i++;
		i %= Ntsent;
	} while (i != n);

	++resmiss;
	return -1;
}

static Tmac *
rrtmac(Target *t, int port)
{
	Tmac *om, *m, *e, *r;

	if (t->lasttmac > t->ntmac)
		t->lasttmac = 0;
	om = m = t->tmac + t->lasttmac;
	e = t->tmac + t->ntmac;
	r = nil;
	do {
		m++;
		if (m >= e)
			m = t->tmac;
		if (m->sanmask)
		if (port < 0 || m->sanmask & (1<<port)) {
			if (m->retries > TMACtry) {
				if (r == nil)
					r = m;
				continue;
			}
			t->lasttmac = m - t->tmac;
			return m;
		}
	} while (m != om);

	if (r) {
		t->lasttmac = r - t->tmac;
		return r;
	}
	return nil;
}

static int
rrport(Tmac *m)
{
	uint p, x, i;
	ulong sanmask;

	if ((1<<m->lastport & m->sanmask) == 0)
		m->lastport = 0;
	p = m->lastport;
	sanmask = ~m->retried & m->sanmask;
	if (sanmask == 0)
		sanmask = m->sanmask;
	for (i = 0; i < nsanports; i++) {
		p++;
		x = 1<<p;
		if (p >= nsanports) {
			p = 0;
			x = 1;
		}
		if ((sanmask & x) && sanports[p].mbps >= 1000)
			return m->lastport = p;
	}

	return -1;
}

int
route(Target *t, uchar *ea, int port)
{
	Tmac *m;

	m = rrtmac(t, port);
	if (m == nil)
		return -1;
	memcpy(ea, m->ea, 6);
	if (port < 0)
		port = rrport(m);
	return port;
}
int
parsetargetstr(char *s, int allow)
{
	char *orig;
	int u, v;
	int sh, sl;
	
	orig = s;
	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == 0) {
		uerr("no target specified");
		return -1;
	}
	if (*s == '.') {
		uerr("%s has an invalid format, expect XX.X", orig);
		return -1;
	}		
	v = 0;
	while ('0' <= *s && *s <= '9')
		v = v * 10 + *s++ - '0';
	u = v;
	if (*s != '.') {
		uerr("%s has an invalid format, expect XX.X", orig);
		return -1;
	}
	s++;
	if (*s == 0) {
		uerr("%s has an invalid format, expect XX.X", orig);
		return -1;
	}
	v = 0;
	while ('0' <= *s && *s <= '9')
		v = v * 10 + *s++ - '0';
	if (*s)  {
		uerr("%s has an invalid format, expect XX.X", orig);
		return -1;
	}
	/*
	 * if you change this to permit broadcast, find *every*
	 * use of this command and make sure broadcast guards are
	 * put in.
	 */
	sh = allow ? 0xffff : 0xfffe;
	sl = allow ? 0xff   : 0xfe;
	if (u < 0 || u > sh) {
		uerr("shelf %d outside AoE%s range [0 - %d]", u, allow ? "" : " working", sh);
		return -1;
	}
	if (v < 0 || v > sl) {
		uerr("lun id %d outside AoE%s range [0 - %d]", v, allow ? "" :  " working", sl);
		return -1;
	}
	return u << 8 | v & 0xff;
}

int
parsetarget(char *s)
{
	return parsetargetstr(s, 0);
}

int
parsetargetbc(char *s)
{
	return parsetargetstr(s, 1);
}

void
edaddpv(PVPool *pvp, Cmdbuf *cb)		/* addpv pvname target */
{
	int t;
	uvlong len;

	if (waserror())
		return;
	t = parsetarget(cb->f[1]);
	if (t == -1)
		nexterror();	//parsetarget sets uerr
	len = targlenclean(t);
	if (len == 0)
		nexterror();	// targlen sets uerr
	poperror();
	xladdpv(t, len, pvp);
}

void
probetarg(Cmdbuf *cb)		/* probe target */
{
	uchar buf[Xblk];
	int t;
	ulong maxwait;
	vlong offset;

	t = parsetarget(cb->f[1]);
	if (t == -1)
		return;

	offset = strtoll(cb->f[2], nil, 0);
	maxwait = strtoul(cb->f[3], nil, 0);
	u->tag->maxwait = maxwait;

	xsyslog(LOGCOM "read %T offset %lld maxwait %uld\n",
		t, offset, maxwait);

	edread(t, buf, Xblk, offset);
}

void
clrpvmeta(Cmdbuf *cb)		/* clrpvmeta target */
{
	uchar buf[Xblk];
	int t, n;

	t = parsetarget(cb->f[1]);
	if (t == -1)
		return;

	rlock(&lk);
	n = targused(t);
	runlock(&lk);
	if (n != -1) {
		uerr("target %T already in use", t);
		return;
	}
	if (targlen(t) == 0)
		return;		// targlen sets uerr

	if (!ispvmeta(t)) {	/* sets uerr, but set a better one */
		uerr("PV %T no metadata detected\n", t);
		return;
	}
	memset(buf, 0, Xblk);
	if (edwrite(t, buf, Xblk, 0) != Xblk)
		uerr("cannot clear metadata from %T\n", t);
	else
		xsyslog("PV %T metadata cleared\n", t);
}

void
hasmeta(Cmdbuf *cb)	/* check if metadata exists on target */
{
	int t;

	t = parsetarget(cb->f[1]);
	if (t == -1) {
		uerr("invalid format for target %s ", cb->f[1]);
		return;
	}
	if (!ispvmeta(t))	/* sets uerr */
		return;
}

static int
cantruncate(PV *pv, int nexts)	/* called with pv locked */
{
	int i;

	for (i = pv->npve - 1; i > 0; i--) {
		if (REFCNT(pv->ref[i]))
			return 0;
		if (--nexts <= 0)
			return 1;
	}
	return 0;
}

static int
truncatepv(PV *pv, int nexts)	/* called with pv and pv->pool wlocked */
{
	if (!cantruncate(pv, nexts))
		return -1;

	pv->npve -= nexts;
	pv->length -= nexts * Xextent;
	xaddu(&pv->pool->etotal, - nexts);
	xaddu(&totalext, - nexts);
	xaddu(&pv->pool->efree, - nexts);

	return 0;
}

int
mendmirror(PV *pv)
{
	uchar buf[Xblk];
	ushort s;
	PV mpv;

	if (edio(pv->mirror, buf, Xblk, 0, OREAD) != Xblk) {
		xsyslog(LOGCOM "mirror %T target %T read failed\n",
			pv->targ, pv->mirror);
		uerr("mirror %T target %T read failed", pv->targ, pv->mirror);
		return -1;
	}
	if (memcmp(buf, VSPVMAGIC, 4) != 0) {
		xsyslog(LOGCOM "mendmirror %T bad magic\n", pv->mirror);
		return 0;
	}
	s = GBIT16(buf+4);
	buf[4] = buf[5] = 0;
	if (onesum(buf, Xblk) != s) {
		xsyslog(LOGCOM "mendmirror %T bad sum\n", pv->mirror);
		return 0;
	}
	memset(&mpv, 0, sizeof mpv);
	convM2PV(buf, buf + Xblk, &mpv);

	if (pv->ctime == mpv.ctime && pv->length == mpv.length
	    && pv->npve == mpv.npve
	    && ((pv->targ == mpv.targ && pv->mirror == mpv.mirror)
		||(pv->targ == mpv.mirror && pv->mirror == mpv.targ)))
	    return 1;

	xsyslog(LOGCOM "mendmirror %T bad meta %uld %lld %ud %T %T\n",
		pv->mirror, mpv.ctime, mpv.length, mpv.npve,
		mpv.targ, mpv.mirror);
	return 0;
}

void
mirror(PVPool *pvp, Cmdbuf *cb, int clean)	/* mirror pvname target */
{
	int t, mt, nexts, mend;
	PV *pv, opv;
	uvlong mlen, dlen;

	t = parsetarget(cb->f[1]);
	if (t < 0)
		return;		//parsetarget sets uerr
	mt = parsetarget(cb->f[2]);
	if (mt < 0)
		return;		// parsetarget sets uerr
	mlen = targlenclean(mt);
	if (mlen == 0)
		return;		// targlen sets uerr
	wlock(&lk);
	if (waserror()) {
		wunlock(&lk);
		return;
	}
	pv = targ2pv(mt);
	if (pv)
		if (pv->targ == mt)
			error("Mirror target %T already in use", mt);
		else if (pv->targ != t)
			error("Mirror target %T already used by %T",
			      mt, pv->targ);
	nexts = 0;
	wlock(pvp);		// for lookuppv() and to exclude moo()
	if (waserror()) {
		wunlock(pvp);
		nexterror();
	}
	pv = lookuppv(pvp, t);
	if (pv == nil) {
		error("PV target %T unknown", t);
	}
	if (pv->flags & PVFlost)
		error("PV target %T lost", t);
	opv = *pv;
	wlock(pv);
	if (waserror()) {
		wunlock(pv);
		nexterror();
	}
	mend = 0;
	switch (pv->state) {
	case PVSmissing:
		error("PV missing, can't mirror");
	case PVSbroke:
		if (pv->mirror == mt) {
			if (!(pv->flags & PVFfullsilver)) {
				mend = mendmirror(pv);
				if (mend < 0)
					nexterror();
			}
			break;
		}
	case PVSoosync:
	case PVSresilver:
	case PVSdouble:
		error("PV already mirrored; unmirror before remirror");
	case PVSsingle:
		break;
	}
	if (pv->length > mlen) {
		dlen = pv->length - mlen;
		if (dlen > 1000000000)
			error("Mirror %T smaller than PV %T by more than 1 GB",
			      mt, t);
		nexts = dlen/Xextent;
		if (dlen % Xextent)
			nexts++;

		if (truncatepv(pv, nexts) < 0)
			error("PV %T can't shrink %lld MB to match mirror %T",
			      t, nexts * Xextent / 1000000, mt);
	}
	pv->mirror = mt;
	pv->state = clean ? PVSdouble : PVSoosync;
	pv->sysalrt = 0;
	pv->flags &= ~(PVFabort|PVFuserbroke);
	if (!clean && !mend)
		pv->flags |= PVFfullsilver;

	if (updpv(pv) < 0) {
		pv->mirror = opv.mirror;
		pv->state = opv.state;
		pv->sysalrt = opv.sysalrt;
		pv->flags |= (opv.flags & PVFuserbroke);
		if (nexts) {
			pv->npve = opv.npve;
			pv->length = opv.length;
			xaddu(&pv->pool->etotal, nexts);
			xaddu(&totalext, nexts);
			xaddu(&pv->pool->efree, nexts);
		}
		error("PV metadata save failed");
	}
	if (nexts)
		xsyslog("PV %T shrank %lld MB to match mirror %T\n",
			t, nexts * Xextent / 1000000, mt);
	setpvtarget(mt, pv->sn[1]);
	wunlock(pv);
	poperror();
	wunlock(pvp);
	poperror();
	wunlock(&lk);
	poperror();
	mirrorck = 1;
	trestrict(mt);
	if (pv->flags & PVFfullsilver)
		xsyslog("PV %T mirrored to target %T full silver\n", t, mt);
	else
		xsyslog("PV %T mirrored to target %T partial silver\n", t, mt);
}

int
metaextents(int nx, int perblk)
{
	nx += perblk - 1;
	nx /= perblk;		// blocks of extents
	nx *= Xblk;		// represent x bytes on disk
	nx += Xblk;		// for metadata header
	nx += Xextent -1;	// round up to complete extent size
	nx /= Xextent;
	return nx;
}

/*
 * "Watch me pull a rabbit out of my hat! 
 *		-- Bullwinkle
 */
/*
 * The legacy data for the first PV metadata worth of extents is copied to another PV. 
 * This allows the beginning of the PV to hold PV metadata. Alloclvt is called with thin extents. 
 * The LV metadata is also stored on a different PV. Eventually the LVT will point to right extents.
 */
void
addleg(PVPool *pvp, Cmdbuf *cb)	 	/* nothing up my sleeve... */
{
	int t, nx, pvmx, i, mb, me, off, frstdat, n, doff;
	PV *pv;
	LV *lv;
	PVE pve;
	vlong length, offset;
	LVE *lve, *lvt;
	char *name, *targ, *err;
	char buf[Xblk];

	/* keeping the complexity of this puppy local to this function; mostly */

	name = cb->f[1];
	targ = cb->f[2];

	wlock(&lk);
/*1*/	if (waserror()) {
		wunlock(&lk);
		return;
	}

	/* validate arguments */

	lv = lookuplv(name);
	if (lv)
		error("LV %s already exists in pool %s", name, lv->pool->name);
	t = parsetarget(targ);
	if (t < 0)
		nexterror();	//parsetarget sets uerr
	length = targlenclean(t);
	if (length == 0)
		nexterror();	// targlen sets uerr
	if (targused(t) != -1)
		error("primary target %T already in use", t);
	
	/* figure out how many extents we need from pool */
	
	nx = (length+Xextent-1) / Xextent;	/* round up! */
	if (nx + totalext > MAXEXT)
		error("max vsx limited exceeded");
	frstdat = metaextents(nx, Xlveperblk);
	pvmx = metaextents(nx, Xrefperblk);
	/* This will hold displaced data */
	wlock(pvp);
	pve = newpve(pvp, nil, pvmx);
	wunlock(pvp);
	if (pve.pid == -1)
		error("failed to allocate metadata for pv");
	for (i=0; i<pvmx; i++)
		pvp->pv[pve.pid]->ref[pve.off+i] |= REFused|REFnf|1;
	pv = nil;
	lv = nil;
/*2*/	if (waserror()) {
		/* release metadata allocated for PV Metadata */
		for (i=0; i<pvmx; i++) {
			xincu(&pvp->efree);
			pvp->pv[pve.pid]->ref[pve.off+i] = 0;
		}
		freepv(pv);
		freelv(lv);
		nexterror();
	}
	pv = mallocz(sizeof *pv, 1);
	if (pv == nil)
		error("allocation failure for PV");
	pv->ref = vtalloc(nx, sizeof *pv->ref);
	pv->label = strdup("");
	if (pv->ref == nil || pv->label == nil)
		error("allocation failure for PV elements");

	lv = mallocz(sizeof *lv, 1);
	if (lv == nil)
		error("allocation failure for LV");
	lv->label = strdup("");
	lv->name = strdup(name);
	if (lv->label == nil || lv->name == nil)
		error("allocation failure for LV");
	pv->state = PVSsingle;
	pv->ctime = time(nil);
	pv->targ = t;
	pv->mirror = -1;
	pv->length = length;
	pv->offset = 0;
	pv->meta = pv;
	pv->mtarg = t;
	pv->npve = nx;
	pv->flags |= PVFpartial;

	/* Displace data to another PV, error out on first failure */
	for (i = 0; i < pvmx; i++) {
		if (pvxtntcopy(pvp->pv[pve.pid], pve.off + i, pv, i) < 0) {
			error("extent copy failure");
		}
	}
	if (initpvt(pv) < 0 || updpv(pv) < 0) {
		error("PV initialization failure");
	}
/*3*/	if (waserror()) {
		/* try to restore everything */
		for(i = 0; i < pvmx; i++) {
			if (pvxtntcopy(pv, i, pvp->pv[pve.pid], pve.off + i) < 0) {
				/* Sorry about your data... */
				xsyslog("legacy extent copy recovery error on %T off: %ud\n", pvp->pv[pve.pid]->targ, pve.off);
			}
		}
		nexterror();
	}
	/* mark every extent on the PV as used */
	for (i = 0; i < nx; i++)
		pv->ref[i] = REFused|REFnf|1;

	/* make the lv */
	lv->lun = -1;
	lv->length = length;
	lv->mode = LVREAD|LVWRITE;		/* read-write volume */
	lv->ctime = time(nil);
	lv->pool = pvp;
	lv->nqc = 0;
	lv->snaplimit = SLign;
	lv->nlve = frstdat+nx;
	lv->frstdat = frstdat;
	setserial(lv);

	if (err = alloclvt(lv, nil, nil, AThin | AClrDirty))	/* pretend its thin so we only allocate extents for meta data */
		error("failure allocating LV Table for legacy volume: %s", err);
	lv->thin = 0;		/* but they're not really thin */

	/* xladdtopool assigns pv->id.  One more thing to unroll on failure */
	wlock(pvp);
	wlock(pv);
	if (xladdtopool(pvp, pv) < 0) {	/* xladdtopool sets uerr */
		wunlock(pv);
		wunlock(pvp);
		nexterror();
	}
	wunlock(pv);
	wunlock(pvp);
	lv->exts[pv->id] = nx - pvmx;
/*4*/	if (waserror()) {
		xdecu(&pvp->npv);
		pvp->pv[pv->id] = nil;
		nexterror();
	}
	off = pvmx;
	doff = pve.off; /* Displaced data offset */
	lvt = (LVE *)buf + Xlveperblk;
	for (i = 0, mb = 1; i < lv->nlve; ++mb) {
		me = mb / Xblkperext;
		offset = lv->lve[me].off * Xextent + (mb % Xblkperext) * Xblk;
		for (lve = (LVE *)buf; i < lv->nlve && lve < lvt; ++i, ++lve) {
			if (i < lv->frstdat)
				fetchlve(lv, i, lve);
			else {
				 if (i < pvmx + frstdat) {
					lve->pid = pve.pid;
					lve->off = doff++;
					lv->exts[pve.pid]++;
				} else {
					lve->pid = pv->id;
					lve->off = off++;
				}
				lve->flag = LFdirty;	/* legacy is assumed all dirty */
				lv->dirty++;
			}
		}
		if (lv->lve[me].flag & LFthin)
			error("thin metadata lv on %s: %d\n", lv->name, me);
		n = pvio(lv->pool->pv[lv->lve[me].pid], buf, Xblk, offset, OWRITE);
		if (n != Xblk)
			error("alloclvt: failure writing lvt for %s", lv->name);
	}

	if (savemeta(lv, LVWIP) < 0)
		error("failure writing LV metadata");
	rlock(pvp);
	if (flushrefs(pvp) < 0) {
		runlock(pvp);
		error("failure writing pool PV Table state");
	}
	runlock(pvp);
	if (savemeta(lv, 0) < 0)
		error("failure updating LV metadata");
	lv->next = vols;
	vols = lv;
	xaddu(&pvp->etotal, nx); /* not free because they are spoken for */
	xaddu(&totalext, nx);
/*4*/	poperror();
/*3*/	poperror();
/*2*/	poperror();
/*1*/	poperror();
	setpvtarget(t, pv->sn[0]);
	wunlock(&lk);
	newpv(pv);
	newlv(lv);
	trestrict(t);
	writeconfig();
	xsyslog("LV %s created in pool %s from legacy LUN %T\n", lv->name, pvp->name, t);
	
	/*
	 * "Oh, Bullwinkle, that trick never works!"
	 *		-- Rocky the Squirrel
	 */
}

PV *
targ2pv(int t)	/* called with lk locked */
{
	PVPool *pvp;
	PV *pv;
	int i;

	for (pvp = pools; pvp; pvp = pvp->next) {
		rlock(pvp);
		for (i = 0; i < Npvsperpool; i++) {
			if (pv = pvp->pv[i])
			if (pv->targ == t || pv->mirror == t) {
				runlock(pvp);
				return pv;
			}
		}
		runlock(pvp);
	}
	return nil;
}

int
targused(int t)		/* return targ if true, otherwise -1, called with lk locked */
{
	PV *pv;
	pv = targ2pv(t);
	if (pv)
		return pv->targ;
	return -1;
}

/* enters with pv lock held */
int
stopsilver(PV *pv)
{
	int i;

	pv->flags |= PVFabort;
	wunlock(pv);
	for (i=0; i<50; i++) {
		if ((pv->flags & PVFsilvering) == 0) {
			wlock(pv);
			return 0;
		}
		sleep(100);
	}
	wlock(pv);
	return -1;
}

void
mpromote(PVPool *pvp, char *targ)
{
	int t, i, m;
	PV *pv, opv;

	t = parsetarget(targ);
	if (t == -1)
		return;		//parsetarget sets uerr
	rlock(pvp);
	if (waserror()) {
		runlock(pvp);
		return;
	}
	pv = nil;
	for (i = 0; i < Npvsperpool; i++) {
		pv = pvp->pv[i];
		if (pv && pv->mirror == t)
			break;
	}

	if (i >= Npvsperpool || pv == nil)
		error("PV mirror element %T not found in pool %s", t, pvp->name);
	switch (pv->state) {
	default:
		error("PV state %d unknown", pv->state);
	case PVSmissing:
		error("cannot promote %T; %T is missing", t, pv->targ);
	case PVSsingle:
		error("PV %T is not mirrored", pv->targ);
	case PVSoosync:
	case PVSbroke:
	case PVSresilver:
		error("cannot promote a broken PV %T", t);
	case PVSdouble:
		wlock(pv);
		opv = *pv;
		pv->mirror = m = pv->targ;
		pv->targ = t;
		if (updpv(pv) < 0) {
			pv->mirror = opv.mirror;
			pv->targ = opv.targ;
			wunlock(pv);
			error("PV metadata save failed");
		}
		memcpy(pv->sn[0], opv.sn[1], Nserial);
		memcpy(pv->sn[1], opv.sn[0], Nserial);
		wunlock(pv);
	}
	poperror();
	runlock(pvp);
	writeconfig();
	xsyslog("PV %T element %T promoted to primary\n", m, t);
}

int
brkmirror(PV *pv)	/* stop silvering and unassociate the mirror */
{
	uchar buf[Xblk];
	int state;
	PV opv;

	wlock(pv);
	opv = *pv;
	if (waserror()) {
		wunlock(pv);
		return -1;
	}
	memset(buf, 0, sizeof buf);
	state = pv->state;
	switch (state) {
	case PVSdouble:
		edio(pv->mirror, buf, Xblk, 0, OWRITE);
	case PVSmissing:
		break;
	case PVSbroke:
	case PVSoosync:
	case PVSresilver:
		if (stopsilver(pv) < 0)
			error("cannot halt silver process");
		if (state != PVSbroke)
			edio(pv->mirror, buf, Xblk, 0, OWRITE);
		break;
	default:
		error("PV %T is not mirrored", pv->targ);
	}
	pv->state = PVSsingle;
	pv->mirror = -1;
	memset(pv->sn[1], 0, Nserial);
	if (updpv(pv) < 0) {
		pv->state = opv.state;
		pv->mirror = opv.mirror;
		memcpy(pv->sn[1], opv.sn[1], Nserial);
		error("PV metadata save failed");
	}
	poperror();
	wunlock(pv);
	clrpvtarget(opv.mirror);
	return 0;
}

void
unmirror(PVPool *pvp, char *targ)	/* unmirror PV */
{
	PV *pv;
	int t, m;

	rlock(pvp);
	if (waserror()) {
		runlock(pvp);
		return;
	}
	t = parsetarget(targ);
	if (t == -1)
		nexterror();
	pv = lookuppv(pvp, t);
	if (pv == nil)
		error("PV %T not found in pool %s", t, pvp->name);
	if (pv->flags & PVFlost)
		error("PV %T lost", t);
	poperror();
	runlock(pvp);
	m = pv->mirror;
	if (brkmirror(pv) < 0)
		return;
	tunrestrict(m);
	writeconfig();
	xsyslog("PV %T unmirrored from target %T\n", t, m);
}

void
breakmirror(PVPool *pvp, char *targ)	/* brkmirror PV */
{
	PV *pv;
	int t;

	t = parsetarget(targ);
	if (t == -1)
		return;

	rlock(pvp);
	if (waserror()) {
		runlock(pvp);
		return;
	}
	pv = lookuppv(pvp, t);
	if (pv == nil)
		error("PV %T not found in pool %s", t, pvp->name);
	wlock(pv);
	if (waserror()) {
		wunlock(pv);
		nexterror();
	}
	if (pv->state != PVSdouble)
		error("%T is not a complete mirror", pv->targ);

	pv->state = PVSbroke;
	pv->flags |= PVFuserbroke;
	if (updpv(pv) < 0) {
		pv->state = PVSdouble;
		pv->flags &= ~PVFuserbroke;
		error("PV metadata save failed");
	}
	poperror();
	wunlock(pv);
	poperror();
	runlock(pvp);
	xsyslog("PV %T: broken from target %T\n", t, pv->mirror);
}
	
/*
 * At this point we have a request that is <= 16 sectors
 */

long 
edread(int t, void *a, long count, vlong offset)
{
	return edio(t, a, count, offset, OREAD);
}

long
edwrite(int t, void *a, long count, vlong offset)
{
	return edio(t, a, count, offset, OWRITE);
}

long
edio(int t, void *a, long count, vlong offset, int rw)
{
	Msg *m, *n;
	AoeDisk *ad;
	Tag *tg;

	u->err = nil;
	if (offset & 0x1ff)
		uerr("edio: offset %lld not on sector boundary", offset);
	if (count & 0x1ff)
		uerr("edio: count %ld not a mutiple of a sector", count);
	if (count > 8192)
		u->err = "edio: count too large";
	if (u->err) {
		xsyslog("%s\n", u->err);
		return 0;
	}

	tg = u->tag;
	if (u->tag == nil) 
		print("%s:%d null tg\n", u->name, u->pid);
	m = aoerwmsg(t, a, count, offset, rw);
	if (m == nil) {
		u->err = "edio: failed msg allocation";
		return 0;
	}
	if (u->debug)
		aoeshow(m, "edio");
	n = tagsend(tg, m);
	msgfree(m);
	if (n == nil) {
		u->err = "failed request";
//**/		print("edio: a=%p count=%ld offset=%,lld tag=%08ulx: %s\n", a, count, offset, tg->tag, u->err);
		return 0;
	}
	ad = (AoeDisk *)n->data;
	if (isresperror(ad)) {
		xsyslog("%r %T %ld %lld %x\n", t, count, offset, rw);
		msgfree(n);
		return 0;
	}
	if (rw == OREAD)
		memmove(a, ad->data, count);
	msgfree(n);
	return count;
}

static int
rdtargsn(void *va, void *a, int count, vlong offset)
{
	char buf[8192];
	int targ, i;
	Target *t;

	targ = (int) va;
	t = lookuptarg(targ);
	if (t == nil)
		return 0;
	qlock(t);
	buf[0] = '\0';
	if (t->serial[0].stime) {
		i = sncpynl(buf, t->serial[0].sn);
		if (t->serial[1].stime)
			sncpynl(buf + i, t->serial[1].sn);
	}
	qunlock(t);
	reletarg(t);
	return readstr(offset, a, count, buf);
}

static void
resettargx(Target *t)
{
	t->srtt = Rttavginit;
	t->sdev = Rttdevinit;
	t->ntmac = t->lasttmac = t->tmacver = 0;
	memset(t->tmac, 0, sizeof t->tmac);
}

static int
wrtargsn(void *va, void *a, int count, vlong)
{
	int targ;
	Target *t;
	ushort sn[Nserial/2];
	PV *pv;

	if (count < 20) {
		uerr("not enough characters in serial number");
		return 0;
	}
	targ = (int) va;
	t = lookuptarg(targ);
	if (t == nil)
		return 0;
	qlock(t);
	setfld(sn, 0, Nserial, a);
	if (memcmp(sn, t->serial[0].sn, Nserial) == 0
	    || memcmp(sn, t->serial[1].sn, Nserial) == 0) {
		rlock(&lk);
		pv = targ2pv(t->target);
		runlock(&lk);
		if (pv) {
			t->flags |= (TARGpv|TARGckd);
			wlock(pv);
			if (t->target == pv->targ)
				memcpy(pv->sn[0], sn, Nserial);
			else
				memcpy(pv->sn[1], sn, Nserial);
			wunlock(pv);
			writeconfig();
			xsyslog("PV target %T set serial %.*s\n",
				t->target, Nserial, a);
			memcpy(t->serial[0].sn, sn, Nserial);
			t->serial[0].stime = time(nil);
			t->serial[1].stime = 0;
			resettargx(t);
			discovertarg(targ);
		}
	} else {
		uerr("unknown serial number");
		count = 0;
	}
	qunlock(t);
	reletarg(t);
	return count;
}

static void
resettarg(Target *t)
{
	qlock(t);
	resettargx(t);
	qunlock(t);
}

static Target *
newtarg(int target)
{
	Target *t;
	static int nt;

	t = mallocz(sizeof *t, 1);
	if (t == nil)
		goto e;
	t->target = target;
	t->name = smprint("%T", target);
	if (t->name == nil) {
		free(t);
		goto e;
	}
	t->dir = newdir(targdir, t->name, 0777);
	if (t->dir == nil) {
		free(t->name);
		free(t);
		goto e;
	}
	newfile(t->dir, "target", 0644, 0, rdtarg, nowrite, (void *) target);
	newfile(t->dir, "paths", 0644, 0, rdpaths, nowrite, (void *) target);
	newfile(t->dir, "ident", 0644, 0, rdident, nowrite, (void *) target);
	newfile(t->dir, "config", 0644, 0, rdconf, nowrite, (void *) target);
	newfile(t->dir, "rtt", 0644, 0, rdrtt, nowrite, (void *) target);
	newfile(t->dir, "serial", 0666, 0, rdtargsn, wrtargsn, (void *) target);
	resettarg(t);

	return t;

e:	if (nt < 10) {
		xsyslog("newtarg: allocation failure for target %T\n", target);
		nt++;
	}
	return nil;
}

static Target *
clookuptarg(int target, int create)
{
	Target *t, **tt;
	uint h;

	qlock(&targetlk);
	h = target % Nthash;
	tt = targs + h;
	for (; t = *tt; tt = &t->next)
		if (t->target == target) {
			*tt = t->next;
			goto hol;
		}
	if (t == nil && create) {
		t = newtarg(target);
		if (t != nil) {
hol:			t->next = targs[h];
			targs[h] = t;
			t->ref++;
		}
	}
	qunlock(&targetlk);
	return t;
}

Target *
lookuptarg(int target)
{
	return clookuptarg(target, 0);
}

uvlong
targlen(int target)
{
	Target *t;
	uvlong len;

	len = 0;
	if (t = lookuptarg(target)) {
		len = t->length;
		reletarg(t);
	}
	if (t == nil || len == 0)
		uerr("target %T is currently not available", target); 
	return len;
}

uvlong
targlenclean(int target)
{
	Target *t;
	uvlong len;
	int twotiming;

	len = 0;
	twotiming = 0;
	if (t = lookuptarg(target)) {
		len = t->length;
		twotiming = t->serial[1].stime;
		reletarg(t);
	}
	if (len == 0) {
		uerr("target %T is currently not available", target);
	} else if (twotiming) {
		uerr("target %T has more than one serial number. Contact Coraid TAC.",
		     target);
		len = 0;
	}
	return len;
}

int asd;
#define asdprint if(!asd) USED(asd); else print

static int missing, assymirrors;

/* wakeup lazy assembler */

void
kickass(void)
{
	qlock(&qassy);
	missing = 1;
	rwakeup(&assy);
	qunlock(&qassy);
}

static uint
mirrorclean(PV *pv)
{
	uint n, t;

	for (n = 0, t = 0; n < pv->npve; ++n) {
		if (pv->ref[n] & REFdirty) {
			pv->ref[n] &= ~REFdirty;
			pv->ref[n] |= REFnf;
			++t;
		}
	}
	return t;
}

static void
mirrorupgradedouble(PV *pv)
{
	uint t;

	wlock(pv);
	t = mirrorclean(pv);
	pv->flags &= ~PVFfullsilver;
	updpv(pv);
	wunlock(pv);
	if (t == 0) {
		xsyslog(LOGCOM "mirror clean %T nop\n", pv->targ);
		return;
	}
	if (flushpv(pv) < 0) {
		xsyslog(LOGCOM "mirror clean %T flush failed\n", pv->targ);
	} else {
		xsyslog(LOGCOM "mirror clean %T\n", pv->targ);
	}
}

static void
mirrorupgradebroke(PV *pv)
{
	int r;

	wlock(pv);
	pv->flags |= PVFfullsilver;
	r = updpv(pv);
	wunlock(pv);
	if (r < 0)
		xsyslog(LOGCOM "mirror broke set full silver failed %T\n",
			pv->targ);
	else
		xsyslog(LOGCOM "mirror broke set full silver %T\n",
			pv->targ);
}

static void
mirrorupgrade(void)
{
	PVPool *pvp;
	PV *pv;
	uint i;

	if (releaselast == nil)	// no upgrade
		return;

	// on upgrade from 1.x.x releaselast is "unknown"

	if (strcmp(releaselast, "unknown") != 0)
		return;

	// remove dirty bits from 1.x.x mirrors

	rlock(&lk);
	for (pvp = pools; pvp; pvp = pvp->next) {
		wlock(pvp);
		for (i=0; i<Npvsperpool; i++) {
			pv = pvp->pv[i];
			if (pv == nil)
				continue;
			switch (pv->state) {
			case PVSdouble:
				mirrorupgradedouble(pv);
				break;
			case PVSbroke:
				mirrorupgradebroke(pv);
				break;
			}
		}
		wunlock(pvp);
	}
	runlock(&lk);
}

static
void
tendfound(void)
{
	PVPool *pvp;
	PV *pv;
	LV *lv;
	int i, lvck;
	uvlong t, m;

	qlock(&lostlk);
	for(;;) {
		rsleep(&rfound);
		if (shutdown) {
			qunlock(&lostlk);
			xlexits(0);
		}
		rlock(&lk);
		lvck = 0;
		for(pvp = pools; pvp; pvp = pvp->next) {
			rlock(pvp);
			for (i = 0; i < Npvsperpool; i++) {
				pv = pvp->pv[i];
				if (pv == nil || pv->state == PVSmissing)
					continue;
				wlock(pv);
				if (pv->flags & PVFlost) {
					t = targlen(pv->targ);
					m = 0;
					switch(pv->state) {
						case PVSdouble:
							m = targlen(pv->mirror);
						default:
							if (t || m) {
								xsyslog("PV %T found\n", pv->targ);
								pv->flags &= ~PVFlost;
								lostpvcnt--;
								lvck++;
							}
							break;
					}
				}
				wunlock(pv);
			}
			runlock(pvp);
		}
		if (lvck) {
			for (lv = vols; lv; lv = lv->next) {
				wlock(lv);
				if (lv->flags & LVFsuspended) {
					for (i = 0; i < Npvsperpool; i++) {
						if ((pv = lv->pool->pv[i]) && lv->exts[i])
							if (pv->flags & PVFlost)
								goto e;
					}
					lv->flags &= ~LVFsuspended;
					if ((lv->mode & LVSNAP) == 0)
						xsyslog("LV %s: state healthy\n", lv->name);
				}
e:				wunlock(lv);
			}
		}
		runlock(&lk);
	}
}

static
void
tendlost(void)
{
	PVPool *pvp;
	PV *pv;
	LV *lv;
	int i, lvck;
	uvlong t, m;
	
	qlock(&lostlk);
	for (;;) {
		rsleep(&rlost);
		if (shutdown) {
			qunlock(&lostlk);
			xlexits(0);
		}
		if (lostck == 0)
			continue;
		rlock(&lk);
		lostck = 0;
		lvck = 0;
		for (pvp = pools; pvp; pvp = pvp->next) {
			rlock(pvp);
			for (i = 0; i < Npvsperpool; i++) {
				pv = pvp->pv[i];
				if (pv == nil || pv->state == PVSmissing)
					continue;
				wlock(pv);
				if (pv->flags & PVFlost) {
					wunlock(pv);
					continue;
				}
				t = targlen(pv->targ);
				switch(pv->state) {
				case PVSdouble:
					m = targlen(pv->mirror);
					if (t == 0 && m == 0) {
						pv->flags |= PVFlost;
						lostpvcnt++;
						lvck++;
						xsyslog("PV %T has been lost\n", pv->targ);
					} else if (t == 0) {
						xsyslog("AoE target %T lost, breaking mirror\n", pv->targ);
						failmir(pv, pv->targ, 1);
					} else if (m == 0) {
						xsyslog("AoE target %T lost, breaking mirror\n", pv->mirror);
						failmir(pv, pv->mirror, 1);
					}
					break;
				default:
					if (t == 0) {
						pv->flags |= PVFlost;
						lostpvcnt++;
						lvck++;
						xsyslog("PV %T lost\n", pv->targ);
					}
					break;
				}
				wunlock(pv);
				
			}
			runlock(pvp);
		}
		if (lvck) {
			for (lv = vols; lv; lv = lv->next) {
				wlock(lv);
				if ((lv->flags & LVFsuspended) == 0) 
					for (i = 0; i < Npvsperpool; i++) {
						if ((pv = lv->pool->pv[i]) && lv->exts[i]) {
							if (pv->flags & PVFlost) {
								lv->flags |= LVFsuspended;
								if ((lv->mode & LVSNAP) == 0)
									xsyslog("LV %s: state suspended\n", lv->name);
								break;
							}
						}
					}
				wunlock(lv);
			}
		}
		runlock(&lk);
	}
}

static uvlong
targlensnck(Target *t, char *pvsn) // called with t locked
{
	char tsn[2][Nserial+1];

	if (!t->serial[0].stime)
		return 0;

	if (t->serial[1].stime) {
		sncpy(tsn[0], t->serial[0].sn);
		sncpy(tsn[1], t->serial[1].sn);
		xsyslog("PV target %T has serial numbers %s and %s\n",
			t->target, tsn[0], tsn[1]);
		return 0;
	}
	if ((time(nil) - t->serial[0].stime) > 45) {
		t->flags |= (TARGpv|TARGckd);
		memcpy(pvsn, t->serial[0].sn, Nserial);
		_xinc(&snsaveconfig);
		sncpy(tsn[0], pvsn);
		xsyslog(LOGCOM "setsnpv %T %s\n", t->target, tsn[0]);
		return t->length;
	}
	return 0;
}

static uvlong
targlensn(int target, char *pvsn)
{
	Target *t;
	uvlong len;

	len = 0;
	if (t = lookuptarg(target)) {
		if (pvsn[0]) {
			len = t->length;
		} else {
			qlock(t);
			len = targlensnck(t, pvsn);
			qunlock(t);
		}
		reletarg(t);
	}
	return len;
}

static void
missinglog(PV *pv, int targ, int len, int fmissing, int flogged, char *msg)
{
	if (len) {
		if (pv->flags & flogged)
			xsyslog("PV %s %T found after missing\n",
				msg, targ);
		pv->flags &= ~flogged;
	} else
		pv->flags |= fmissing;
}

static
char *
harole(void)
{
	char b[32];

	if (readfile(b, sizeof b, "/n/kfs/conf/ha/role") < 0)
		return "Unknown";

	return *b == 'p' ? "Primary" : "Secondary";
}

static
void
assembler(void)
{
	PVPool *pvp;
	PV *pv;
	int ftarg, new;
	int i;
	uvlong t, mt;
	static int legck;

	qlock(&qassy);
	missing = 1;	/* assume there's at least one first time */
	for (;;) {
		rsleep(&assy);
		if (shutdown) {
			qunlock(&qassy);
			xlexits(0);
		}
		if (missing == 0 || assembleck == 0) {
			if (assymirrors) {
				assymirrors = 0;
				mirrorupgrade();
			}
			continue;
		}
		rlock(&lk);
		missing = 0;
		for (pvp = pools; pvp; pvp = pvp->next) {
			new = 0;
			/* sweep the pool, installing pvs that have come online */
			for (i=0; i<Npvsperpool; i++) {
				pv = pvp->pv[i];
				if (pv == nil || pv->state != PVSmissing)
					continue;
				missing++;
				wlock(pv);
				t = targlensn(pv->mtarg, pv->sn[0]);
				missinglog(pv, pv->mtarg, t, PVFmissp,
					   PVFmissplgd, "target");
				/* if there's no mirror, just try to load it */
				if (pv->mirror == -1) {
					if (t) {
						asdprint("assembler: nomirror, loading %T\n", pv->mtarg);
						new += xlloadpv(pv, 1);
					}
					wunlock(pv);
					continue;
				}
				mt = targlensn(pv->mirror, pv->sn[1]);
				missinglog(pv, pv->mirror, mt, PVFmissm,
					   PVFmissmlgd, "mirror target");
				/* if neither is online, ignore */
				if (!t && !mt) {
					wunlock(pv);
					continue;
				}
				/* hey, they're both here! */
				if (t && mt) {
					asdprint("assembler: mirload\n");
					new += xlloadpv(pv, 1);
					wunlock(pv);
					assymirrors++;
					continue;
				}
				/* ok, one is available, start the 2m counter. */
				if (pv->mirwait == 0) {
					pv->mirwait = ticks;
					wunlock(pv);
					continue;
				}
				asdprint("assembler: mirrorwait %lld\n", ticks-pv->mirwait);
				/* ticks are in ms */
				if (ticks - pv->mirwait < 1000 * 60 * 2) {
					wunlock(pv);
					continue;
				}
				pv->mirwait = ticks;
				ftarg = pv->mirror;
				if (mt) {
					ftarg = pv->mtarg;
					pv->mtarg = pv->mirror;
				}
				USED(ftarg);	// shup, compiler.
				if (xlloadpv(pv, 0)) {
					if (pv->state != PVSdouble)
					if (ftarg == pv->targ) {
						xsyslog("Cannot bring PV %T online - mirror not in sync and only available element is un-synced secondary\n", pv->targ);
						pv->state = PVSmissing;
						pv->sysalrt = 1;
						pv->mtarg = ftarg;
						wunlock(pv);
						continue;
					}	
					asdprint("assembler: loadwfail\n");
					failmir(pv, ftarg, 1);	// XXX fail?
					wunlock(pv);
					new += xlloadpvt(pv);
				}
				else
					wunlock(pv);
			}
			if (new == 0) {
				continue;
			}
			xlcheck4lvs(pvp);
			
		}
		runlock(&lk);
		if (!missing)
			xsyslog("HA %s Node LV discovery complete\n", harole());
	}
}

static ulong
rttdelta(ulong sent)
{
	ulong rtt;

	rtt = ticks - sent;
	if (rtt == 0)
		rtt++;
	return rtt;
}

static void
rttavg(Target *t, ulong rtt)
{
	int n;

	n = rtt;
	n -= t->srtt >> Rttscale;
	t->srtt += n;
	if (n < 0)
		n = -n;
	n -= t->sdev >> Rttdscale;
	t->sdev += n;
}

static ulong
rttrto(Target *t)
{
	ulong rto;

	rto = 2 * (t->srtt >> Rttscale);
	rto += 8 * (t->sdev >> Rttdscale);
	rto++;
	return rto;
}

/* Soli Deo Gloria */
/* Brantley Coile */
