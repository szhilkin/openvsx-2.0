#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <ip.h>
#include <ctype.h>
#include <libcutil.h>
#include "dat.h"
#include "fns.h"
#include "alib.h"

enum {
	CMbind = 0,		/* bind device to target */
	CMtrace,
	CMdiscover,
	CMreset,
	CMflush,
	CMrevalidate,
};

enum {
	NOFORCE,
	FORCE
};

static int canredirect(LV *, Msg *, int);

int	targctl(void *, void *, int, vlong);
int	rdshelf(void *, void *, int, vlong);
int	wrshelf(void *, void *, int, vlong);
int	rdstats(void *, void *, int, vlong);
int	rdinits(void *, void *, int, vlong);
int	rdea(void *, void *, int, vlong);
int	rdpeerea(void *, void *, int, vlong);
int	wrpeerea(void *, void *, int, vlong);
int	rdports(void *, void *, int, vlong);
static int	etheropen(char *);
int	loadrrstate(LV *);

void	tdisk(LV *, Msg *, Aoe *);
void	tqc(LV *, Msg *, Aoe *);
void	tmask(LV *, Msg *, Aoe *);
void	tres(LV *, Msg *, Aoe *);
void	tkrr(LV *, Msg *, Aoe *);

void	noteinit(Aoe*);

static	void	bindports(void);


static
Cmdtab	ctltab[] = {
	{ CMbind, 	"bind",		2 },	/* bind /net/ether0 */
	{ CMtrace,	"trace",	2 },	/* trace [on|off] */
	{ CMdiscover, 	"discover",	0 }, 	/* discover */
	{ CMreset, 	"reset",	0 }, 	/* reset */
	{ CMflush, 	"flush",	0 }, 	/* flush */
	{ CMrevalidate, "revalidate",	0 }, 	/* reset and disocver */
};

Inits	inits[Ninits];
int	ninits;
QLock	initslk;
int	shelf = -1;
uchar 	bcast[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

enum {
	Nioq= 65536,
};

struct {
	Msg 	*hd;
	Msg	*tl;
	int	nq;
	int	nqmax;
	Rendez;
	QLock;
} ioq;

int *nqp = &ioq.nq, *nqmaxp = &ioq.nqmax;

void	(*proto[])(LV *, Msg *, Aoe *) = 
{
	[ACATA] tdisk,
	[ACQC]	tqc,
	[ACMASK]tmask,
	[ACRES]	tres,
	[ACKRR]	tkrr,
	[AVmax]	nil,
};

static QLock portslk;

static void
portupdate(int i, int mbps)
{
	int ombps;
	char *igmsg = "detects link speed below 1000 Mbps, ignoring packets",
	       *msg = "detects link speed at or above 1000 Mbps, processing packets";

	if (mbps == sanports[i].mbps) {
		if (sanports[i].alrt && mbps < 1000 && needalrtsec(sanports[i].alrt++)) {
			if (mbps)
				xsyslog("%s %s\n", sanports[i].name, igmsg);
			else
				xsyslog("%s link down\n", sanports[i].name);
		}
		return;
	}
	sanports[i].alrt = 1;

	qlock(&portslk);
	ombps = sanports[i].mbps;
	sanports[i].mbps = mbps;
	qunlock(&portslk);

	if (ombps == 0 && mbps > 0)
		xsyslog("%s: link up\n", sanports[i].name);
	else if (ombps > 0 && mbps == 0)
		xsyslog("%s: link down\n", sanports[i].name);

	if (ombps < 1000 && mbps >= 1000)
		xsyslog("%s: %s\n", sanports[i].name, msg);
	else if (ombps >= 1000 && mbps < 1000)
		xsyslog("%s: %s\n", sanports[i].name, igmsg);
}

static void
portmonitor(void)
{
	int i, n;
	char b[512], *l;

loop:
	for (i = 0; i < nsanports; ++i) {
		n = readfile(b, sizeof b, "/net/%s/stats", sanports[i].name);
		if (n > 0) {
			b[n] = '\0';
			l = strstr(b, "link: ");
			if (l) {
				l = strchr(l, ' ');
				portupdate(i, atoi(l));
			}
		}
	}
	if (shutdown)
		xlexits(0);
	sleep(1*1000);
	goto loop;
}

void
targinit(void)
{
	int i;

	fmtinstall('E', eipfmt);
	newfile(targdir, "ctl", 0666, 0, noread, targctl, 0);
	newfile(targdir, "shelf", 0666, 0, rdshelf, wrshelf, 0);
	newfile(targdir, "stats", 0444, 0, rdstats, nowrite, 0);
	newfile(targdir, "initiators", 0444, 0, rdinits, nowrite, 0);
	newfile(targdir, "myea", 0666, 0, rdea, nowrite, 0);
	newfile(targdir, "peerea", 0666, 0, rdpeerea, wrpeerea, 0);
	newfile(targdir, "ports", 0444, 0, rdports, nowrite, 0);
	phyinit();
	ioq.l = &ioq;
	for (i = 0; i < Ntargprocs; i++)
		if (xlrfork(RFPROC|RFMEM, "serve") == 0)
			serve();
	if (xlrfork(RFPROC|RFMEM, "ticks") == 0)
		for (;;) {
			sleep(1);
			ticks++;
		}
	bindports();
	if (xlrfork(RFPROC|RFMEM, "portmonitor") == 0)
		portmonitor();
};

static void
portadd(int fd, char *name)
{
	uchar ea[6];
	char *p;
	int i;

	if (myetheraddr(ea, name) == 0)
		insea(ea);
	p = strrchr(name, '/');
	if (p == nil)
		p = name;
	else
		p++;
	qlock(&portslk);
	if (nsanports >= Nsanports) {
		qunlock(&portslk);
		xsyslog("portadd: out of net structures\n");
		return;
	}
	i = nsanports;
	sanports[i].fd = fd;
	strcpy(sanports[i].name, p);
	nsanports++;
	qunlock(&portslk);
	if (xlrfork(RFPROC|RFMEM, "netread") == 0)
		netread(i);
	if (xlrfork(RFPROC|RFMEM, "discover") == 0)
		discover(i);
}

static void
bindports(void)
{
	int i;
	int fd;
	char buf[32];

	for (i=2; i<Nsanports; i++) {
		snprint(buf, sizeof buf, "ether%d", i);
		fd = etheropen(buf);
		if (fd < 0)
			continue;
		portadd(fd, buf);
	}
}

static int
etheropen(char *p)
{
	char buf[64];
	int fd, cfd;

	snprint(buf, sizeof buf, "%s!0x88a2", p);
	fd = dial(buf, 0, 0, &cfd);
	if (fd < 0)
		return fd;
	if (waserror()) {
		xsyslog("%s\n", u->errstr);
		close(fd);
		close(cfd);
		return -1;
	}
	if (fprint(cfd, "mtu 9000")  < 0)
		error("failed to set mtu 9000 on %s: %r", p);
	if (fprint(cfd, "qnolimit") < 0)
		error("failed to set qnolimit on %s: %r", p);
	poperror();
	close(cfd);
	return fd;
}

int
revalidtarg(char *target)
{
	int t;

	t = parsetargetbc(target);
	if (t < 0)
		return -1;	//parsetargetbc sets uerr

	if (flush(t, FORCE) < 0)
		return -1;	//flushtarg sets uerr

	return discovertarg(t);	//discovertarg sets uerr
}

static int
targctlx(void *a, int count)
{
	Cmdbuf *cb;
	Cmdtab *ct;
	int fd;
	
	cb = parsecmd((char *)a, count);
	ct = lookupcmd(cb, ctltab, nelem(ctltab));
	if (ct == nil) {
		u->err = "unknown ctl command";
		free(cb);
		return 0;
	}
	switch (ct->index) {
	case CMtrace:
		if (strcmp(cb->f[1], "on") == 0)
			tracing = 1;
		else if (strcmp(cb->f[1], "off") == 0)
			tracing = 0;
		break;
	case CMbind:
		fd = etheropen(cb->f[1]);
		if (fd < 0) {
			xsyslog("etheropen failure: %r\n");
			u->err = "bind failed";
			free(cb);
			return 0;
		}
		portadd(fd, cb->f[1]);
		break;
	case CMdiscover:
		switch (cb->nf) {
		case 1:
			fdiscover();
			break;
		case 2:
			if (discovertarg(parsetargetbc(cb->f[1])) < 0)
				count = 0;	//discovertarg and parsetargetbc sets uerr
			break;
		default:
			u->err = "usage: discover [ target ]";
			count = 0;
		}
		break;
	case CMreset:
		switch (cb->nf) {
		case 1:
			flushshelf(0xffffff, FORCE);
			break;
		case 2:
			if (flush(parsetargetbc(cb->f[1]), FORCE) < 0)
				count = 0;	//parsetargetbc sets uerr
			break;
		default:
			u->err = "usage: reset [ target ]";
			count = 0;
		}
		break;
	case CMflush:
		switch (cb->nf) {
		case 1:
			flushshelf(0xffffff, NOFORCE);
			xsyslog("Flushed all stale AoE targets\n");
			break;
		case 2:
			if (flush(parsetargetbc(cb->f[1]), NOFORCE) < 0)
				count = 0;	//flushtarget and parsetargetbc sets uerr
			else
				xsyslog("Flushed stale AoE target: %s\n",cb->f[1]);
			break;
		default:
			u->err = "usage: flush [ target ]";
			count = 0;
		}
		break;
	case CMrevalidate:
		switch (cb->nf) {
		case 1:
			flushshelf(0xffffff, FORCE);
			fdiscover();
			break;
		case 2:
			if (revalidtarg(cb->f[1]) < 0)
				count = 0;	//validtarg sets uerr
			break;
		default:
			u->err = "usage: revalidate [ target ]";
			count = 0;
		}
		break;
	}
	free(cb);
	return count;
}

int
targctl(void *, void *a, int count, vlong)
{
	int ret;

	xsyslog(LOGCOM "targ/ctl %.*s\n", endspace(a, count), a);
	ret = targctlx(a, count);
	if (u->err)
		xsyslog(LOGCOM "targ/ctl error %s\n", u->err);
	return ret;
}

void
insea(uchar *ea)	/* make sure ea is in the table */
{
	int i;
	
	wlock(&myealck);
	for (i = 0; i < myeaindx; i++) {
		if (memcmp(&myea[i*6], ea, 6) == 0) {
			wunlock(&myealck);
			return;	/* it's there already */
		}
	}
	if (myeaindx < Nmyea)
		memmove(&myea[6*myeaindx++], ea, 6);
	wunlock(&myealck);
}

int
isours(uchar *ea)	/* true if this is one of our macs */
{
	int i;
	
	rlock(&myealck);
	for (i = 0; i < myeaindx; i++)
		if (memcmp(ea, &myea[i*6], 6) == 0) {
			runlock(&myealck);
			return 1;
		}
	runlock(&myealck);
	return 0;
}
	
int
rdshelf(void *, void *a, int count, vlong off)
{
	if (shelf == -1)
		return readstr(off, a, count, "unset");
	
	return readnum(off, a, count, shelf, 10);
}

int
wrshelf(void *, void *a, int len, vlong)
{
	int i, n, s;
	char buf[16];
	char *ebad = "bad shelf value, use [0 - 65504]", *e;

	rlock(&lk);
	if (waserror()) {
		runlock(&lk);
		return 0;
	}
	if (shelf != -1) {
		n = 0;
		for (i = 0; i < nelem(luns); i++)
			if (luns[i])
			if (luns[i]->mode & LVONL)
				n++;
		if (n)
			error("can't change shelf, %d luns online", n);
	}
	if (len > sizeof buf - 1 || len <= 0)
		error(ebad);
	memmove(buf, a, len);
	buf[len] = 0;
	s = strtol(buf, &e, 10);
	if (*e || s > 65504)
		error(ebad);
	n = shelf;
	shelf = s;
	poperror();
	runlock(&lk);
	writeconfig();
	setprompt();
	if (s < 0)
		xsyslog("Shelf address base changed from %d to unset\n", n);
	else if (n < 0)
		xsyslog("Shelf address base set to %d\n", s);
	else
		xsyslog("Shelf address base changed from %d to %d\n", n, s);
	return len;
}

int
wrpeerea(void *, void *a, int len, vlong)
{
	int n;
	char buf[2048];

	if (len > sizeof buf - 1 || len <= 0) {
		uerr("bad addresses");
		return 0;
	}
	memmove(buf, a, len);
	buf[len] = 0;
	wlock(&peerealck);
	n = setpeerea(buf);	/* sets uerr */
	wunlock(&peerealck);
	if (n == 0)
		return 0;
	restrictpeer();
	return len;
}

int
rdinits(void *, void *a, int count, vlong offset)
{
	char buf[8192];
	char *p, *e;
	int i;
	Inits *ip;
	
	p = buf;
	e = p + sizeof buf;
	*p = 0;
	qlock(&initslk);
	for (i = 0; i < ninits; i++) {
		ip = inits + i;
		if (ip->time)
			p = seprint(p, e, "%E %ulld\n", ip->ea, ip->time);
	}
	qunlock(&initslk);
	return readstr(offset, a, count, buf);
}

int
rdports(void *, void *a, int count, vlong offset)
{
	char buf[8192], *p, *e;
	int i;

	p = buf;
	e = p + sizeof buf;
	*p = 0;
	for (i=0; i<nsanports; i++)
		p = seprint(p, e, "%d %s\n", i, sanports[i].name);
	return readstr(offset, a, count, buf);
}

int
rdea(void *, void *a, int count, vlong offset)
{
	char buf[8192];
	char *p, *e;
	int i;
	
	p = buf;
	e = p + sizeof buf;
	*p = 0;
	rlock(&myealck);
	for (i = 0; i < myeaindx; i++)
		p = seprint(p, e, "%E\n", &myea[i*6]);
	runlock(&myealck);
	return readstr(offset, a, count, buf);
}

int
rdpeerea(void *, void *a, int count, vlong offset)
{
	char buf[8192];
	char *p, *e;
	int i;
	
	p = buf;
	e = p + sizeof buf;
	*p = 0;
	rlock(&peerealck);
	for (i = 0; i < peereaindx && p<e; i++)
		p = seprint(p, e, "%E\n", &peerea[i*6]);
	runlock(&peerealck);
	return readstr(offset, a, count, buf);
}

int
rdstats(void *, void *a, int count, vlong)
{
	USED(a, count);
	return 0;
}

void
netread(int port)
{
	Msg *m, *n;
	Aoe *ah;
	int fd;

	fd = sanports[port].fd;
	n = msgalloc();
loop:
	m = n;
	u->err = nil;
	m->count = read(fd, m->data, Nmsgsz);
	if (m->count <= 0)
		goto loop;
	if (sanports[port].mbps < 1000)
		goto loop;
	ah = (Aoe *)m->data;
	if (isours(ah->s))
		goto loop;
	m->port = port;
	n = msgalloc();
	if (n == nil) {		/* make sure we get ours */
		n = m;
		goto loop;
	}
	if (tracing && memcmp(m->data, bcast, 6) != 0) 
		aoeshow(m, "receiving port %d:", fd);
	if (ah->vf & ARESP) {
		gotresp(m);		/* gotresp keeps it */
		goto loop;
	}
	/* need to be able to process responses in shutdown mode, o/w drop */
	if (shutdown || shelf == -1 || ioq.nq >= Nioq) {
		msgfree(m);
		goto loop;
	}
	qlock(&ioq);
	if (ioq.hd)
		ioq.tl->next = m;
	else
		ioq.hd = m;
	ioq.tl = m;
	ioq.nq++;
	if (ioq.nq > ioq.nqmax)
		ioq.nqmax = ioq.nq;
	rwakeup(&ioq);
	qunlock(&ioq);
	goto loop;
}

void
serve(void)	/* target process */
{
	Msg *m;
	ushort sh;
	Aoe *ah;
	LV *l;
	void (*f)(LV *, Msg *, Aoe *);

	for (m = nil;; msgfree(m)) {
		qlock(&ioq);
		if (m)
			serving--;
		while ((m = ioq.hd) == nil)
			rsleep(&ioq);
		serving++;
		if (serving > servingmax)
			servingmax = serving;
		if (serving == Ntargprocs)
			servingbusy++;
		ioq.hd = m->next;
		ioq.nq--;
		qunlock(&ioq);
		if (shutdown)	// anyone stuck in rsleep will get shot with "kill"
			xlexits(0);
		ah = (Aoe *)m->data;
		sh = nhgets(ah->shelf);
		if (sh != 0xffff && (sh < shelf || sh > shelf + 15))
			continue;
		if (ah->slot == 0xff || sh == 0xffff) {
			srvbcst(m, ah);
			continue;
		}
		l = luns[((sh - shelf) << 8) + ah->slot];
		if (l == nil)
			continue;
		rlock(l);
		if ((l->mode & LVONL) == 0) {
			runlock(l);
			continue;
		}
		if (!maskok(l, ah->s)) {
			runlock(l);
			continue;
		}
		runlock(l);
		if (0)	// XXX should be able to turn this on and off
			noteinit(ah);
		f = proto[ah->proto];
		if (f)
			(*f)(l, m, ah);
	}
}

void
srvbcst(Msg *m, Aoe *ah)	/* handle broadcasts */
{
	LV *l;
	Msg *n;
	int i, sh, sl, lsh, lsl;
	Aoe *nh;
	void (*f)(LV *, Msg *, Aoe *);

	sh = nhgets(ah->shelf);
	sl = ah->slot;
	for (i = 0; i < nelem(luns); i++) {
		l = luns[i];
		if (l == nil)
			continue;
		rlock(l);
		if (waserror()) {
			runlock(l);
			continue;
		}
		if ((l->mode & LVONL) == 0)
			nexterror();
		lsh = shelf + SH(l->lun);
		lsl = l->lun & 0xff;
		if (sh != 0xffff && sh != lsh)
			nexterror();
		if (sl != 0xff && sl != lsl)
			nexterror();
		if (!maskok(l, ah->s))
			nexterror();
		poperror();
		runlock(l);
		n = msgclone(m);
		if (n == nil)
			continue;
		if (0)
			noteinit(ah);
		if (ah->proto >= ACmax) {
			msgfree(n);
			return;
		}
		nh = (Aoe *) n->data;
		hnputs(nh->shelf, lsh);
		nh->slot = lsl;
		f = proto[ah->proto];
		if (f)
			(*f)(l, n, (Aoe *)n->data);
		msgfree(n);
	}
	
}

Msg *
msgalloc(void)
{
	Msg *m;
	
	qlock(&msglock);
	if ((m = freemsg) == nil) {
		msgcount++;
		m = malloc(sizeof *m);
		if (m == nil) {
			qunlock(&msglock);
			return nil;
		}
	} else {
		msgavail--;
		freemsg = m->next;
	}
	qunlock(&msglock);
	m->next = nil;
	m->data = m->xdata + Nmsghdr;
	return m;
}

void
msgfree(Msg *m)
{
	qlock(&msglock);
	msgfrcount++;
	m->next = freemsg;
	freemsg = m;
	msgavail++;
	qunlock(&msglock);
}

Msg *
msgclone(Msg *m)
{
	Msg *n;
	
	n = msgalloc();
	if (n == nil)
		return nil;
	n->count = m->count;
	n->port = m->port;
	memmove(n->data, m->data, n->count);
	return n;
}

void
noteinit(Aoe *ah)	/* note who the initiator is */
{
	Inits *p, *q;

	if (isours(ah->s))
		return;
	q = nil;
	qlock(&initslk);
	for (p = inits; p < &inits[ninits]; p++) {
		if (p->time == 0 && q == nil)
			q = p;
		if (memcmp(p->ea, ah->s, 6) == 0) {
			p->time = ticks;
			goto done;
		}
	}
	if (q == nil) {	/* make a new slot */
		if (ninits >= Ninits) 
			goto done;
		ninits++;
		p->time = ticks;
		memmove(p->ea, ah->s, 6);
		goto done;
	}
	q->time = ticks;
	memmove(q->ea, ah->s, 6);
done:
	qunlock(&initslk);
}
		
typedef struct Ipmsg Ipmsg;
struct Ipmsg {
	int valid;
	Msg *m;
	AoeDisk	*ad;
	int targ;
	uvlong	start;
	uvlong	end;
};

QLock ipmsgq;
Ipmsg ipmsg[Ntargprocs];

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))

static int
iostart(Msg *m, int *idx)
{
	int i, n, targ;
	uvlong start, end;
	uvlong emin, smax;
	Ipmsg *p, *ip;
	AoeDisk *ad;

	ad = (AoeDisk *) m->data;
	qlock(&ipmsgq);
	if (waserror()) {
		qunlock(&ipmsgq);
		return -1;
	}
	n = 0;
	switch (ad->cmd) {
	case 0x30:
	case 0x34:
		n = ad->sectors << 9;
	case 0x20:
	case 0x24:
		break;
	default:
		nexterror();
	}
	start = getlba(ad->lba);
	end = start + ad->sectors;
	targ = (nhgets(ad->shelf) << 8) | ad->slot;
	ip = nil;
	for (i = 0; i < nelem(ipmsg); i++) {
		p = &ipmsg[i];
		if (p->valid == 0) {
			if (ip == nil)
				ip = p;
			continue;
		}
		if (targ != p->targ)
			continue;
		if (start == p->start)
		if (end == p->end)
		if (ad->cmd == p->ad->cmd)
		if (memcmp(ad->data, p->ad->data, n) == 0) {
			/*
			 * This is an exact match, which means it's probably
			 * a retransmit.  Update the msg we'll use to respond
			 * so that the dst and tag are set to this latter message.
			 *
			 * note this also means we *must* iofini before we
			 * rewrite the dst,src header for response.
			 */
			memmove(p->ad->tag, ad->tag, sizeof ad->tag);
			memmove(p->ad->s, ad->s, sizeof ad->s);
			p->m->port = m->port;
			nexterror();
		}
		/*
		 * ok, not an exact match but check for overlap since
		 * that's probably bad, too.
		 */
		emin = MIN(end, p->end);
		smax = MAX(start, p->start);
		if (smax < emin)
			nexterror();
	}
	if (ip == nil)
		nexterror();
	ip->targ = targ;
	ip->start = start;
	ip->end = end;
	ip->ad = ad;
	ip->m = m;
	ip->valid = 1;
	poperror();
	qunlock(&ipmsgq);
	return *idx = (ip - ipmsg);
}

static
void
iofini(int idx)
{
	Ipmsg *p;

	p = &ipmsg[idx];
	qlock(&ipmsgq);
	if (p->valid == 0)
		print("iofini element already completed\n");
	ipmsg[idx].valid = 0;
	qunlock(&ipmsgq);
}

static void
aoesenderr(int err, Msg *m, Aoe *a)
{
	a->err = err;
	a->vf |= ARESP+AERR;
	memmove(a->d, a->s, 6);
	aoesend(sanports[m->port].fd, m->data, 60);
}

void
tdisk(LV *l, Msg *m, Aoe *a)
{
	AoeDisk *ad;
	uvlong offset;
	vlong length;
	int count, mode, idx;

	ad = (AoeDisk *)a;
	rlock(l);
	if (l->flags & LVFsuspended) {
		runlock(l);
		aoesenderr(AEDEV, m, a);
		return;
	}
	if (!resok(l, a->s, ad->cmd)) {
		runlock(l);
		aoesenderr(AERES, m, a);
		return;
	}
	runlock(l);

	switch(ad->cmd) {
	case 0xec:
		rlock(l);
		doident(l, m, ad);
		runlock(l);
		break;
	case 0x20:
	case 0x30:
		ad->lba[3] &= 0xf0;
		ad->lba[4] = 0;
		ad->lba[5] = 0;
	case 0x24:
	case 0x34:
		mode = ad->cmd & 0x10 ? OWRITE : OREAD;
		if (iostart(m, &idx) < 0)
			return;
		rlock(l);
		length = l->length;
		offset = getlba(ad->lba) << 9;
		count = ad->sectors << 9;
		if (mode == OWRITE)
		if ((l->mode & LVSNAP) || (l->mode & LVWRITE) == 0) {
			ad->cmd = DRDY|ERR;
			ad->fea = WP;
			runlock(l);
			goto rsp;
		}
		runlock(l);
		if (offset + count > length) {
			ad->cmd = DRDY|ERR|DF;
			ad->fea = ABRT;
		} else if (canredirect(l, m, mode)) {
			iofini(idx);
			return;
		} else if (lvio(l, ad->data, count, offset, mode) != count) {
			ad->cmd = DRDY|ERR|DF;
			ad->fea = ABRT;
		} else
			ad->cmd = DRDY;
		putlba(ad->lba, (offset + count) >> 9);
rsp:		ad->vf |= ARESP;
		iofini(idx);
		memmove(ad->d, ad->s, 6);
		aoesend(sanports[m->port].fd, m->data, mode == OWRITE ? 60 : count+36);
		break;
	case 0xe7:		// flush cache
	case 0xea:
		ad->vf |= ARESP;
		ad->fea = 0;
		ad->cmd = DRDY;
		memmove(ad->d, ad->s, 6);
		aoesend(sanports[m->port].fd, m->data, 60);
		break;
	default:
		break;
	}
}

/*
 * if the message can be redirected its header is rewritten
 * and it is sent out.  It is assumed this is a unicast request
 * and that ata read/write filtering has already been performed.
 */
static int
canredirect(LV *lv, Msg *m, int mode)
{
	AoeDisk *ad;
	Aoe *a;
	uvlong off;
	uint cnt;
	PVIO pvv;
	uchar ea[6];
	Target *t;
	PV *pv;

	/* special mode for performance testing */
	if (enableredir == 0)
		return 0;

/*1*/	if (waserror())
		return 0;

	ad = (AoeDisk *)m->data;
	off = getlba(ad->lba) * 512;
	cnt = ad->sectors * 512;

	/*
	 * request cannot as a result of writing need to dirty metadata.
	 * xlate handles doing the extent "dirty" work on writes
	 */
	if (xlate(&pvv, lv, off, cnt, mode) == 0)
		nexterror();
/*2*/	if (waserror()) {
		xlatefini(&pvv);
		nexterror();
	}

	/* request cannot split an extent */
	if (pvv.count != cnt)
		nexterror();

	pv = pvv.pv;

	/* validate pv can handle redirect */
	switch (pv->state) {
	case PVSbroke:
	case PVSoosync:
		if (mode == OWRITE)
			dirtyxtnt(pv, pvv.offset, 0);
		// fall through
	case PVSsingle:
		break;
	default:
		nexterror();
	}

	/* pv is available on the ingress port of the request */
	t = lookuptarg(pv->targ);
	if (t == nil)
		nexterror();
	qlock(t);
	if (route(t, ea, m->port) < 0) {
		qunlock(t);
		reletarg(t);
		nexterror();
	}
	qunlock(t);
	reletarg(t);

	/* passed - redirect, rewriting headers and lba */
	m->data -= 24;
	m->count += 24;
	memset(m->data, 0, 24);
	a = (Aoe *)m->data;
	memmove(a->s, ad->s, 6);
	memmove(a->d, ea, 6);
	hnputs(a->t, 0x88a2);
	a->vf = 0x10;
	hnputs(a->shelf, SH(pv->targ));
	a->slot = SL(pv->targ);
	a->proto = AVCREDIR;
	memmove(a->tag, ad->tag, 4);
	putlba(ad->lba, pvv.offset / 512);
	aoesend(sanports[m->port].fd, m->data, m->count);
/*2*/	poperror();
	xlatefini(&pvv);
/*1*/	poperror();

	incfilter(mode == OREAD ? &lv->rf : &lv->wf, cnt, 0);
	incfilter(mode == OREAD ? &pv->rf : &pv->wf, cnt, 0);

	return 1;
}

static void
dstannounce(LV *l, uchar *dst)		/* called with l locked */
{
	char buf[Xblk];
	AoeQC *ap;
	int i;

	ap = (AoeQC *)buf;
	memset(ap, 0, sizeof *ap);
	memcpy(ap->d, dst, 6);
	hnputs(ap->t, 0x88a2);
	ap->vf = 0x10 | ARESP;
	hnputs(ap->shelf, shelf + SH(l->lun));
	ap->slot = l->lun & 0xff;
	ap->proto = 1;

	hnputs(ap->bcnt, BCNT);
	ap->maxsec = MAXSEC;
	ap->ccmd = 0x10 | AQCREAD;
	hnputs(ap->len, l->nqc);
	memmove(ap->conf, l->qc, l->nqc);

	for (i=0; i<nsanports; i++)
		aoesend(sanports[i].fd, buf, sizeof *ap + l->nqc);
}

void
tannounce(LV *l)		/* called with l locked */
{
	int i;

	loadrrstate(l);
	if (l->nmmac == 0)
		dstannounce(l, bcast);
	for (i=0; i<l->nmmac; i++)
		dstannounce(l, l->mmac[i]);
}

void
tqc(LV *l, Msg *m, Aoe *ah)
{
	AoeQC *ap;
	int reqlen, nqc;
	uchar qc[1024];
	uchar *cfg, *p;

	ap = (AoeQC *)ah;
	hnputs(ap->shelf, shelf + SH(l->lun));
	ap->slot = l->lun & 0xff;
	ap->err = 0;
	reqlen = nhgets(ap->len);
	if (ap->ccmd & 0xf != AQCREAD)
	if (reqlen > 1024)
		return;
	cfg = ap->conf;
	
	wlock(l);
	nqc = l->nqc;
	memmove(qc, l->qc, nqc);
	switch (ap->ccmd & 0xf) {
	case AQCTEST:
		if (reqlen != nqc) {
			wunlock(l);
			return;
		}
		/* fall thru */
	case AQCPREFIX:
		if (reqlen > nqc) {
			wunlock(l);
			return;
		}
		if (memcmp(qc, cfg, nqc)) {
			wunlock(l);
			return;
		}
		/* fall thru */
	case AQCREAD:
		break;
	case AQCSET:
		if (nqc)
		/* fall thru */
	case AQCTAR:
		if (nqc != reqlen || memcmp(qc, cfg, reqlen)) {
			ap->vf |= AERR;
			ap->err = AECFG;
			break;
		}
		if ((ap->ccmd & 0xf) == AQCTAR) {
			p = cfg + reqlen;
			reqlen = nhgets(p);
			if (reqlen > 1024) {
				wunlock(l);
				return;
			}
			p += sizeof(short);
			cfg = p;
		}
		/* fall thru */
	case AQCFORCE:
		/* Setting qc is forbidden when suspended */
		if (l->flags & LVFsuspended) {
			wunlock(l);
			aoesenderr(AEDEV, m, ap);
			return;
		}
		memmove(l->qc, cfg, reqlen);
		l->nqc = reqlen;
		if (savemeta(l, 0) < 0) {
			/* probably best if we just don't respond */
			xsyslog("tqc: failure writing LV metadata\n");
			memmove(l->qc, qc, nqc);
			l->nqc = nqc;
			wunlock(l);
			return;
		}
		tannounce(l);
		nqc = reqlen;
		memmove(qc, cfg, nqc);
		break;
	default:
		ap->vf |= AERR;
		ap->err = AEARG;
		break;
	}
	wunlock(l);
	
	memmove(ap->conf, qc, nqc);
	hnputs(ap->len, nqc);
	hnputs(ap->bcnt, BCNT);
	ap->vf |= ARESP;
	ap->maxsec = MAXSEC;
	ap->fwver[0] = 0;
	ap->fwver[1] = 0;
	memmove(ap->d, ap->s, 6);
	aoesend(sanports[m->port].fd, m->data, sizeof (AoeQC) + nqc-1);
}

static int
twrite(int fd, void *a, int count)
{
	Msg m;

	if (tracing) {
		m.port = fd;
		m.count = count;
		m.data = a;
		memset(m.data+6, 0, 6);
		aoeshow(&m, "sending port %d:", fd);
	}
	return write(fd, a, count);
}


int
aoesend(int fd, void *a, int count)
{
	uchar b[60];

	if (count < sizeof b) {
		memcpy(b, a, count);
		memset(b + count, 0, sizeof b - count);
		count = sizeof b;
		a = b;
	}
	return twrite(fd, a, count);
}

vlong
getlba(uchar *p)
{
	int i;
	vlong lba;
	
	lba = 0;
	for (i = 0; i < 6; i++)
		lba |= (vlong)(*p++) << (8*i);
	return lba;
}

void
aoeshow(Msg *m, char *fmt, ...)
{
	AoeDisk *ad;
	AoeQC *aq;
	Aoe *p;
	int len, i;
	char *cp, *ep;
	char buf[512];
	static QLock showlk;
	va_list arg;
	
	qlock(&showlk);
	va_start(arg, fmt);
	vfprint(1, fmt, arg);
	va_end(arg);
	p = (Aoe *)m->data;
	if (nhgets(p->t) != 0x88a2) {
		qunlock(&showlk);
		return;
	}
again:
	print("\tether(s=%E d=%E pr=%04x ln=%d)\n",
		p->s, p->d, nhgets(p->t), m->count);
	print("\taoe(ver=%d flag=%c%c%c%c, err=%d %d.%d cmd=%d tag=%ux)\n",
		p->vf >> 4 & 0xf, 
		p->vf & 0x8 ? 'R' : '-',
		p->vf & 0x4 ? 'E' : '-',
		p->vf & 0x2 ? '1' : '0',
		p->vf & 0x1 ? '1' : '0',
		p->err, nhgets(p->shelf), p->slot, p->proto, nhgetl(p->tag));
		
	if (p->proto == 0) {	// disk
		ad = (AoeDisk *)p;
		print("\taoeata(aflag=-%c-%c--%c%c errfeat=%02x ",
			ad->aflags & 0x40 ? 'E' : '-',
			ad->aflags & 0x10 ? 'D' : '-',
			ad->aflags & 0x02 ? 'A' : '-',
			ad->aflags & 0x01 ? 'W' : '-',
			ad->fea);
		print("sectors=%d cmdstat=%02x lba=%,lld)\n",
			ad->sectors, ad->cmd, getlba(ad->lba));
		dump(ad->data, 64);
	} else if (p->proto == 1) { // Query/Config
		aq = (AoeQC*) p;
		print("\taoeqc(bc=%d, fw=%04x sc=%d ver=%d ",
			nhgets(aq->bcnt), nhgets(aq->fwver), 
			aq->maxsec, aq->ccmd >> 4 & 0xf);
		print("ccmd=%d len=%d cfg=", aq->ccmd & 0xf, nhgets(aq->len));
		len = nhgets(aq->len);
		cp = buf;
		ep = cp + sizeof buf;
		if (len > 32)
			len = 32;
		for (i = 0; i < len; i++)
			if (' ' <= aq->conf[i] && aq->conf[i] <= '~')
				cp += snprint(cp, ep-cp, "%c", aq->conf[i]);
			else
				cp += snprint(cp, ep-cp, "\\x%02x", aq->conf[i]);
		snprint(cp, ep-cp, ")\n");
		print("%s", buf);
				
	} else if (p->proto == 0xf1) {		/* redirect */
		p = (Aoe *)((uchar *)p+24);
		print("REDIRECT\n");
		goto again;
	}
	print("\n");
	qunlock(&showlk);
}

Msg *
aoerwmsg(int t, void *a, int count, vlong offset, int wf)
{
	Msg *m;
	AoeDisk *ad;
	
	count += 511;
	count &= ~511;
	if ((m = prepmsg(t, wf ? count+36 : 60)) == nil)
		return nil;
	ad = (AoeDisk *)m->data;
	if (wf) {
		diskmsg(ad, offset, 0x34, count, Aext|Awrite);
		memmove(ad->data, a, count);
		m->count = 36 + count;
	} else {
		diskmsg(ad, offset, 0x24, count, Aext);
		m->count = 60;
	}
	return m;
}

void
diskmsg(AoeDisk *a, vlong offset, int cmd, int count, int aflags)
{
	a->aflags = aflags;
	a->cmd = cmd;
	count /= 512;
	if (count > MAXSEC)
		count = MAXSEC;
	a->sectors = count;
	putlba(a->lba, offset / 512);
}

Msg *
aoerdcfg(int t)
{
	Msg *m;
	AoeQC *a;
	
	if ((m = prepmsg(t, 60)) == nil)
		return nil;
	a = (AoeQC*)m->data;
	qcmsg(a, AQCREAD, 0);
	return m;
}

#define	max(a, b)	((a) > (b) ? (a) : (b))

Msg *
aoesetcfg(int t, void *a, int count, int force)
{
	Msg *m;
	AoeQC *ap;
	
	if ((m = prepmsg(t, 36 + 1024)) == nil)
		return nil;
	ap = (AoeQC *)m->data;
	m->count = max(60, 36 + 8 + count);
	qcmsg(ap, force ? AQCFORCE : AQCSET, count);
	if (count > 1024)
		count = 1024;
	if (count > 0)
		memmove(ap->conf, a, count);
	return m;
}

void
qcmsg(AoeQC *a, int ccmd, int count)
{
	a->proto = 1;
	hnputs(a->bcnt, 0);
	hnputs(a->bcnt, 0);
	hnputs(a->fwver, 0);
	a->maxsec = 0;
	a->ccmd = ccmd;
	hnputs(a->len, count);
}

Msg *
prepmsg(int t, int mc)
{
	Msg *m;
	Aoe *a;
	
	m = msgalloc();
	if (m == nil)
		return nil;
	a = (Aoe *)m->data;
	hnputs(a->t, 0x88a2);
	a->vf = 0x10;
	a->err = 0;
	hnputs(a->shelf, t >> 8);
	a->slot = t;
	a->proto = 0;
	m->count = mc;
	return m;
}

void
putlba(uchar *p, vlong lba)
{
	int i;
	
	for (i = 0; i < 6; i++) {
		p[i] = lba;
		lba >>= 8;
	}
}

void
doident(LV *l, Msg *m, AoeDisk *ad)
{
	fmtident(l, ad->data, l->length / 512);
	hnputs(ad->shelf, shelf + SH(l->lun));
	ad->slot = l->lun & 0xff;
	ad->fea = (l->mode & LVWRITE) ? 0 : WP;
	memmove(ad->d, ad->s, 6);
	ad->vf |= ARESP;
	ad->cmd = 0x40;
	aoesend(sanports[m->port].fd, m->data, 36+512);
}

#define LBA28MAX 0x0FFFFFFF
#define LBA48MAX 0x0000FFFFFFFFFFFFLL

static 
ushort ident[256] = {
	[47] 0x8000,
	[49] 0x0200,
	[50] 0x4000,
	[83] 0x5400,
	[84] 0x4000,
	[86] 0x1400,
	[87] 0x4000,
	[93] 0x400b,
};

void
fmtident(LV *l, void *a, vlong length)
{
	ushort *ip;
	vlong n;
	
	ip = (ushort *)a;
	memmove(ip, ident, sizeof ident);
	setfld(ip, 27, 40, "Coraid EtherDrive VSX");
	setfld(ip, 23, 8, "5.29");
	setfld(ip, 10, 20, l->serial);	
	n = length;
	n &= ~LBA28MAX;
	if (n)
		setlba28(ip, LBA28MAX);
	else
		setlba28(ip, length);
	setlba48(ip, length);
}

void
setlba28(ushort *p, vlong lba)
{
	p += 60;
	*p++ = lba & 0xffff;
	*p = lba >> 16 & 0x0fffffff;
}

void
setfld(ushort *a, int idx, int len, char *str)
{
	uchar *p;

	p = (uchar *)(a+idx);
	while (len > 0) {
		if (*str == 0)
			p[1] = ' ';
		else
			p[1] = *str++;
		if (*str == 0)
			p[0] = ' ';
		else
			p[0] = *str++;
		p += 2;
		len -= 2;
	}
}

void
setlba48(ushort *p, vlong lba)
{
	p += 100;
	*p++ = lba;
	lba >>= 16;
	*p++ = lba;
	lba >>= 16;
	*p++ = lba;
	lba >>= 16;
	*p = lba;
}

uint
convM2ML(uchar *ap, uint nap, AoeMac *am)	/* cvt message to aoemac struct */
{
	uchar *p, *ep;
	AoeMDir *d;
	int i;
	
	p = ap;
	ep  = p + nap;
	p++;	/* skip reserved byte */
	am->mcmd = *p++;
	am->merror = *p++;
	am->dircnt = *p++;
	if (p + am->dircnt * 8 > ep)
		return 0;
	for (i = 0; i < am->dircnt; i++) {
		d = &am->dir[i];
		p++;	/* another resrved byte */
		d->dcmd = *p++;
		memmove(d->ea, p, 6);
		p += 6;
	}
	return p - ap;
}

uint
convML2M(AoeMac *am, uchar *ap, uint nap)	/* cvt aoemac struct to message */
{
	int i;
	uchar *p;
	AoeMDir *d;

	if (am->dircnt * 8 + 4 > nap)
		return 0;
	p = ap;
	*p++ = 0;
	*p++ = am->mcmd;
	*p++ = am->merror;
	*p++ = am->dircnt;
	for (i = 0; i < am->dircnt; i++) {
		d = &am->dir[i];
		*p++ = 0;
		*p++ = d->dcmd;
		memmove(p, d->ea, 6);
		p += 6;
	}
	return p - ap;
}

int
maskok(LV *l, uchar *mac)		/* return true if mac is in the list, called with l locked */
{
	int i;

	if (l->nmmac == 0)
		return 1;	/* empty == yes */
	for (i = 0; i < l->nmmac; i++)
		if (memcmp(mac, l->mmac[i], 6) == 0)
			return 1;
	return 0;
}

int
addmask(LV *l, uchar *mac)	/* add a mac to the list */
{
	wlock(l);
	if (l->nmmac > 0 && maskok(l, mac)) {	/* already in there */
		wunlock(l);
		return 0;
	}
	if (l->nmmac >= Nmacmask) {
		wunlock(l);
		return -1;
	}
	memmove(l->mmac[l->nmmac++], mac, 6);
	wunlock(l);
	return 0;
}

void
rmmask(LV *l, uchar *mac)	/* take a mac out of the list */
{
	int i;
	
	rlock(l);
	for (i = 0; i < l->nmmac; i++) {
		if (memcmp(l->mmac[i], mac, 6) == 0) {
			memmove(l->mmac[i], l->mmac[--l->nmmac], 6);
			break;
		}
	}
	runlock(l);
}

void
tmask(LV *l, Msg *m, Aoe *a)
{
	AoeMac *am;
	AoeMDir *d;
	int i, j, n;
	
	n = 0;
	am = mallocz(sizeof *am + 255 * sizeof *d, 1);
	if (am == nil)
		return;
	if (convM2ML(m->data + 24, m->count - 24, am) == 0) {
		free(am);
		return;
	}
	rlock(l);
	if (l->flags & LVFsuspended) {
		runlock(l);
		a->err = AEDEV;
		a->vf |= AERR;
		goto bail;
	}
	runlock(l);
	if (am->mcmd == 1) {	/* edit mac list */
		for (i = 0; i < am->dircnt; i++) {
			d = &am->dir[i];
			switch (d->dcmd) {
			case 0:
				break;
			case 1:	/* add a mac */
				addmask(l, d->ea);
				break;
			case 2:	/* ditch a mac */
				rmmask(l, d->ea);
				break;
			default:
				am->merror = 2;
				goto bail;
			}
		}
	}
	rlock(l);
	j = 0;
	for (i = 0; i < l->nmmac; i++) {
		d = &am->dir[j++];
		d->dcmd = 0;
		memmove(d->ea, l->mmac[i], 6);
	}
	runlock(l);
	am->dircnt = j;
	if ((n = convML2M(am, m->data + 24, m->count - 24)) == 0) {
		free(am);
		return;
	}
bail:
	memmove(a->d, a->s, 6);
	a->vf |= ARESP;
	n += 24;
	if (n < 60)
		n = 60;
	aoesend(sanports[m->port].fd, m->data, n);
	free(am);
	return;

}

int
resok(LV *lv, uchar *ea, uchar cmd)		/* called with lv locked */
{
	int i;

	if (lv->nrmac == 0)
		return 1;
	if ((cmd & ~0x14) != 0x20)
		return 1;
	if ((lv->flags & LVFresew) && (cmd & 0x10) == 0)
		return 1;
	for (i = 0; i < lv->nrmac; i++)
		if (memcmp(&lv->rmac[i*6], ea, 6) == 0)
			return 1;
	return 0;
}

void
tres(LV *l, Msg *m, Aoe *a)
{
	uchar *p;
	int n, fd, nrmac, i;
	char *q, *cfn, *key, buf[8192];

	key = "1234567812345678";
	wlock(l);
	p = m->data + 24;
	if (l->flags & LVFsuspended) {
		wunlock(l);
		aoesenderr(AEDEV, m, a);
		return;
	}
	cfn = smprint("/n/rr/%d/ctl", l->lun);
	if (!cfn) {
		xsyslog("tres: memory allocation failure\n");
		a->vf |= AERR;
		a->err = 2;
		goto r;
	}
	switch (*p++) {
	case 0:		/* read reserve list */
r:		*p++ = l->nrmac;
		memmove(p, l->rmac, 6*l->nrmac);
		break;
	case 1:		/* set reserve list */
		if (!resok(l, a->s, 0x30)) {
			a->vf |= AERR;
			a->err = 6;
			goto r;
		}
		/* fall thru */
	case 2:		/* force set reserve list */
		nrmac = *p++;
		if (rmrr(l->lun) < 0) {
			xsyslog("tres: error removing reservation\n");
			a->vf |= AERR;
			a->err = 2;
			goto r;
		}
		if (nrmac == 0) {
			loadrrstate(l);
			break;
		}
		q = seprint(buf, buf + sizeof buf, "register %s", key);
		for (i = 0; i < nrmac; i++) {
			q = seprint(q, buf + sizeof buf, " %.*lH", 6, p + i * 6);
		}
		fd = open(cfn, OWRITE);
		if (fd < 0) {
			xsyslog("tres: could not open control file %s: %r\n", cfn);
			a->vf |= AERR;
			a->err = 2;
			goto r;
		}
		if (write(fd, buf, strlen(buf)) < 0) {
			xsyslog("tres: register write error on %s: %r\n", cfn);
			a->vf |= AERR;
			a->err = 2;
			close(fd);
			goto r;
		}
		seprint(buf, buf + sizeof buf, "reserve 1 %s", key);
		if (write(fd, buf, strlen(buf)) < 0) {
			xsyslog("tres: reserve write error on %s: %r\n", cfn);
 			a->vf |= AERR;
			a->err = 2;
			close(fd);
			goto r;
		}
		close(fd);
		if (loadrrstate(l) < 0) {
			xsyslog("tres: error loading state on LV %s\n", l->name);
			a->vf |= AERR;
			a->err = 2;
			goto r;
		}
		break;
	}
	free(cfn);
	l->flags &= ~LVFresew;
	n = l->nrmac*6 + 26;
	wunlock(l);
	a->vf |= ARESP;
	memmove(a->d, a->s, 6);
	aoesend(sanports[m->port].fd, m->data, n);
}

int
loadrrstate(LV *l)		/* called with l locked */
{
	char *mfn;
	char *toks[Nmacres+1];
	int fd, n, i;
	char buf[8193];

	mfn = smprint("/n/rr/%d/macs", l->lun);
	if (!mfn)
		return -1;
	fd = open(mfn, OREAD);
	if (fd < 0) {
		xsyslog("loadrrstate: could not open macs file %s: %r\n", mfn);
		free(mfn);
		return -1;
	}
	free(mfn);
	n = read(fd, buf, 8192);
	if (n < 0) {
		xsyslog("loadrrstate: could not read macs file %s: %r\n", mfn);
		return -1;
	}
	buf[n] = 0;
	close(fd);
	n = tokenize(buf, toks, Nmacres+1);
	if (n < 1) {
		l->nrmac = 0;
		return 0;
	}
	if (atoi(toks[0]))
		l->flags |= LVFresew;
	else
		l->flags &= ~LVFresew;
	l->nrmac = n - 1;
	for (i = 1; i < n; ++i)
		dec16(l->rmac + 6 * (i-1), 6, toks[i], 12);
	return 0;
}


void
tkrr(LV *l, Msg *m, Aoe *a)
{
	AoeKRR *kp;
	AoeRKRR *rp;
	uchar *p;
	char *cfn, *sfn, *q;
	char *toks[Nmacres+1];
	int fd, n, ntok, i;
	char buf[8193];

	wlock(l);
	cfn = smprint("/n/rr/%d/ctl", l->lun);
	sfn = smprint("/n/rr/%d/stat", l->lun);
	p = m->data + 24;
	kp = (AoeKRR *)m->data;
	if (!cfn || !sfn) {
		xsyslog("tkrr: memory allocation failure\n");
		a->vf |= AERR;
		a->err = 2;
		goto out;
	}
	fd = -1;
	if (l->flags & LVFsuspended) {
		a->vf |= AERR;
		a->err = AEDEV;
		goto out;
	}
	if (kp->rcmd != RCStat) {
		fd = open(cfn, OWRITE);
		if (fd < 0) {
			xsyslog("tkrr: could not open control file %s: %r\n", cfn);
			a->vf |= AERR;
			a->err = 2;
			goto out;
		}
	}
	switch (kp->rcmd) {
	case RCStat:
		break;
	case RCRegister:
		n = kp->rtype;
		q = seprint(buf, buf+8192, "register %.*lH", sizeof kp->key, kp->key);
		for (i = 0; i < n; ++i) {
			q = seprint(q, buf+8192, " %.*lH", 6, kp->other + i * 6);
		}
		if (write(fd, buf, strlen(buf)) < 0) {
			a->vf |= AERR;
			a->err = 2;
		}
		break;
	case RCSet:
		if (!resok(l, a->s, 0x30)) {
			a->vf |= AERR;
			a->err = 6;
			goto out;
		}
		seprint(buf, buf+8192, "reserve %d %.*lH", kp->rtype, sizeof kp->key, kp->key);
		if (write(fd, buf, strlen(buf)) < 0) {
			a->vf |= AERR;
			a->err = 6;
		}
		break;
	case RCReplace:
		seprint(buf, buf+8192, "replace %d %.*lH %.*lH", kp->rtype, sizeof kp->key , kp->key, 8, kp->other);
		if (write(fd, buf, strlen(buf)) < 0) {
			a->vf |= AERR;
			a->err = 2;
		}
		break;
	case RCReset:
		fprint(fd, "reset");
		break;
	default:
		a->vf |= AERR;
		a->err = 2;
		break;
	}
	if (kp->rcmd != RCStat)
		close(fd);
	if (!(a->vf & AERR)) {
		if (kp->rcmd != RCStat && loadrrstate(l) < 0) {
			a->vf |= AERR;
			a->err = 2;
			goto out;
		}
		fd = open(sfn, OREAD);
		if (fd < 0 || (n = read(fd, buf, 8192)) < 0) {
			xsyslog("tkrr: could not access status file %s: %r\n", sfn);
			a->vf |= AERR;
			a->err = 2;
			goto out;
		}
		close(fd);
		buf[n] = 0;
		ntok = tokenize(buf, toks, Nmacres+1);
		rp = (AoeRKRR *)m->data;
		rp->rtype = atoi(toks[0]);
		hnputs(rp->nkeys, ntok - 3);
		memset(rp->reserved, 0, 4);
		hnputl(rp->genctr, atol(toks[1]));
		dec16(rp->key, 8, toks[2], 16);
		p = rp->other;
		for (i = 3; i < ntok; ++i) {
			p += dec16(p, 8, toks[i], 16);
		}
	}
out:
	free(cfn);
	free(sfn);
	wunlock(l);
	a->vf |= ARESP;
	memmove(a->d, a->s, 6);
	aoesend(sanports[m->port].fd, m->data, p - m->data);
}

/* Soli Deo Gloria */
/* Brantley Coile */
