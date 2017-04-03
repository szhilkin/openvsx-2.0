#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <ip.h>
#include <ctype.h>
#include <mp.h>
#include <libsec.h>
#include <pool.h>
#include <libcutil.h>
#include "dat.h"
#include "fns.h"
#include "alib.h"

/*
 * xlate.c: the core of the VSX
 */

#define togbsyslog if(!togb) USED(togb); else xsyslog

static	char	Ebadalloc[] =	"memory allocation failure";
static	char	Epoolfull[] = 	"out of space in pool";
static	char	EnoLVE[] = 	"could not fetch LVE";
static	char	Ewlvt[] =	"error writing LVT";

static	char	Mmconf[] =	"/n/kfs/conf/maintenancemode";
static	char	Prr[] = 	"striped";	/* roundrobin allocation */
static	char	Pct[] = 	"concat";	/* default allocation */

static	void	mark(LV *, int);
static	void	check4lv(PVPool *, PV*);
static	void	countem(uint *, PV *);
static	int	createlv(PVPool*, char*, vlong, uint, int);
static	void	decrefs(LV*); 
static	void	dumpref(ushort *, int);
static	PV*	emptiest(PVPool*);
static	int	getexts(LV *l, int);
static	PV*	insertpv(PVPool *, PV*);
static	LV*	loadmeta(PV*, uint);
static	int	loadvt(LV*, PV *, vlong, int);
static	PVPool*	lookuppool(char *, int *);
static	int	maxedout(LV *);
static	int	poolspace(PVPool*);
static	void	prhisto(char *, char *, uint *);
static	uint	pvspace(PV*);
static	void	updpool(PVPool*);
static	int	moo(LV *, ulong, LVE *, PVIO *);
static	int	cntfreerefs(PV *);
static	PV *	loadpvt(PV *);
static	PVE	allocpve(PVPool *, PV *, int);
static	int	flushpve(PV *, int);
static	void	incpvref(PVPool *, LVE *);
static	void	updnse(LV *, ulong, LVE *);
static	void	zpvmeta(PV *);
static	LVEBUF *getlveblk(LV *, int);
static	void	snapcal(LV *);
static	void	rmpool(PVPool *);
static	void	invalblk(LV *);

static LVEBUF *lvecache[Nlvebuf];
static LVEBUF *lruhead, *lrutail;
static QLock lclock;
PV virtpv = {
	.state = PVSvirtual,
	.length = (1LL<<63)-1,
};

char *
invalidname(char *op)
{
	char *p;

	for (p=op; *p; p++) {
		if (!isalnum(*p))
			break;
	}
	if (*p || p-op > Nname)
		return uerr("LV name %s must be entirely alphanumeric and %d or fewer characters", op, Nname);
	return nil;
}

PVPool *
xlmkpool(char *name, char *label)		/* create new pool */
{
	PVPool *p;
	int pos;

	if (invalidname(name))		/* sets u->err */
		return nil;
	wlock(&lk);
	p = nil;
	if (waserror()) {
		wunlock(&lk);
		freepool(p);
		return nil;
	}
	if (lookuppool(name, &pos))
		error("pool %s already exists", name);
	if (pos == Npools) 
		error("cannot create more than %d pools", Npools);
	p = mallocz(sizeof *p, 1);
	if (p == nil)
		error("memory allocation failure for pool");
	p->name = strdup(name);
	p->label = strdup(label);
	if (p->pv == nil || p->name == nil || p->label == nil)
		error("memory allocation failure for pool elements");
	p->npv = 0;
	p->next = pools;
	pools = p;
	p->rrobin = 0;
	newpool(p, name);
	poperror();
	wunlock(&lk);
	return p;
}

/* Does the real pool remove, called with wlock on lk */
static void
rmpool(PVPool *p)
{
	PVPool **pp;

	for (pp = &pools; *pp && *pp != p; pp = &(*pp)->next)
		;
	assert(*pp);
	*pp = p->next;
	deldir(p->dir);
	freepool(p);
}

int
xlrmpool(char *name)	/* see if we can remove a pool */
{
	PVPool *p;
	
	wlock(&lk);
	if (waserror()) {
		wunlock(&lk);
		return -1;
	}
	p = lookuppool(name, nil);
	if (p == nil)
		error("cannot delete pool %s: unknown pool", name);
	if (p->npv)
		error("cannot delete pool %s: pool contains %ld pvs", name, p->npv);
	rmpool(p);
	poperror();
	wunlock(&lk);
	return 0;
}

void
xllabpool(PVPool *pvp, char *label)
{
	wlock(pvp);
	if (pvp->label)
		free(pvp->label);
	pvp->label = strdup(label);
	wunlock(pvp);
	writeconfig();
}
	
/*
 * Fields
	Free Exts
	Total Exts
	[ vols in pool ]
 */

int
poolstatus(void *arg, void *a, int count, vlong offset)
{
	PVPool *pvp;
	char buf[9000];
	char *s, *e;

	if (offset)
		return 0;	
	s = buf;
	e = s + sizeof buf;
	pvp = arg;
	seprint(s, e, "%ld %ld 0 %s", pvp->efree, pvp->etotal, pvp->flags & PVPFstriped ? Prr : Pct);
	return readstr(offset, a, count, buf);
}

void
debugpvpfree(PVPool *pvp)	/* print to stdout debugging information */
{
	PV *pv;
	int i, j, used, total;

	used = total = 0;
	for (i = 0; i < Npvsperpool; i++) {
		pv = pvp->pv[i];
		if (pv == nil)
			continue;
		for (j = 0; j < pv->npve; j++)
			if (pv->ref[j] & 07777)
				used++;
		total += pv->npve;
	}
	if (pvp->efree != total - used)
		print("!!! efree=%ld ", pvp->efree);
	if (pvp->etotal != total)
		print("!!! etotal=%ld ", pvp->etotal);
	print("free=%d total=%d\n", total - used, total);
}

int
rdlvexts(void *arg, void *a, int count, vlong offset)
{
	LV *lv;
	char buf[8192];
	char *p, *e;
	int i;

	lv = arg;
	p = buf;
	e = p + sizeof(buf);
	*buf = 0;
	rlock(lv);
	for (i = 0; i < Npvsperpool; i++) {
		if (lv->exts[i])
			p = seprint(p, e, "%d %lud\n", i, lv->exts[i]);
	}
	runlock(lv);
	return readstr(offset, a, count, buf);
}

int
rdpoollab(void *arg, void *a, int count, vlong offset)
{
	PVPool *pvp;
	int n;
	
	pvp = arg;
	rlock(pvp);
	n = rdlabel(pvp->label, a, count, offset);
	runlock(pvp);
	return n;
}

int
rdpoollvs(void *arg, void *a, int count, vlong offset)
{
	PVPool *pvp;
	LV *l;
	char buf[9000];
	char *p, *e;

	pvp = arg;
	p = buf;
	e = p + sizeof buf;
	rlock(&lk);
	for (l = vols; l; l = l->next) {
		rlock(l);
		if (l->pool == pvp)
			p = seprint(p, e, "%s ", l->name);
		runlock(l);
	}
	runlock(&lk);
	return readstr(offset, a, count, buf);
}

int
rdlabel(char *label, void *a, int count, vlong offset)		/* called with structure holding label locked */
{
	return readstr(offset, a, count, label);
}

int
wrpoollab(void *arg, void *a, int count, vlong)
{
	PVPool *pvp;
	int n;
	
	pvp = arg;
	wlock(pvp);
	n = wrlabel(&pvp->label, a, count);
	wunlock(pvp);
	writeconfig();
	return n;
}

/* XXX need to break this out to save the meta for each object
 * that a label is stored on.  Unused today.
 */
int
wrlabel(char **lp, void *a, int count)		/* called with structure holding lp locked */
{
	char *p;

	p = mallocz(count+1, 1);
	if (p == nil) {
		uerr("allocation failure for label");
		return 0;
	}
	memmove(p, a, count);
	p[count] = 0;
	/* newlines mess up the config file */
	while(count) {
		if (p[count - 1] == '\n' || p[count - 1] == '\r' )
			p[--count] = 0; 
		else 
			break;
	}
	if (strlen(p) >= Nlabelmax) {
		uerr("label '%s' too long\n", p);
		return -1;
	}
	if (*lp)
		free(*lp);
	*lp = p;
	return count;
}

int
rdpvlab(void *arg, void *a, int count, vlong offset)
{
	PV *pv;
	int n;
	
	pv = arg;
	rlock(pv);
	n = rdlabel(pv->label, a, count, offset);
	runlock(pv);
	return n;
}

int
wrpvlab(void *arg, void *a, int count, vlong)
{
	PV *pv;
	int n;
	char *olabel;

	pv = arg;
	wlock(pv);
	if (pv->flags & PVFlost) {
		wunlock(pv);
		uerr("PV %T lost", pv->targ);
		return 0;
	}
	olabel = pv->label;
	pv->label = nil;
	n = wrlabel(&pv->label, a, count);
	if (n > 0) {
		if (updpv(pv) < 0) {
			free(pv->label);
			pv->label = olabel;
			n = 0;
		} else {
			free(olabel);
		}
	} else {
		pv->label = olabel;
	}
	wunlock(pv);
	return n;
}

int
rdpv(void *arg, void *a, int count, vlong offset)
{
	PV *pv;
	int size;
	
	pv = arg;
	size = (pv->npve-1) * sizeof (ushort) + sizeof (*pv);
	return readmem(offset, a, count, arg, size);
}

int
rdlvlab(void *arg, void *a, int count, vlong offset)
{
	LV *lv;
	int n;
	
	lv = arg;
	rlock(lv);
	n = rdlabel(lv->label, a, count, offset);
	runlock(lv);
	return n;
}

int
wrlvlab(void *arg, void *a, int count, vlong)
{
	LV *lv;
	int n;
	
	lv = arg;
	n = 0;
	wlock(lv);
	if (lv->flags & LVFsuspended) {
		uerr("LV %s suspended\n", lv->name);
	} else {
		n = wrlabel(&lv->label, a, count);
		savemeta(lv, 0);
	}
	wunlock(lv);
	return n;
}

int
rdlvt(void *arg, void *a, int count, vlong offset)
{
	vlong vtsz;
	uvlong n;
	int cnt, rcnt;
	uchar *p;
	LVEBUF *b;
	LV *lv;

	p = (uchar *) a;
	lv = arg;
	vtsz = (vlong)lv->nlve / Xlveperblk * Xblk;

	if (offset % Xblk) {
		uerr("offset must be a multiple of %d\n", Xblk);
		return -1;
	}

	if (offset > vtsz)
		return 0;

	if (offset + count > vtsz)
		count = vtsz - offset;

	n = offset / Xblk;
	n *= Xlveperblk;
	for (rcnt = 0; rcnt < count;) {
		cnt = count;
		if (cnt > Xblk)
			cnt = Xblk;

		qlock(&lclock);
		b = getlveblk(lv, n);
		if (b == nil) {
			qunlock(&lclock);
			break;
		}
		memcpy(p, b->buf, cnt);
		qunlock(&lclock);

		p += cnt;
		rcnt += cnt;
		n += Xlveperblk;
	}
	return rcnt;
			
}

int
rdlvserial(void *arg, void *a, int count, vlong offset)
{
	LV *lv;
	int n;

	lv = arg;
	rlock(lv);
	if (count > 20) 
		count = 20;
	n = readstr(offset, a, count, lv->serial);
	runlock(lv);
	return n;
}

int
rdmask(void *arg, void *a, int count, vlong offset)
{
	LV *l;
	char buf[8192], *p, *ep;
	int i;
	
	p = buf;
	ep = buf + sizeof buf;
	l = arg;
	*p = 0;
	rlock(l);
	for (i = 0; i < l->nmmac; i++)
		p = seprint(p, ep, "%E\n", l->mmac[i]);
	runlock(l);
	return readstr(offset, a, count, buf);
}

int
rdres(void *arg, void *a, int count, vlong offset)
{
	LV *l;
	char buf[8192], *p, *ep;
	int i;
	
	p = buf;
	ep = buf + sizeof buf;
	*p = 0;
	l = arg;
	rlock(l);
	for (i = 0; i < l->nrmac; i++)
		p = seprint(p, ep, "%E\n", &l->rmac[i*6]);
	runlock(l);
	return readstr(offset, a, count, buf);
}

int
rdlvpool(void *arg, void *a, int count, vlong offset)
{
	LV *l = arg;

	return readstr(offset, a, count, l->pool->name);
}

int
rdlviops(void *arg, void *a, int count, vlong offset)
{
	LV *l = arg;
	char buf[1024];

	snprint(buf, 1024, "%*O %*O", l->iopssamp, &l->rf, l->iopssamp, &l->wf);
	return readstr(offset, a, count, buf);
}
int
wrlviops(void *arg, void *a, int count, vlong)
{
	int secs, nf;
	LV *l;
	Cmdbuf *cb;

	cb = parsecmd(a, count);
	if (cb == nil) {
		uerr("allocation failure");
		return 0;
	}
	secs = atoi(cb->f[0]);
	nf = cb->nf;
	free(cb);
	l = arg;
	if (nf != 1 || secs <= 0 || secs > 30) {
		uerr("seconds must be a number between 1 and 30");
		return 0;
	}
	wlock(l);
	l->iopssamp = secs;
	wunlock(l);
	return count;
}

int
rdsnaplimit(void *arg, void *a, int count, vlong offset)
{
	LV *l;
	char buf[1024];

	l = arg;
	rlock(l);
	if (l->snaplimit == SLunset)
		strncpy(buf, "unset", sizeof(buf));
	else if (l->snaplimit == SLign)
		strncpy(buf, "ignore", sizeof(buf));
	else
		snprint(buf, sizeof(buf), "%lld", l->snaplimit);
	runlock(l);
	return readstr(offset, a, count, buf);

}

int
rdsnapsched(void *arg, void *a, int count, vlong offset)
{
	Snapsched s[32];
	LV *l;
	char buf[8192], *b, *e;
	int i;

	b = buf;
	e = buf + sizeof buf;
	l = arg;

	rlock(l);
	memcpy(s, l->sched, sizeof(Snapsched) * Nsched);
	runlock(l);
	qsort(s, Nsched, sizeof (Snapsched), (int (*)(void *, void *))schedcmp);
	for (i = 0; i < Nsched; i++) {
		if (s[i].retain) {
			b = seprint(b, e, "%d %d %d %d %d %d %d\n",
			s[i].class,  s[i].retain, s[i].mon, s[i].mday, 
			s[i].wday, s[i].hour, s[i].min);
		}
	}
	return readstr(offset, a, b - buf > count ? count : b - buf, buf);
}

int
wrlvserial(void *arg, void *a, int count, vlong)
{
	LV *l;
	
	l = arg;
	wlock(l);
	if (l->flags & LVFsuspended) {
		wunlock(l);
		uerr("LV %s suspended", l->name);
		return 0;
	}
	if (count > 20) 
		count = 20;
	strncpy(l->serial, a, 20);
	l->serial[count] = 0;
	savemeta(l, 0);
	wunlock(l);
	return count;
}

int
rdlvstats(void *arg, void *a, int count, vlong offset)
{
	LV *l;
	char buf[1024];

	l = arg;
	rlock(l);
	snprint(buf, 1024, "%*I %*I", l->statsamp, &l->rf, l->statsamp, &l->wf);
	runlock(l);
	return readstr(offset, a, count, buf);
}

int
wrlvstats(void *arg, void *a, int count, vlong)
{
	int secs, nf;
	LV *l;
	Cmdbuf *cb;

	cb = parsecmd(a, count);
	if (cb == nil) {
		uerr("allocation failure");
		return 0;
	}
	secs = atoi(cb->f[0]);
	nf = cb->nf;
	free(cb);
	l = arg;
	if (nf != 1 || secs <= 0 || secs > 30) {
		uerr("seconds must be a number between 1 and 30");
		return 0;
	}
	wlock(l);
	l->statsamp = secs;
	wunlock(l);
	return count;
}

int
wrpviops(void *arg, void *a, int count, vlong)
{
	int secs, nf;
	PV *pv;
	Cmdbuf *cb;

	cb = parsecmd(a, count);
	if (cb == nil) {
		uerr("allocation failure");
		return 0;
	}
	secs = atoi(cb->f[0]);
	nf = cb->nf;
	free(cb);
	pv = arg;
	if (nf != 1 || secs <= 0 || secs > 30) {
		uerr("seconds must be a number between 1 and 30");
		return 0;
	}
	wlock(pv);
	pv->iopssamp = secs;
	wunlock(pv);
	return count;
}

int
rdpviops(void *arg, void *a, int count, vlong offset)
{
	PV *pv = arg;
	char buf[1024];
	
	rlock(pv);
	snprint(buf, 1024, "%*O %*O",
		pv->iopssamp, &pv->rf, pv->iopssamp, &pv->wf);
	runlock(pv);
	return readstr(offset, a, count, buf);
}

int
wrpvstats(void *arg, void *a, int count, vlong)
{
	int secs, nf;
	PV *pv;
	Cmdbuf *cb;

	cb = parsecmd(a, count);
	if (cb == nil) {
		uerr("allocation failure");
		return 0;
	}
	secs = atoi(cb->f[0]);
	nf = cb->nf;
	free(cb);
	pv = arg;
	if (nf != 1 || secs <= 0 || secs > 30) {
		uerr("seconds must be a number between 1 and 30");
		return 0;
	}
	wlock(pv);
	pv->statsamp = secs;
	wunlock(pv);
	return count;
}

int
rdpvstats(void *arg, void *a, int count, vlong offset)
{
	PV *pv = arg;
	char buf[1024];

	rlock(pv);
	snprint(buf, 1024, "%*I %*I",
		pv->statsamp, &pv->rf, pv->statsamp, &pv->wf);
	runlock(pv);
	return readstr(offset, a, count, buf);
}

/*
 * pv status fields
 *	states: missing, single, mirroed, broke, oosync, relivering
 *	Free Exts
 *	dirty exts
 *	extents used in as volume meta data
 *	Total Exts
 *	primary target
 *	Mirror's AoE Target (if defined)
 *	length in bytes
 */

int
pvstatus(void *arg, void *a, int count, vlong offset)
{
	char buf[8192];
	int used, dirty, lvt, i, alloc;
	ushort r;
	PV *pv;

	if (offset)
		return 0;
	pv = arg;
	rlock(pv);
	used = dirty = lvt = alloc = 0;
	for (i = 0; i < pv->npve; i++) {
		r = pv->ref[i];
		if (REFCNT(r)) {
			alloc++;
			if (r & REFused)
				used++;
			if (r & REFdirty)
				dirty++;
			if (r & REFlvt)
				lvt++;
		}
	}
		
	snprint(buf, sizeof buf, "%s %d %d %d %ud %T %T %lld 0x%lx %d", 
		fmtpvstate(pv->state), 
		pv->npve - alloc, used, lvt, pv->npve, pv->targ, pv->mirror, pv->length, pv->flags, dirty);
	runlock(pv);
	return readstr(offset, a, count, buf);
}

char *
fmtsnapclass(Snapsched *s) {
	switch(s->class) {
	case LVSmanual:
		return "manual";
	case LVShour:
		return "hourly";
	case LVSday:
		return "daily";
	case LVSweek:
		return "weekly";
	case LVSmonth:
		return "monthly";
	case LVSyear:
		return "yearly";
	default:
		return "???";
	}

}

int
pvctime(void *arg, void *a, int count, vlong offset)
{
	char buf[64];
	PV *pv;

	if (offset)
		return 0;
	pv = arg;
	rlock(pv);
	snprint(buf, sizeof buf, "%uld", pv->ctime);
	runlock(pv);
	return readstr(offset, a, count, buf);
}

char *
fmtpvstate(int state)
{
	switch (state) {
	case PVSmissing:
		return "missing";
	case PVSsingle:
		return "single";
	case PVSdouble:
		return "mirrored";
	case PVSbroke:
		return "broken";
	case PVSoosync:
//		return "pre-silver";
	case PVSresilver:
		return "silvering";
	default:
		return "???";
	}
}

/*
 *  lv status fields
 *	mode
 *	total extents
 *	thin extents
 *	dirty extents
 *	unique extents
 *	creation date
 *	shadow remote VSX name
 *	declared length in bytes
 *	Is snapshot (normal or snap)
 *	Exported Lun value (-1.255 if unset)
 *	shadow remote VSX LV
 *	actual length
 *	VLAN
 */

int
lvstatus(void *arg, void *a, int count, vlong offset)
{
	char buf[8192];
	LV *l;

	if (offset)
		return 0;
	l = arg;
	rlock(l);
	snprint(buf, sizeof buf, "0x%ulx %uld %uld %uld %uld %uld %T %lld %s %T %T %lld %d %uld %s %s %s %uld",
		l->mode, l->nlve, l->thin, l->dirty, l->mode & LVSNAP ? l->nse : 0, 
		l->ctime, -1, l->length,
		l->mode & LVSNAP ? fmtsnapclass(l->sched) : "normal",
		lun2targ(l->lun), -1, (l->nlve - l->frstdat) * 1LL * Xextent,
		l->vlan, l->nse, l->rmtname ? l->rmtname : "unset",
		l->rmtlv ? l->rmtlv : "unset",
		l->flags & LVFsuspended ? "suspended" : "healthy",
		l->prevsnap);
	runlock(l);
	return readstr(offset, a, count, buf);
}

int
lvctime(void *arg, void *a, int count, vlong offset)
{
	char buf[64];
	LV *l;

	if (offset)
		return 0;
	l = arg;
	rlock(l);
	snprint(buf, sizeof buf, "%uld", l->ctime);
	runlock(l);
	return readstr(offset, a, count, buf);
}

/*
 * list historograms for reference stats for each pv
 *  Fields
 *	pvname wrcnt dirtycnt lvcnt flushcnt { cnt:refcnt }
 */

int
pvhisto(void *arg, void *a, int count, vlong offset)
{
	char buf[4096];
	uint *histo;
	PV *pv;

	if (offset)
		return 0;
	pv = arg;
	histo = mallocz((4+4096)*sizeof (uint), 1);
	if (histo == nil) {
		uerr("allocation failure for histogram");
		return 0;
	}
	rlock(pv);
	countem(histo, pv);
	runlock(pv);
	prhisto(buf, buf + sizeof buf, histo);
	free(histo);
	return readstr(offset, a, count, buf);
}

static 
void
countem(uint *h, PV *pv)	/* create histogram of physical volume refs, called with pv locked */
{
	int i;
	ushort *r;

	for (i = 0; i < pv->npve; i++) {
		r = pv->ref + i;
		if (*r & REFused)
			h[0]++;
		if (*r & REFdirty)
			h[1]++;
		if (*r & REFlvt)
			h[2]++;
		if (*r & REFnf)
			h[3]++;
		h[(*r & 07777)+4]++;
	}
}

static 
void
prhisto(char *p, char *ep, uint *h)	/* format histo */
{
	int i;

	p = seprint(p, ep, "%ud %ud %ud %ud ", h[0], h[1], h[2], h[3]);
	for (i = 4; i < 4+4096; i++) {
		if (h[i] == 0)
			continue;
		p = seprint(p, ep, "%ud:%ud ", h[i], i-4);
	}
	seprint(p, ep, "\n");
}

/* Read the raw bits from the pvt.  This data is fixed on disk anyway. */
int
pvtread(void *arg, void *a, int count, vlong off)
{
	PV *pv;
	int size;
	
	pv = arg;
	rlock(pv);
	size = pv->npve * sizeof (ushort);
	if (off >= size) {
		runlock(pv);
		return 0;
	}
	if (off + count > size)
		count = size - off;
	memmove(a, pv->ref + off / 2, count);
	runlock(pv);
	return count;
}

static 
PVPool *
lookuppool(char *name, int *pos)		/* called with lk locked */
{
	PVPool *p;
	int i;

	for (i = 0, p = pools; p; i++, p = p->next)
		if (strcmp(p->name, name) == 0) {
			break;
		}
	if (pos)
		*pos = i;
	return p;
}

static 
uvlong
genuuid(void)
{
	uvlong uuid;
	
	uuid = time(nil);
	uuid <<= 32;
	uuid |= truerand();
	return uuid;
}

int
xladdtopool(PVPool *pvp, PV *pv)		/* called with pvp and pv locked */
{
	int i;

	/* find empty slot in pool */
	for (i=0; i<Npvsperpool; i++)
		if (pvp->pv[i] == nil) {
			pv->pool = pvp;
			pvp->pv[i] = pv;
			pv->id = i;
			xincu(&pvp->npv);
			return 0;
		}
	return -1;
}

void
freepv(PV *pv)
{
	if (pv) {
		delfilter(&pv->rf);
		delfilter(&pv->wf);
		free(pv->ref);
		free(pv->label);
		free(pv);
	}
}

void
freepool(PVPool *p)
{
	if (p) {
		free(p->label);
		free(p->name);
		free(p);
	}
}

/* 
 * See other code when adding legacy volumes.
 * A pv starts with the machine structure on disk
 * for the pv unless it's a legacy.  The qc will indicate
 * which volume and which pve the label is found on.
 */

void
xladdpv(int targ, vlong length, PVPool *pvp)
{
	PV *pv;
	ulong nexts;
	int n;

	wlock(&lk);
	n = targused(targ);
	if (waserror()) {
		wunlock(&lk);
		return;
	}
	if (n != -1) {
		error("target %T already in use", targ);
	}
	nexts = length / Xextent;
	if (nexts == 0) {
		error("PV is too small");
	}
	if (totalext + nexts > MAXEXT) {
		error("max vsx limit exceeded");
	}
	if (ispvmeta(targ)) {
		error("metadata already exists on target %T, please contact Coraid TAC", targ);
	}
	pv = mallocz(sizeof *pv, 1);
	if (pv == nil) {
		error("allocation failure for PV");
	}
	wlock(pvp);
	if (waserror()) {
		wunlock(pvp);
		freepv(pv);
		nexterror();
	}
	setpvtarget(targ, pv->sn[0]);
	pv->label = strdup("");
	pv->ref = vtalloc(nexts, sizeof *pv->ref);
	if (pv->label == nil || pv->ref == nil)
		error("allocation failure for PV elements");

	pv->state = PVSsingle;
	pv->ctime = time(nil);
	pv->targ = targ;
	pv->mtarg = targ;
	pv->meta = pv;
	pv->mirror = -1;
	pv->npve = nexts;
	pv->length = length;
	pv->offset = 0;
	pv->pool = pvp;
	if (initpvt(pv) < 0 || updpv(pv) < 0) {
		error("PV initialization failure");
	}
	wlock(pv);
	if (xladdtopool(pvp, pv) < 0) {
		wunlock(pv);
		zpvmeta(pv);
		error("can not add more than %d PVs to a pool", Npvsperpool);
	}
	xaddu(&pvp->etotal, nexts);
	xaddu(&totalext, nexts);
	xaddu(&pvp->efree, cntfreerefs(pv));
	newpv(pv);
	wunlock(pv);
	wunlock(pvp);
	wunlock(&lk);
	poperror();
	poperror();
	trestrict(targ);
	writeconfig();
	xsyslog("PV %T: created in pool %s\n", targ, pvp->name);
}

/* PV has already been removed from pool, no need to lock */
static
void
zpvmeta(PV *pv)
{
	uchar buf[Xblk];

	memset(buf, 0, sizeof buf);
	switch(pv->state) {
		case PVSdouble:
		case PVSresilver:
		case PVSoosync:
			edio(pv->mirror, buf, Xblk, 0, OWRITE);
		case PVSbroke:	/* Sorry, if you are broke then don't try to clear the mirror */
		case PVSsingle:
			edio(pv->targ, buf, Xblk, 0, OWRITE);
	}
}

int
setpvtarget(int target, char *sn)
{
	Target *t;

	if (target == -1)
		return 0;
	t = lookuptarg(target);
	if (t) {
		qlock(t);
		t->flags |= (TARGpv|TARGckd);
		memcpy(sn, t->serial[0].sn, Nserial);
		qunlock(t);
		reletarg(t);
		return 0;
	}
	return -1;
}

int
clrpvtarget(int target)
{
	Target *t;

	if (target == -1)
		return 0;
	t = lookuptarg(target);
	if (t) {
		qlock(t);
		t->flags &= ~TARGpv;
		qunlock(t);
		reletarg(t);
		return 0;
	}
	return -1;
}

/* Destroy the pool, all PVs, and all LVs
   Do not bother writing anything out to PVs
   Do not bother with most of the checks that normally take place */
void
xldestroypool(char *pool)
{
	LV *lv, *prev;
	int lun, i, targ;
	PVPool *pvp;
	PV *pv;

	prev = nil;
	wlock(&lk);
	if (waserror()) {
		wunlock(&lk);
		return;
	}
	pvp = lookuppool(pool, nil);
	if (pvp == nil) 
		error("cannot destroy pool %s: unknown pool", pool);
	/* 2 Pre-flight checks */
	for (lv = vols; lv; lv = lv->next) {
		if (lv->pool != pvp)
			continue;
		rlock(lv);
		if (isshadow(lv)) {
			runlock(lv);
			error("cannot remove LV, unshadow %s first", lv->name);
		}
		runlock(lv);
	}
	for (i = 0; i < Npvsperpool; i++) {
		pv = pvp->pv[i];
		if (pv == nil)
			continue;
		wlock(pv);
		if (stopsilver(pv) < 0) {
			targ = pv->targ;
			wunlock(pv);
			error("PV %T: unable to stop PV silvering process", targ);
		}
		wunlock(pv);
	}
	for (lv = vols; lv; lv = lv->next) {
		wlock(lv);
		if (lv->pool != pvp) {
			prev = lv;
			wunlock(lv);
			continue;
		}
		if (waserror()) {
			wunlock(lv);
			nexterror();
		}
		/* paranoid */
		if (isshadow(lv))
			error("cannot remove LV, unshadow %s first", lv->name);
		if ((lun = lv->lun) != -1) {
			if (lv->mode & LVONL) {
				lv->mode &= ~LVONL;
				xsyslog("Lun %T: offlined\n", lun2targ(lun));
			}
			lv->lun = -1;
			rmrr(lun);
			if (lv->lundir)
				deldir(lv->lundir);
			luns[lun] = nil;
			xsyslog("Lun %T: removed\n", lun2targ(lun));
		}
		if (prev)
			prev->next = lv->next;
		else
			vols = lv->next;
		poperror();
		invalblk(lv);
		if (lv->dir)
			deldir(lv->dir);
		xsyslog("LV %s: removed\n", lv->name);
		wunlock(lv);
		freelv(lv);
	}
	for (i = 0; i < Npvsperpool; i++) {
		pv = pvp->pv[i];
		if (pv == nil)
			continue;
		wlock(pv);
		if (waserror()) {
			wunlock(pv);
			nexterror();
		}
		/* paranoid */
		if (stopsilver(pv) < 0)
			error("PV %T: unable to stop PV silvering process", pv->targ);
		xaddu(&totalext, -pv->npve);
		deldir(pv->dir);
		pvp->pv[i] = nil;
		xdecu(&pvp->npv);
		poperror();
		if ((pv->flags & PVFlost) == 0 && pv->state != PVSmissing)
		 	zpvmeta(pv);
		wunlock(pv); 
		if ((pv->flags & PVFlost) || pv->state == PVSmissing) {
			tforget(pv->targ);
			tforget(pv->mirror);
		} else {
			tunrestrict(pv->targ);
			if (pv->state == PVSbroke)
				tforget(pv->mirror);
			else
				tunrestrict(pv->mirror);
		}
		clrpvtarget(pv->targ);
		clrpvtarget(pv->mirror);
		xsyslog("PV %T: removed\n", pv->targ);
		freepv(pv);
	}
	rmpool(pvp);
	poperror();
	wunlock(&lk);
	writeconfig();
	xsyslog("Pool %s: destroyed\n", pool);
}

void
xlrmpv(PVPool *pvp, char *pvtarg)	/* if pv empty remove it */
{
	PV *pv, *mpv;
	int i, targ, x, n;

	wlock(pvp);
	if (waserror()) {
		wunlock(pvp);
		return;
	}
	targ = parsetarget(pvtarg);
	if (targ < 0)
		nexterror();	//parsetarget sets uerr
	pv = lookuppv(pvp, targ);
	if (pv == nil)
		error("PV %T not found", targ);
	if (pv->flags & PVFlost)
		error("PV %T is lost", targ);
	/* can't free pv if there's a legacy lun on it; it will just look busy */
	n = metaextents(pv->npve, Xrefperblk);
	i = pv->offset ? 0 : n;				/* legacy PV */
	x = 0;
	for (; i < pv->npve; i++)
		if (REFCNT(pv->ref[i]))
			x++;
	if (x)
		error("cannot remove PV %T with %d allocated extents", targ, x);
	wlock(pv);
	if (stopsilver(pv) < 0) {
		wunlock(pv);
		error("Unable to stop PV silvering process");
	}
	wunlock(pv);
	if (pv->offset)				/* free meta pves on other pv */
	if (mpv = pv->meta) {
		x = pv->offset / Xextent;
		for (i=0; i<n; i++)
			mpv->ref[x+i] = REFnf;
		if (flushpv(mpv) < 0)
			xsyslog("Cannot write pv metadata for %T, potentially losing %d extents in freeing %T\n", mpv->targ, n, pv->targ);
	}
	/* at this point we are committed to tossing pv */
	xaddu(&pvp->etotal, -pv->npve);
	xaddu(&totalext, -pv->npve);
	xaddu(&pvp->efree, -(pv->npve - n));
	for (i = 0; i < Npvsperpool; i++)
		if (pv == pvp->pv[i])
			break;
	if (i >= Npvsperpool)
		error("couldn't find pv, can't happen");
	pvp->pv[i] = nil;
	xdecu(&pvp->npv);
	deldir(pv->dir);
	poperror();
	wunlock(pvp);
	zpvmeta(pv);
	tunrestrict(pv->targ);
	tunrestrict(pv->mirror);
	clrpvtarget(pv->targ);
	clrpvtarget(pv->mirror);
	freepv(pv);
	writeconfig();
	xsyslog("PV %T: removed from pool %s\n", targ, pvp->name);
}

void
xlsnaplimit(LV *lv, char *size)
{
	vlong length;
	int serr;
	
	wlock(lv);	
	if (cistrcmp(size, "ignore") == 0) {
		length = SLign;
	} else {
		length = atosize(size, &serr);
		if (serr) {
			wunlock(lv);
			uerr("Invalid size: %s",size);
			return;
		}
		if (length > lv->pool->etotal * Xextent) {
			wunlock(lv);
			uerr("limit greater than size of pool");
			return;
		}
		if (length < (usedextents(lv) + lv->nse) * Xextent) {
			wunlock(lv);
			uerr("limit too small");
			return;
		}
	}
	if (lv->mode & LVSNAP) {
		wunlock(lv);
		uerr("cannot set a snaplimit on a snapshot");
		return;
	}
	lv->snaplimit = length;
	savemeta(lv, 0);
	wunlock(lv);
	xsyslog("LV %s: scheduled snapshot limit now %s\n", lv->name, size);
}

void
xlclrsnapsched(LV *lv, char *c, char *t)
{
	Snapsched sc;
	char buf[512];

	memset(&sc, 0, sizeof sc);
	/* strtoscls sets uerr */
	if ((sc.class = strtoscls(c)) == -1) { 
		return;
	}	/* parsestime sets uerr */
	if (parsestime(&sc, t)) {	
		return;
	}
	rlock(&lk);
	wlock(lv);
	/* delsched sets uerr */	
	if (delsched(lv, &sc) < 0) {
		wunlock(lv);
		runlock(&lk);
		return;
	}
	savemeta(lv, 0);
	wunlock(lv);
	runlock(&lk);
	schedtostr(&sc, buf, 512);
	xsyslog("LV %s: removed snapshot schedule %s\n", lv->name, buf);
}

void
xlsnapsched(LV *lv, char *c, char *t, char *r)
{
	Snapsched sc;
	char buf[512], *e, *ret;
	long v;

	memset(&sc, 0, sizeof sc);
	/* strtoscls sets uerr */
	if ((sc.class = strtoscls(c)) == -1) { 
		return;
	}

	/* parsestime sets uerr */
	if (parsestime(&sc, t)) {	
		return;
	}

	if (cistrncmp("hold", r, strlen(r)) == 0) {
		sc.retain = -1;
		ret = "hold enabled";
	} else {
		v = strtol(r, &e, 10);
		if (*e) {
			uerr("invalid retain value: %s", r);
			return;
		}
		if (v <= 0) {
			uerr("retain count must be greater than 0\n");
			return;
		}
		if (v > Nretain) {
			uerr("retain count may not be greater than %d\n", Nretain);
			return;
		}
		sc.retain = v;
		ret = r;
	}
	wlock(lv);	
	if (lv->mode & LVSNAP) {
		wunlock(lv);
		uerr("cannot set snapsched on a snapshot");
		return;
	}
	if (lv->snaplimit == SLunset) {
		wunlock(lv);
		uerr("snaplimit must be set");
		return;
	}
	/* savesched sets uerr */
	if (savesched(lv, &sc) < 0) {
		wunlock(lv);
		return;
	}
	savemeta(lv, 0);
	wunlock(lv);
	schedtostr(&sc, buf, 512);
	xsyslog("LV %s: snapshot schedule %s retain count: %s\n", lv->name, buf, ret);
}

void
xlcanprune(LV *lv, int p)
{
	ulong mode;

	wlock(lv);
	if (!(lv->mode & LVSNAP)) {
		wunlock(lv);
		uerr("%s is not a snapshot", lv->name);
		return;
	}
	mode = lv->mode;
	if (p)
		lv->mode |= LVCANPRUNE;
	else
		lv->mode &= ~LVCANPRUNE;
	if (mode != lv->mode)
		savemeta(lv, 0);
	wunlock(lv);
	xsyslog("LV %s: hold is %s\n", lv->name, p ? "disabled" : "enabled");
}

void
xlflushlvmeta(LV *l)
{	
	wlock(l);
	if (savemeta(l, 0) < 0)
		uerr("flush: could not write LV metadata");
	wunlock(l);
	xsyslog("LV %s: flush\n",l->name);
}

void
xlflushpvmeta(PVPool *pvp, char *targ)
{
	int t;
	PV *p;

	t = parsetarget(targ);
	if (t < 0)
		return;		//parsetarget sets uerr
	rlock(pvp);
	if (waserror()) {
		runlock(pvp);
		return;
	}
	p = lookuppv(pvp, t);
	if (p == nil)
		error("PV target %T unknown", t);
	poperror();
	runlock(pvp);

	if (flushpv(p) < 0) {
		uerr("PV metadata save failed");
		return;
	}
	writeconfig();
	xsyslog("PV %T: flush\n", t);
}

PV *
lookuppv(PVPool *pvp, int targ)		/* called with pvp locked */
{
	int i;
	PV *pv;

	for (i = 0; i < Npvsperpool; i++) {
		pv = pvp->pv[i];
		if (pv && pv->targ == targ)
			return pv;
	}
	return nil;
}
 
/*
 * Zero out the pvt on the metadata target. No need to call with PV
 * locked because addleg/xladdpv has the only reference, and
 * xladdtopool has not been called, yet, so nobody else can see it.
 */

int
initpvt(PV *p)
{
	vlong offset;
	ushort *a;
	int n, m, i;

	n = metaextents(p->npve, Xrefperblk);
	for (i=0; i<n; i++)
		p->ref[i] = REFused|1;
	offset = p->offset;
	offset += Xblk;		/* skip label block */
	a = p->ref;
	for (n = p->npve; n > 0; n -= Xrefperblk) {
		m = edio(p->mtarg, a, Xblk, offset, OWRITE);
		if (m != Xblk) {
			xsyslog("initpvt: write failure %T:%lld (%d)\n", p->mtarg, offset, m);
			return -1;
		}
		a += Xrefperblk;
		offset += Xblk;
	}
	return 0;
}	

static 
uchar*
pstring(uchar *p, char *s)
{
	uint n;

	if (s == nil){
		PBIT16(p, 0);
		p += BIT16SZ;
		return p;
	}

	n = strlen(s);
	/*
	 * We are moving the string before the length,
	 * so you can S2M a struct into an existing message
	 */
	memmove(p + BIT16SZ, s, n);
	PBIT16(p, n);
	p += n + BIT16SZ;
	return p;
}

/* 16-bit ones complement sum len bytes @ p, dufftastically!
 * note: assumes len%2 == 0
 */
ushort
onesum(uchar *p, ulong len)
{
	ulong sum = 0;
	ushort *s = (ushort *) p;
	int nw = len >> 1;
	uint n = (nw+7)/8;

	if (n | nw)
	switch (nw%8) {
	case 0:	do {	sum += *s++;
	case 7:		sum += *s++;
	case 6:		sum += *s++;
	case 5:		sum += *s++;
	case 4:		sum += *s++;
	case 3:		sum += *s++;
	case 2:		sum += *s++;
	case 1:		sum += *s++;
			} while (--n > 0);
	}
	sum = (sum >> 16) + (sum & 0xffff);
	return (sum >> 16) + (sum & 0xffff);
}

void
mkpvmeta(PV *p, uchar *buf)		/* called with p locked */
{
	uchar *b;
	ushort s;

	memset(buf, 0, Xblk);
	b = buf;
	memmove(b, VSPVMAGIC, 4);
	b += 8;				/* skip magic (4B) + checksum (4B) */
	PBIT32(b, p->state);
	b += BIT32SZ;
	PBIT32(b, p->ctime);
	b += BIT32SZ;
	PBIT32(b, p->flags & PVFmeta);
	b += BIT32SZ;
	PBIT32(b, p->targ);
	b += BIT32SZ;
	PBIT32(b, p->mirror);
	b += BIT32SZ;
	PBIT64(b, p->length);
	b += BIT64SZ;
	PBIT64(b, p->offset);
	b += BIT64SZ;
	PBIT32(b, p->npve);
	b += BIT32SZ;
	pstring(b, p->label);

	/* set cksum last */
	s = onesum(buf, Xblk);
	PBIT16(buf+4, s);
}

int
updpv(PV *p)	/* update a given pv in a pool. called with p locked */
{
	uchar buf[Xblk];		/* out buffer */
	int n;

	p->uid++;
	mkpvmeta(p, buf);
	n = pviox(p->meta, buf, Xblk, p->offset, OWRITE, 1);
	if (n == Xblk) {
		p->flags &= ~PVFBZ3318;
		return 0;
	}
	xsyslog("updpv: metadata write failure %T:%lld (%d)\n",
		p->meta->targ, p->offset, n);
	return -1;
}

void
printpv(PV *pv, int all)	/* show me what you got */
{
	int cnt, i;
	ushort prev;

	rlock(pv);
	print("pv: %p: ", pv);
	print("state=%x id=%d label=%q target=%T ",
		pv->state, pv->id, pv->label, pv->targ);
	print("meta=%T aoemirror=%T ctime=%uld length=%lld offset=%llx ",
		pv->mtarg, pv->mirror, 
		pv->ctime, pv->length, pv->offset);
	print("npve=%d\n", pv->npve);
	if (!all) {
		runlock(pv);
		return;
	}
	cnt = 0;
	prev = 0;
	for (i = 0; i < pv->npve; i++) {
		if (i > 0 && pv->ref[i] == prev) {
			cnt++;
			continue;
		}
		if (cnt > 0) {
			print("\trepeats %d times\n", cnt);
			cnt = 0;
		}
		prev = pv->ref[i];
		print("%3d %04ux\n", i, prev);
	}
	runlock(pv);
}

static 
void
printpool(PVPool *pvp)
{
	rlock(pvp);
	print("PVPool: %p: ", pvp);
	print("next=%p name=%q label=%q npv=%ld mpv=%d\n",
		pvp->next, pvp->name, pvp->label, pvp->npv, Npvsperpool);
	runlock(pvp);
}

static 
uchar*
gstring(uchar *p, uchar *ep, char **s)
{
	uint n;

	if (p+BIT16SZ > ep)
		return nil;
	n = GBIT16(p);
	p += BIT16SZ - 1;
	if (p+n+1 > ep)
		return nil;
	if (n == 0) {
		*s = strdup("");
		return p+1;
	}
	/* move it down, on top of count, to make room for '\0' */
	memmove(p, p + 1, n);
	p[n] = '\0';
	*s = strdup((char*)p);
	p += n+1;
	return p;
}

uint
convM2PV(uchar *b, uchar *ep, PV *pv)		/* called with pv locked */
{
	uchar *o;

	o = b;
	b += 8;	/* skip magic and checksum */
	pv->state = GBIT32(b);
	b += BIT32SZ;
	pv->ctime = GBIT32(b);
	b += BIT32SZ;
	pv->flags = GBIT32(b);
	b += BIT32SZ;
	pv->targ = GBIT32(b);
	b += BIT32SZ;
	pv->mirror = GBIT32(b);
	b += BIT32SZ;
	pv->length = GBIT64(b);
	b += BIT64SZ;
	pv->offset = GBIT64(b);
	b += BIT64SZ;
	pv->npve = GBIT32(b);
	b += BIT32SZ;
	b = gstring(b, ep, &pv->label);
	switch (pv->state) {
	case PVSresilver:
		pv->state = PVSoosync;
	case PVSoosync:
		mirrorck = 1;
	}
	if (pv->flags & ~PVFmeta)
		pv->flags = (pv->flags & PVFmeta) | PVFBZ3318;
	pv->sysalrt = 0;
	return b - o;
}

int
ispvmeta(int targ)
{
	uchar buf[Xblk];
	ushort s;

	if (edread(targ, buf, Xblk, 0) != Xblk) {
		uerr("cannot read metadata from %T\n", targ);
		return 0;
	}
	if (memcmp(buf, VSPVMAGIC, 4) != 0) {
		uerr("PV metadata extent %T missing header magic\n", targ);
		return 0;
	}
	s = GBIT16(buf+4);
	buf[4] = buf[5] = 0;
	if (onesum(buf, Xblk) != s) {
		uerr("invalid PV metadata checksum on %T\n", targ);
		return 0;
	}
	return 1;
}

int
xlloadpv(PV *pv, int pvt)		/* called with pv locked */
{
	uchar buf[Xblk], *e;
	int targ, ret;
	ushort s;

	ret = 1;
	targ = pv->mtarg;
	trestrict(targ);
	if (edio(targ, buf, Xblk, pv->offset, OREAD) != Xblk) {
		xsyslog("xlloadpv: cannot read metadata from %T\n", targ);
		return 0;
	}
	if (memcmp(buf, VSPVMAGIC, 4) != 0) {
		xsyslog("xlloadpv: PV metadata extent %T.%lld missing header magic\n", targ, pv->offset);
		return 0;
	}
	s = GBIT16(buf+4);
	buf[4] = buf[5] = 0;
	if (onesum(buf, Xblk) != s) {
		xsyslog("xlloadpv: Invalid PV metadata checksum on %T.%lld\n", targ, pv->offset);
		return 0;
	}
	e = &buf[sizeof buf];
	convM2PV(buf, e, pv);
	if (pv->ref == nil)
		pv->ref = vtalloc(pv->npve, sizeof *pv->ref);
	if (pv->offset == 0)
		pv->meta = pv;
	else if (!fixuplegacy(pv)) {
		pv->state = PVSmissing;
		pv->sysalrt = 1;
		return 0;
	}
	trestrict(pv->targ);
	trestrict(pv->mirror);
	if (pvt)
		ret = xlloadpvt(pv);
	return ret;
}

int
xlloadpvt(PV *pv)	/* read meta data for target, called with pv locked */
{
	PVPool *pvp = pv->pool;

	loadpvt(pv);
	if (pv->state == PVSmissing)	// fail.
		return 0;
	xaddu(&pvp->efree, cntfreerefs(pv));
	xaddu(&pvp->etotal, pv->npve);
	xaddu(&totalext, pv->npve);
	return 1;
}	

int
xlinitpv(PVPool *pvp, int i, int targ, int mirtarg, vlong offset)
{
	PV *pv;
	
	wlock(pvp);
	pvp->pv[i] = pv = mallocz(sizeof *pv, 1);
	if (pv == nil) {
		wunlock(pvp);
		return -1;
	}
	pv->pool = pvp;
	pv->id = i;
	xincu(&pvp->npv);
	pv->targ = offset ? -1 : targ;
	pv->mtarg = targ;
	pv->mirror = mirtarg;
	pv->offset = offset;
	pv->state = PVSmissing;
	pv->sysalrt = 1;
	pv->lm.pid = -1;
	wunlock(pvp);
	newpv(pv);
	return 0;
}

/*
 * Note: allocating space for ref tables
 * Elsewhere we write the table out in units
 * of Xblks.  We need to make sure that we have enough
 * space so that the last write doesn't fault.  We do this
 * by making sure the ref table is big enough that the last
 * write will never fault.
 */

static 
PV *
loadpvt(PV *pv)	/* put pv into pool, loading its pvt, called with pv locked */
{
	uchar *p;
	vlong ns, cnt, roff, xcnt, xtnt;
	XCB *x;

	ns = nsec();
	p = nil;
	xtnt = (pv->offset + Xblk) / Xextent;		// xtnt offset in PV
	roff = (pv->offset + Xblk) % Xextent;		// ref offset in xtnt
	cnt = roff + pv->npve * sizeof pv->ref[0];
	if (cnt % Xblk)
		cnt += (Xblk - (cnt % Xblk));

	for (x = xcballoc(); cnt > 0; xtnt++) {
		xcnt = cnt > Xextent ? Xextent : cnt;
		xcbinit(x, nil, pv->mtarg, xtnt, xcnt, nil, OREAD);
		xcbrun(x, 1);
		if (x->flag & Xfail) {
			xsyslog("loadpvt: %T failure at extent %lld\n",
				pv->mtarg, xtnt);
			pv->state = PVSmissing;
			pv->sysalrt = 1;
			break;
		}
		cnt -= xcnt;
		if (p)
			memcpy(p, x->xbuf, xcnt);
		else
			memcpy(p = (uchar *)pv->ref, x->xbuf + roff,
			       xcnt -= roff);
		p += xcnt;
	}
	xcbfree(x);
	if (0 && pv->state != PVSmissing)
		xsyslog(LOGCOM "loadpvt %T in %lld ns\n",
			pv->mtarg, nsec() - ns);
	return pv;
}

/* Shift legacy data from legacy PV to a new home */
static int
shiftlegacy(PV *pv)
{
	int i, pvmx;
	PVPool *pvp;

	pvp = pv->pool;
	pvmx = metaextents(pv->npve, Xrefperblk);
	wlock(pvp);
	pv->lm = newpve(pvp, nil, pvmx);
	wunlock(pvp);
	if (pv->lm.pid == -1)
		return 0;
	if (waserror()) {
		for (i = 0; i < pvmx; i++) {
			xincu(&pvp->efree);
			pvp->pv[pv->lm.pid]->ref[pv->lm.off+i] = REFnf;
		}
		rlock(pvp);
		flushrefs(pvp);
		runlock(pvp);
		return 0;
	}		
	/* Displace data to another PV, error out on first failure */
	for (i = 0; i < pvmx; i++) {
		pvp->pv[pv->lm.pid]->ref[pv->lm.off+i] |= REFused|REFnf|1;
		if (pvxtntcopy(pvp->pv[pv->lm.pid], pv->lm.off + i, pv, i) < 0) {
			error("extent copy failure");
		}
	}
	poperror();
	rlock(pvp);
	flushrefs(pvp);
	runlock(pvp);
	return 1;
}

static int
updlegpv(PV *pv)
{
	PVPool *pvp;
	int i, pvmx;


	if (pv == nil)
		return 0;
	if (pv->offset == 0)
		return 0;
	pvp = pv->pool;
	/* Copy PV meta at offset to new home */
	pvmx = metaextents(pv->npve, Xrefperblk);
	for (i = 0; i < pvmx; i++) {
		/* pv can't be locked when calling pvxtntcopy */
		if (pvxtntcopy(pv, i, pv->meta, pv->offset / Xextent + i) < 0) {
			xsyslog("updlegpv: failed to update PV metadata\n");
			rlock(pvp);
			flushrefs(pvp);
			runlock(pvp);
			return 0;
		}
		wlock(pv);
		pv->ref[i] = pv->meta->ref[pv->offset / Xextent + i] | REFnf;
		pv->meta->ref[pv->offset / Xextent + i] = REFnf;
		wunlock(pv);
		/* The new home for this data used up an extent, need to adjust efree here */
		xincu(&pvp->efree);
	}
	wlock(pv);
	pv->mtarg = pv->id;
	pv->offset = 0;
	pv->meta = pv;
	updpv(pv);
	wunlock(pv);
	rlock(pvp);
	flushrefs(pvp);
	runlock(pvp);
	xsyslog("PV %T: legacy metadata updated\n", pv->targ);
	writeconfig();
	return 1;
}

int
fixuplegacy(PV *pv)		/* called with pv locked */
{
	PVPool *pvp;
	PV *npv;
	int i;
	
	pvp = pv->pool;
	for (i = 0; i < Npvsperpool; i++) {
		npv = pvp->pv[i];
		if (npv == nil)
			continue;
		if (npv->targ == pv->mtarg ||
		   (npv->state == PVSdouble && npv->mirror == pv->mtarg)) {
			pv->meta = npv;
			xsyslog("Please run /updatelegacy to optimize legacy volumes\n");
			return 1;
		}
	}
	return 0;
}

/*
 * Use this function to fix up any LVs after an upgrade
 * Note that LV is not in the namespace yet
 */
static void
lvupgrade(LV *lv)
{
	if (releaselast == nil) 
		return;
	
	if (strcmp(releaselast, "unknown") != 0)
		return;

	/* If you've got thin extents you are now officially thin */
	if (lv->thin && (lv->mode & LVTHIN) == 0) {
		lv->mode |= LVTHIN;
		savemeta(lv, 0);
	}
}

/*
 * Return true if all pv's for l are there, called with l locked.
 * Does not need pvp locked because procs that call allpvs keep trying
 * until they find all pvs.
 */

static int
allpvs(PVPool *pvp, LV *l)
{
	int i;
	LVE e;
	PV *pv;

	l->dirty = 0;
	l->thin = 0;
	memset(l->exts, 0, sizeof(l->exts));
	for (i = 0; i < l->nlve; i++) {
		if (fetchlve(l, i, &e) == nil)
			return 0;
		if (e.pid >= Npvsperpool) {
			xsyslog("LV %s: invalid lve in allpvs: lve:%d flag:%02x pid:%d off:%ud\n",
				l->name, i, e.flag, e.pid, e.off);
			return 0;
		}
		pv = pvp->pv[e.pid];
		if (pv == nil || pv->state == PVSmissing || pv->flags & PVFlost)
			return 0;
		if (!validlve(&e, l)) {
			xsyslog("LV %s: got bad lve in allpvs: lve:%d flag:%02x pid:%d off:%ud\n",
				l->name, i, e.flag, e.pid, e.off);
			return 0;
		}
		if (e.flag & LFdirty)
			l->dirty++;
		if (e.flag & LFthin) {
			l->thin++;
			continue;
		}
		l->exts[e.pid]++;
	}
	return 1;
}

int
hasleg(void) /* Called with lk held */
{
	PVPool *pvp;
	PV* pv;
	int i;
	
	for (pvp = pools; pvp; pvp = pvp->next) {
		rlock(pvp);
		for (i = 0; i < Npvsperpool; i++) {
			pv = pvp->pv[i];
			if (pv == nil)
				continue;
			rlock(pv);
			if (pv->state == PVSmissing) {
				uerr("Please online all missing PVs");
				runlock(pv);
				runlock(pvp);
				return -1;
			}
			if (pv->flags & PVFlost) {
				uerr("Please find all lost PVs");
				runlock(pv);
				runlock(pvp);
				return -1;
			}
			if (targlen(pv->targ) == 0) {
				/* targlen sets uerr */
				runlock(pv);
				runlock(pvp);
				return -1;
			}
			if (pv->offset) {
				runlock(pv);
				runlock(pvp);
				return 1;
			}
			runlock(pv);
		}
		runlock(pvp);
	}
	return 0;
}

int
updleglvs(void) /* Called with lk held */
{
	LV *l;
	uint i, sl;
	LVE e, ne;
	PV* pv;
	PVPool *pvp;
	int pvmx, j;

	for (l = vols; l; l = l->next) {
		pvp = l->pool;
		sl = 0;
		wlock(l);
		if (waserror()) {
			wunlock(l);
			return 0;
		}
		for (i = 0; i < l->nlve; i++) {
			if (fetchlve(l, i, &e) == nil) {
				error("fetchlve failure LV: %s i: %ud\n",l->name, i);
			}
			if (!validlve(&e, l)) {
				error("got bad lve in updleglvs for %s: lve:%d flag:%02x pid:%d off:%ud\n",
				l->name, i, e.flag, e.pid, e.off);
			}
			pv = pvp->pv[e.pid];
			/* Update LVE to new home for shifted legacy data */
			if (pv->offset) {
				if (pv->lm.pid == -1) {
					if (shiftlegacy(pv) == 0) {
						error("Unable to shift legacy lun data to new PV\n");
					}
					sl++;
				}	
			}
			pvmx = metaextents(pv->npve, Xrefperblk);
			if (e.off < pvmx) {
				if (e.off == 0)
					xsyslog("LV %s: updating legacy extents\n", l->name);
				ne.flag = e.flag;
				ne.pid = pv->lm.pid;
				ne.off = pv->lm.off + e.off;
				l->exts[e.pid]--;
				l->exts[ne.pid]++;
				/* shiftlegacy will set PV refs to one for initial data
				   subsequent LVs must update refs here */
				if (sl == 0) {
					wlock(pvp->pv[ne.pid]);
					pvp->pv[ne.pid]->ref[ne.off]++;
					pvp->pv[ne.pid]->ref[ne.off] |= REFnf;
					wunlock(pvp->pv[ne.pid]);
				}
				setlve(l, i, &ne);
				if (REFCNT(pv->ref[e.off]) > 1) {
					wlock(pv);
					pv->ref[e.off]--;
					pv->ref[e.off] |= REFnf;
					wunlock(pv);
				/* don't let ref cnt drop to zero
				   someone else might grab the extents
				   after all cnts are 1 it is safe to
				   to update the PV metadata */
				} else {
					for (j = 0; j < pvmx; j++)
						if(REFCNT(pv->ref[j]) != 1)
							break;
					if (j == pvmx)
						updlegpv(pv);
				}
				rlock(pvp);
				flushpve(pv, e.off);
				flushpve(pvp->pv[ne.pid], ne.off);
				runlock(pvp);
			}
		}
		poperror();
		wunlock(l);		
	}
	return 1;
}

static
LV *
knownlv(PVPool *pvp, PV *pv, int extent)	/* called with lk locked */
{
	LV *l;

	for (l=vols; l; l=l->next) {
		if (l->pool == pvp)
		if (pv->id == l->lve[0].pid)
		if (l->lve[0].off == extent) {
			return l;
		}
	}
	return nil;
}

void
freelv(LV *l)
{
	if (l) {
		free(l->name);
		free(l->label);
		delfilter(&l->rf);
		delfilter(&l->wf);
		if (l->lve)
			free(l->lve);
		free(l->rmtname);
		free(l->rmtlv);
		free(l->b);
		free(l);
	}
}

static 
void
check4lv(PVPool *pvp, PV *pv)		/* called with lk */
{
	ushort *r;
	int i, free;
	LV *l;
	ulong ldlverrs;

	ldlverrs = pv->ldlverrs;
	free = 0;
	r = pv->ref;
	for (i = 0; i < pv->npve; i++, r++) {
		if (REFCNT(*r) == 0)
			free++;
		if ((*r & REFlvt) == 0)
			continue;
		l = knownlv(pvp, pv, i);
		if (l == nil)
		if ((l = loadmeta(pv, i)) == nil)
			continue;
		wlock(l);
		if ((l->flags & LVFpartial) == 0) {
			wunlock(l);
			continue;
		}
		togbsyslog("LV %s metadata found on %T, searching backing PVs\n", l->name, pv->mtarg);
		if (!allpvs(pvp, l)) {
			togbsyslog("LV %s missing backing PVs\n", l->name);
			if (time(0) - l->loadtime > 60*2) {
				xsyslog("Warning: LV %s: missing backing PVs after 2 minutes\n", l->name);
				l->flags |= LVFwarned;
			}
			wunlock(l);

			continue;
		}
		togbsyslog("LV %s PVs available, loading into memory\n", l->name);
		if (l->flags & LVFwarned)
			xsyslog("LV %s: component PVs available, loading LV\n", l->name);
		l->flags &= ~(LVFwarned|LVFpartial);
		lvupgrade(l);
		wunlock(l);
		snapcal(l);
	}
	if (free == 0)
		pv->flags |= PVFfull;
	if (ldlverrs != pv->ldlverrs)
		xsyslog("loadmeta: %T had %uld errors\n",
			pv->targ, pv->ldlverrs - ldlverrs);
}

void
xlcheck4lvs(PVPool *pvp)		/* called with lk locked*/
{
	int i;

	for (i=0; i<Npvsperpool; i++) {
		if (pvp->pv[i] == nil || pvp->pv[i]->state == PVSmissing)
			continue;
		check4lv(pvp, pvp->pv[i]);
	}
}

/* Set err on error */
vlong
atosize(char *s, int *err)	/* digits*[KkGgMmTt] */
{
	double size;
	char *p;
	
	*err = 0;
	size = strtod(s, &p);
	switch(*p) {
		case 'T':
		case 't':
			size *= 1000;
		case 'G':
		case 'g':
			size *= 1000;
		case 'M':
		case 'm':
			size *= 1000;
		case 'K':
		case 'k':
			size *= 1000;
			*err = *++p;
		case 0:
			break;
		default:
			*err = 1;
			size = 0;
	}
	return (vlong)size;
}


void
setserial(LV *l)	/* called with l locked */
{
	char buf[1024];
	uchar digest[MD5dlen];
	int len;

	len = snprint(buf, 1024, "%s %ld %lld", l->name, l->nlve, nsec());
	md5((uchar*)buf, len, digest, nil);
	snprint(l->serial, sizeof l->serial, "%.*lH", 10, digest);
}

void
xlmklv(PVPool *pvp, char *name, char *size, int thin)
{
	vlong length;
	uint nxts;
	int serr, frstdat;
	LV *lv;

	if (invalidname(name))	/* sets u->err */
		return;
	wlock(&lk);
	if (waserror()) {
		wunlock(&lk);
		return;
	}
	lv = lookuplv(name);
	if (lv)
		error("LV %s already exists in pool %s", name, lv->pool->name);
	length = atosize(size, &serr);
	if (length <= 0 || serr)
		error("Invalid size: %s",size);	
	nxts = (length+Xextent-1) / Xextent;
	frstdat = metaextents(nxts, Xlveperblk);
	if (thin && frstdat > poolspace(pvp)) 
		error("LV %s requires %.3f GB of space", name, EXT2GB(frstdat));	
	if (thin == 0 && nxts + frstdat > poolspace(pvp))
		error("LV %s requires %.3f GB of space", name, EXT2GB(nxts + frstdat));
	createlv(pvp, name, length, nxts, thin);	/* sets u->err on error */
	poperror();
	wunlock(&lk);
}

/*
 * Createlv creates the logical volume and the metadata extents to save
 * its description.  We have to allocate 1 extent for each extent of
 * data, plus the extents to keep metadata.  The first 8K keeps a copy of
 * the LV structure and the remaing sectors of the extent keep the
 * LVE structures.  All metadata extents ignore the first 16 sectors, so
 * the math is easy.  The frstdat field indicates where the data starts.
 * 
 */

static 
int
createlv(PVPool *pvp, char *name, vlong length, uint nxts, int thin)		/* called with lk locked */
{
	LV *l;
	int frstdat;
	char *err;

	l = nil;
	if (waserror()) {
		freelv(l);
		return -1;
	}
	frstdat = metaextents(nxts, Xlveperblk);
	l = mallocz(sizeof *l, 1);
	if (l == nil)
		error("allocation failure for LV");
	l->label = strdup("");
	l->name = strdup(name);
	if (l->label == nil || l->name == nil)
		error("allocation failure for LV elements");
	wlock(l);
	rlock(pvp);
	if (waserror()) {
		runlock(pvp);
		wunlock(l);
		nexterror();
	}
	l->lun = -1;
	l->snaplimit = SLign;
	l->length = length;
	l->mode = 0600;			/* read-write volume */
	l->mode |= thin ? LVTHIN : 0;
	l->ctime = time(nil);
	l->pool = pvp;
	l->nqc = 0;
	l->nlve = frstdat+nxts;
	l->frstdat = frstdat;
	setserial(l);
	if (err = alloclvt(l, nil, nil, thin ? AThin : 0))
		error("unable to allocate extents for LV: %s", err);
	if (waserror()) {
		decrefs(l);
		flushrefs(pvp);
		nexterror();
	}
	if (savemeta(l, LVWIP) < 0) {
		error("failure writing LV metadata");
	}
	if (flushrefs(pvp) < 0) {
		error("failure writing pool PV Table state");
	}
	if (savemeta(l, 0) < 0)
		error("failure writing LV metadata");
	l->next = vols;
	vols = l;
	poperror();
	poperror();
	poperror();
	runlock(pvp);
	wunlock(l);
	newlv(l);
	xsyslog("LV %s: %s provisioned LV created in pool %s\n", name, thin ? "thin" : "thick", pvp->name);
	return 0;
}

static 
void
dumpref(ushort *r, int len)	/* print some of the refs */
{
	int i;
	
	i = 0;
	while (len-- > 0) {
		print("%2d: %c%c%c%c %d\n", i,
			(*r & REFused) ? 'u' : '-',
			(*r & REFdirty) ? 'd' : '-',
			(*r & REFlvt) ? 'l' : '-',
			(*r & REFnf) ? 'f' : '-',
			*r & 07777);
		r++;
		i++;
	}
}

static void
dellru(LVEBUF *p)
{
	if (p == lruhead)
		lruhead = p->nlru;
	if (p == lrutail)
		lrutail = p->plru;
	if (p->nlru)
		p->nlru->plru = p->plru;
	if (p->plru)
		p->plru->nlru = p->nlru;
	p->nlru = nil;
	p->plru = nil;
}

static void
inslruhead(LVEBUF *p)
{
	if (lruhead)
		lruhead->plru = p;
	p->nlru = lruhead;
	lruhead = p;
	if (lrutail == nil)
		lrutail = p;
}
	
static void
mvhead(LVEBUF *p)
{
	if (p == lruhead)
		return;
	dellru(p);
	inslruhead(p);
}

static LVEBUF *
getlveblk(LV *src, int n)		/* called with src and lvecache locked */
{
	LVEBUF *p;
	vlong offset;
	int i, m, mb, me;

	mb = n / Xlveperblk + 1;
	for (p = lruhead; p; p = p->nlru) {
		if (p->lv == src && p->blkno == mb)
			break;
	}
	if (!p) {
		for (i = 0; i < Nlvebuf && lvecache[i] && (lvecache[i]->flags & LVEBpresent); ++i) ;
		if (i >= Nlvebuf)
			p = lrutail;
		else {
			if (lvecache[i] == nil) {
				p = lvecache[i] = calloc(1, sizeof (LVEBUF));
				if (p == nil)
					p = lrutail;
				inslruhead(p);
			}
			else
				p = lvecache[i];
		}
		me = mb / Xblkperext;
		if (!validlve(src->lve + me, src)) {
			xsyslog("LV %s: got bad lve in getlveblk: lve:%d flag:%02x pid:%d off:%ud\n",
				src->name, me, src->lve[me].flag, src->lve[me].pid, src->lve[me].off);
			return nil;
		}
		if (src->lve[me].flag & LFthin) {
			xsyslog("LV %s: attempting to read thin metadata extent for lve:%d\n", src->name, me);
			return nil;
		}
		offset = src->lve[me].off * Xextent + (mb % Xblkperext) * Xblk;
		m = pvio(src->pool->pv[src->lve[me].pid], p->buf, Xblk, offset, OREAD);
		if (m != Xblk) {
			xsyslog("getlveblock failed: LV:%s lve:%d pid:%d\n", src->name, n, src->lve[me].pid);
			p->flags = 0;
			return nil;
		}
		p->flags = LVEBpresent;
		p->blkno = mb;
		p->lv = src;
	}
	return p;
}

static int
putlveblk(LV *src, int n, uchar *buf)		/* called with src and lvecache locked */
{
	vlong offset;
	int mb, me;

	mb = n / Xlveperblk + 1;
	me = mb / Xblkperext;
	if (!validlve(src->lve + me, src)) {
		xsyslog("got bad lve in putlveblk for %s: lve:%d flag:%02x pid:%d off:%ud\n",
			src->name, me, src->lve[me].flag, src->lve[me].pid, src->lve[me].off);
		return -1;
	}
	if (src->lve[me].flag & LFthin) {
		xsyslog("attempting to write thin metadata extent for %s lve:%d\n", src->name, me);
		return -1;
	}
	offset = src->lve[me].off * Xextent + (mb % Xblkperext) * Xblk;
	n = pvio(src->pool->pv[src->lve[me].pid], buf, Xblk, offset, OWRITE);
	if (n != Xblk)
		return -1;
	return 0;
}

static void
invalblk(LV *src)
{
	int i;

	if (!src)
		return;
	qlock(&lclock);
	for (i = 0; i < Nlvebuf && lvecache[i]; ++i) {
		if (lvecache[i] && (lvecache[i]->flags & LVEBpresent)
				&& lvecache[i]->lv == src) {
			lvecache[i]->flags = 0;
			lvecache[i]->lv = nil;
			lvecache[i]->blkno = 0;
			dellru(lvecache[i]);
		}
	}
	qunlock(&lclock);
}

/* return number of used extents in LV */
ulong
usedextents(LV *lv) {	/* LV should be locked */
	ulong ext, i;
	LVE e;

	ext = 0;
	for (i = 0; i < lv->nlve; i++) {
		if (fetchlve(lv, i, &e) == nil) {
			xsyslog("usedextents: unable to fetch LVE\n");
			return ext;
		}
		if (e.off == -1)
			continue;
		if (lv->pool->pv[e.pid]->ref[e.off] & REFused)
			ext++;
	}
	return ext;
}


/* Called with lv1 and lv2 locked 
    Note that this compares and returns a count of all LVEs 
    in a blk */
static int
blkscan(LV *lv1, LV *lv2, ulong ext) {
	LVE *e1, *e2;
	LVEBUF *b1, *b2, *b3;
	int nse, i, start;
	ulong shift;

	shift = lv2->frstdat - lv1->frstdat;
	nse = 0;
	qlock(&lclock);
	b1 = getlveblk(lv1, ext);
	b2 = getlveblk(lv2, ext);
	b3 = nil;
	if (b1 == nil || b2 == nil) {
		qunlock(&lclock);
		xsyslog("blkscan: b1: 0x%p b2: 0x%p\n", b1, b2);
		return -1;
	}
	start = ext == 0 ? lv1->frstdat : 0;
	for (i = start; i < Xlveperblk; i++) {
		if (ext + i >= lv1->nlve) {
			qunlock(&lclock);
			return nse;
		}
		if (i + shift >= Xlveperblk) {
			if (b3 == nil) {
				if ((b3 = getlveblk(lv2, ext + Xlveperblk)) == nil) {
					qunlock(&lclock);
					xsyslog("blkscan: b3: 0x%p\n", b3);
					return -1;
				}
			}
			e2 = (LVE *)b3->buf + ((i + shift) % Xlveperblk);
		} else 
			e2 = (LVE *)b2->buf + i + shift;
		
		e1 = (LVE *)b1->buf + i;
		
		if (!validlve(e1, lv1)) {
			xsyslog("blkscan: lv1 %s invalid lve %uld: flag:%02x pid 0x%ux off 0x%ux\n", lv1->name, ext + i, e1->flag, e1->pid, e1->off);
			qunlock(&lclock);
			return nse;
		}
		if (!validlve(e2, lv2)) {
			xsyslog("blkscan: lv2 %s invalid lve %uld: flag:%02x pid 0x%ux off 0x%ux\n", lv1->name, ext + i + shift, e2->flag, e2->pid, e2->off);
			qunlock(&lclock);
			return nse;
		}
		if (e1->pid == e2->pid && e1->off == e2->off)
			continue;
		if (e1->off == -1 || e2->off == -1)
			continue;
		nse++;
	}
	mvhead(b1);
	mvhead(b2);
	if (b3) {
		mvhead(b3);
	}
	qunlock(&lclock);
	return nse;
}

/* 
  * This determines if any snaps are missing by checking prevsnap.
  * NOTE: only lists created with LVSignsch should be verified with
  * this function. Any other lists may be inconsistent by design.
  */
int
verifysnaplist(LV *l, LVL *op)
{
	ulong prev;
	LVL *p;
	
	prev = 0;
	for (p = op; p; p = p->next) {
		if (p->l->flags & LVFpartial) {
			return -1;
		}
		if (p->l->prevsnap != prev) {
			return -1;
		}
		prev = snapname2num(p->l->name);
		if (prev == 0) {
			xsyslog("verifysnaplist: %s\n", u->err);
			return -1;
		}
	}
	if (l->prevsnap != prev) {
		return -1;
	}
	return 0;
}

/* 
  * This will take an LV or snapshot and attempt snapshot usage calculations iff all
  * snapshots are present in the system.
  * When calculations are finished all snaps and LV will be added to filesystem
  */
static void
snapcal(LV *lv) /* lv should not be locked, wlock on lk should be held */
{
	int n, nse;
	LV *s1, *s2;
	LVL *p, *op;
	ulong end, i;

	if ((lv->mode & LVSNAP) == 0) {
		if (lv->prevsnap == 0) {
			wlock(lv);
			newlv(lv);
			if (lv->mode & LVLUN) {
				newlun(lv);
				luns[lv->lun] = lv;
				if (lv->mode & LVONL)
					tannounce(lv);
			}
			wunlock(lv);
			togbsyslog("LV %s added to namespace\n", lv->name);
			return;
		}
	} else {
		s1 = lv;
		lv = snaptolv(lv->name);
		if (lv == nil) {
			togbsyslog("LV %s parent not in memory\n", s1->name);
			return;
		}
	}
	n = snaplist(lv, &op, nil);
	if (n < 1) {
e:		togbsyslog("LV %s missing snaps in memory, not adding to namespace\n", lv->name);
		return;
	}
	if (verifysnaplist(lv, op) < 0) {
		freelvl(op);
		goto e;
	}
	lv->nse = 0;
	end = (lv->nlve + Xlveperblk - 1) / Xlveperblk * Xlveperblk;
	for (p = op; p; p = p->next) {
		s1 = p->l;
		if (p->next == nil) {
			s2 = lv;
		} else {
			s2 = p->next->l;
		}
		if (dosnapcal || s1->nse == 0 || issnap(s2) == 0) {
			if (dosnapcal)
				xsyslog("dosnapcal: Calculating snap usage on %s\n", s1->name);
			for (i = 0; i < end; i += Xlveperblk) {
				if (i == 0) {
					s1->nse = s1->frstdat;
					lv->nse += s1->frstdat;
				}
				if (i < s1->nlve) {
					if ((nse = blkscan(s1, s2, i)) > 0) {
						s1->nse += nse;
						lv->nse += nse;
					} else if (nse < 0) {
						xsyslog("snapcal: error in blkscan: cannot load %s\n", lv->name);
						freelvl(op);
						return;
					}
				}
			}
			savemeta(s1, 0); /* no need to calculate next boot */
		} else {
			lv->nse += s1->nse;
		}	
	}
	wlock(lv);
	newlv(lv);
	if (lv->mode & LVLUN) {
		newlun(lv);
		luns[lv->lun] = lv;
		if (lv->mode & LVONL)
			tannounce(lv);
	}
	lv->flags |= LVFallsnaps;
	if (isshadowsend(lv))
		snapcopyck(1);
	wunlock(lv);
	for (p = op; p; p = p->next) {
		wlock(p->l);
		newlv(p->l);
		wunlock(p->l);
	}
	freelvl(op);
	togbsyslog("LV %s with %d snaps added to namespace\n", lv->name, n);
}


int
validlve(LVE *lve, LV *lv)
{
	PVPool *pool;
	PV *pv;

	if (lve->flag & ~(LFthin | LFdirty | LFnf))
		return 0;
	pool = lv->pool;
	if (lve->pid > Npvsperpool)
		return 0;
	if (lve->flag & LFthin)
		return 1;
	pv = pool->pv[lve->pid];
	if (pv == nil)
		return 0;
	if (lve->off > pv->npve)
		return 0;
	if ((lv->mode & LVLEG) || pv->offset != 0)
		return 1;
	if (lve->off < (pv->npve * 2 + Xblk + Xextent - 1) / Xextent)
		return 0;
	return 1;
}

/* always keep LVEs up to firstdat in memory */
LVE *
fetchlve(LV *src, ulong n, LVE *lve)		/* called with src locked */
{
	LVEBUF *p;
	LVE *q;
	int k;

	if (n > src->nlve) {
		xsyslog("Warning: attempt to read invalid LVE: LV:%s lve #%uld\n", src->name, n);
		return nil;
	}
	if (n < src->frstdat) {
		q = src->lve + n;
		if (lve)
			e2e(lve, q);
		return q;
	}
	qlock(&lclock);
	p = getlveblk(src, n);
	if (p == nil) {
		qunlock(&lclock);
		return nil;
	}
	k = n % Xlveperblk;
	q = (LVE *)p->buf + k;
	if (lve)
		e2e(lve, q);
	mvhead(p);
	qunlock(&lclock);
	if (!validlve(q, src)) {
		xsyslog("LV %s: invalid lve %uld: flag:%02x pid 0x%ux off 0x%ux\n", src->name, n, q->flag, q->pid, q->off);
		return 0;
	}
	return q;
}

int
setlve(LV *src, ulong n, LVE *lve)		/* called with src locked */
{
	LVEBUF *p;
	LVE *q;
	uchar buf[Xblk];
	int k;

	if (n > src->nlve) {
		xsyslog("Warning: Attempt to set invalid LVE: LV:%s lve #%uld\n", src->name, n);
		return -1;
	}
	qlock(&lclock);
	if (n < src->frstdat) {
		e2e(&src->lve[n], lve);
		memset(buf, 0, Xblk);
		k = (n / Xlveperblk) * Xlveperblk;
		memmove(buf, &src->lve[k], Xlveperblk * sizeof (LVE));
		putlveblk(src, n, buf);
	}
	else {
		p = getlveblk(src, n);
		if (p == nil) {
			qunlock(&lclock);
			return -1;
		}
		k = n % Xlveperblk;
		q = (LVE *)p->buf;
		e2e(&q[k], lve);
		mvhead(p);
		putlveblk(src, n, p->buf);
	}
	qunlock(&lclock);
	return 0;
}

/*
 * We need a new extent from the pool.  We would
 * rather have it on 'npv' but if not, take it from
 * emptiest physical volume.
 */

PVE
newpve(PVPool *pvp, PV **npv, int span)		/* called with pvp locked */
{
	PV *pv;
	PVE pve;

	pve.pid = -1;
	if (span <= 0)
		return pve;
	pv = npv ? *npv : nil;
	for (;; pv = nil) {
		if (pv == nil && (pv = emptiest(pvp)) == nil)
			return pve;
		if (pv->flags & (PVFlost|PVFfull))
			continue;
		pve = allocpve(pvp, pv, span);
		if (pve.pid != -1)
			break;
		if (span == 1)
			pv->flags |= PVFfull;
	}
	if (npv)
		*npv = pv;
	return pve;
}

PVE
pve1stripe(PVPool *pvp, PV **npv)		/* called with pvp locked */
{
	int i, n;
	PV *pv;
	PVE pve;

	pve.pid = -1;
	i = pvp->rrobin;
	n = 0;
	pv = nil;
	while (n < pvp->npv) {
		if (++i == Npvsperpool)
			i = 0;
		pv = pvp->pv[i];
		if (pv == nil)
			continue;
		if (pv->flags & (PVFlost|PVFfull)) {
			n++;
			continue;
		}
		pve = allocpve(pvp, pv, 1);
		if (pve.pid != -1) {
			pvp->rrobin = i;
			break;
		}
		pv->flags |= PVFfull;
		n++;
	}
	if (npv)
		*npv = pv;
	return pve;
}

PVE
newpve1(PVPool *pvp, PV **npv, int stripe)		/* called with pvp locked */
{
	if (stripe && (pvp->flags & PVPFstriped))
		return pve1stripe(pvp, npv);
	else
		return newpve(pvp, npv, 1);
}

static int
allocpve1(LVE *lve, PVPool *pvp, PV **npv, int stripe)	/* called with pvp locked */
{
	PVE pve;

	pve = newpve1(pvp, npv, stripe);
	if (pve.pid == -1) {
		xsyslog("allocpve1: out of space in pool %s\n", pvp->name);
		/* unroll and deallocate PVEs */
		return -1;
	}
	lve->pid = pve.pid;
	lve->off = pve.off;
	lve->flag = LFnf;
	return 0;
}

void
stripe(PVPool *pvp, char *s)	/* set/clear pool striping */
{
	int r;

	r = 0;
	if (strcmp(s, Prr) == 0)
		r = 1;
	else if (strcmp(s, Pct)) {
		uerr("%s is not a valid pool mode [%s | %s]", s, Prr, Pct);
		return;
	}
	wlock(pvp);
	if (waserror()) {
		wunlock(pvp);
		return;
	}
	if ((r && (pvp->flags & PVPFstriped)) ||
	   (r == 0 && (pvp->flags & PVPFstriped) == 0))
		error("Pool %s already %s", pvp->name, r ? Prr : Pct);
	if (r)
		pvp->flags |= PVPFstriped;
	else 
		pvp->flags &= ~PVPFstriped;
	poperror();
	wunlock(pvp);
	writeconfig();
	xsyslog("Pool %s: mode %s\n", pvp->name, r ? Prr : Pct);
}

/* This will free extent refs that are still sitting in buf */
static void
cleanlvt(LV *l, uchar *buf, int n)
{
	LVE* lve;
	int j;

	j = 0;
	if (n)
		j = (n - 1) / Xlveperblk * Xlveperblk;
	for (lve = (LVE*)buf; j < n; j++, lve++) {
		if (j >= l->frstdat)
			freelve(l->pool, l, j, lve);
	}
}

/*
 * alloc PVEs for LV
 */
char * 
alloclvt(LV *l, LV *src, PV *pv, int flags)		/* called with lk, l, l->pool and src locked */
{
	LVEBUF *sbuf;
	LVE lve2;
	LVE *lve, *lvte;
	PVPool *pvp;
	vlong offset;
	int n, k, mb, me, r, shift, saidit;
	uchar buf[Xblk];

	pvp = l->pool;
	lvte = (LVE *)buf + Xlveperblk;
	shift = 0;
	if (src)
		shift = l->frstdat - src->frstdat;
	saidit = 0;
	l->dirty = 0;
	l->thin = 0;
	/*
	 * See how many new extents we'll need from the pool.  Because
	 * the pool is locked the calls to allocpve1 below should never
	 * fail, but just in case, we'll keep the error handling
	 */
	if (src)
		n = l->nlve - (src->nlve - src->frstdat);
	else if (flags & AThin)
		n = l->frstdat;
	else
		n = l->nlve;
	if (n > pvp->efree)
		return Epoolfull;
	n = (l->frstdat + Xlveperblk - 1) / Xlveperblk;
	l->lve = vtalloc(n, Xblk);
	if (l->lve == nil) {
		xsyslog("Memory allocation failure in alloclvt\n");
		return Ebadalloc;
	}
	for (n = 0, mb = 1; n < l->nlve; ++mb) {
		me = mb / Xblkperext;
		if (me >= l->frstdat) {
			xsyslog("internal error trying to put metatdata into data space on %s\n", l->name);
			return EnoLVE;
		}
		offset = l->lve[me].off * Xextent + (mb % Xblkperext) * Xblk;
		memset(buf, 0, Xblk);
		for (lve = (LVE *)buf; lve < lvte && n < l->nlve; ++n, ++lve) {
			if (n < l->frstdat) {
				if (allocpve1(lve, pvp, &pv, 0) < 0) {
					if (n % Xblkperext != 0) {
						offset = l->lve[me].off * Xextent + (mb % Xblkperext) * Xblk;
						pvio(l->pool->pv[l->lve[me].pid], buf, Xblk, offset, OWRITE);
					}
					l->nlve = n;
					decrefs(l);
					if (src && l->mode & LVSNAP) {
							src->nse -= l->nse;
					}
					return Epoolfull;
				}
				if (!validlve(lve, l)) {
					xsyslog("got bad lve from allocpve1 in alloclvt for %s: lve:%d flag:%02x pid:%d off:%ud\n",
						l->name, n, lve->flag, lve->pid, lve->off);
					return EnoLVE;
				}
				if (l->mode & LVSNAP) {
					l->nse++;
					if (src)
						src->nse++;
				}
				lve->flag &= ~LFnf;
				lve->flag |= LFdirty;
				pvp->pv[lve->pid]->ref[lve->off] |= REFnf|REFused;
				l->exts[lve->pid]++;
				switch (pvp->pv[lve->pid]->state) {
				case PVSbroke:
				case PVSoosync:
					pvp->pv[lve->pid]->ref[lve->off]
						|= REFdirty;
				}
				if (n == 0)
					pvp->pv[lve->pid]->ref[lve->off] |= REFlvt;
				e2e(l->lve + n, lve);
			}
			else if (src && n - shift < src->nlve) {
				if (!fetchlve(src, n - shift, lve))
					return EnoLVE;
				if (lve->pid == 0 && lve->off == 0 && !saidit && !(lve->flag & LFthin)) {
					xsyslog("Warning: fetchlve for %s: %d returned (0,0)\n", src->name, n-shift);
					saidit = 1;
				}
				e2e(&lve2, lve);
				if (!(lve2.flag & LFthin)) {
					l->exts[lve2.pid]++;
					if (lve2.flag & LFdirty) {
						if (!(flags & AClrDirty))
							lve->flag &= ~LFdirty;
					}
					if (flags & AMkDirty)
						lve->flag |= LFdirty;
					r = pvp->pv[lve2.pid]->ref[lve2.off];
					if((flags & AFreeOrphan) && (r & REFused) == 0) {
						l->exts[lve2.pid]--;
						lve->flag = LFthin;
						lve->pid = 0;
						lve->off = ~0;
					}
					else {
						if(!(flags & ANoInc))
							incpvref(pvp, lve);
					}
				}
			}
			else {
				if (flags & AThin) {
					lve->flag = LFthin;
					lve->pid = 0;
					lve->off = ~0;
				}
				else {
					if (allocpve1(lve, pvp, &pv, 1) < 0) {
						if (n % Xblkperext != 0) {
							offset = l->lve[me].off * Xextent + (mb % Xblkperext) * Xblk;
							pvio(l->pool->pv[l->lve[me].pid], buf, Xblk, offset, OWRITE);
						}
						l->nlve = n;
						for (k = 0; k < n; ++k) {
							if (!(src && k-shift < src->nlve && (flags & ANoInc))) {
								if (fetchlve(l, k, &lve2) != nil)
									freelve(l->pool, l, k, &lve2);
							}
						}
						decrefs(l);
						cleanlvt(l, buf, n);
						return Epoolfull;
					}
					if (!validlve(lve, l)) {
						xsyslog("got bad lve from allocpve1 in alloclvt for %s: lve:%d flag:%02x pid:%d off:%ud\n",
							l->name, n, lve->flag, lve->pid, lve->off);
						decrefs(l);
						cleanlvt(l, buf, n);
						return EnoLVE;
					}
					lve->flag &= ~LFnf;
					pvp->pv[lve->pid]->ref[lve->off] |= REFnf;
					l->exts[lve->pid]++;
					if (flags & AMkDirty)
						lve->flag |= LFdirty;
				}
			}
			if (lve->flag & LFdirty)
				l->dirty++;
			if (lve->flag & LFthin)
				l->thin++;
		}
		if (!validlve(l->lve + me, l)) {
			xsyslog("bad lve for %s: lve:%d flag:%02x pid:%d off:%ud\n",
				l->name, me, l->lve[me].flag, l->lve[me].pid, l->lve[me].off);
			decrefs(l);
			cleanlvt(l, buf, n);
			return EnoLVE;
		}
		offset = l->lve[me].off * Xextent + (mb % Xblkperext) * Xblk;
		k = pvio(l->pool->pv[l->lve[me].pid], buf, Xblk, offset, OWRITE);
		if (k != Xblk) {
			xsyslog("alloclvt: failure writing lvt %T:%lld (%d) for %s\n", l->pool->pv[l->lve[me].pid]->targ, offset, n, l->name);
			/* decrefs will take care of LVEs written to disk, and meta LVEs */
			decrefs(l);
			cleanlvt(l, buf, n);
			return Ewlvt;
		}

		offset += Xblk;
	}
	if (src && flags & AClrDirty) {
		qlock(&lclock);
		for (n = src->frstdat; n < src->nlve; ) {
			sbuf = getlveblk(src, n);
			if (sbuf == nil) {
				xsyslog("internal error retrieving LV metadata on %s\n", src->name);
				qunlock(&lclock);
				decrefs(l);
				return EnoLVE;
			}
			lve = (LVE *)sbuf->buf + (n % Xlveperblk);
			lvte = (LVE *)sbuf->buf + Xlveperblk;
			for (; n < src->nlve && lve < lvte; ++n, ++lve) {
				if (lve->flag & LFdirty) {
					src->dirty--;
					lve->flag &= ~LFdirty;
				}
			}
			putlveblk(src, n-1, sbuf->buf);
		}
		qunlock(&lclock);
	}
	return nil;
}

/* kill LV metadata so that this volume doesn't pop up later */
int
zmeta(LV *l)		/* called with l locked */
{
	uchar buf[Xblk];

	if (!validlve(l->lve, l)) {
		xsyslog("bad lve0 for %s: flag:%02x pid:%d off:%ud\n",
			l->name, l->lve[0].flag, l->lve[0].pid, l->lve[0].off);
		return -1;
	}
	if (l->lve[0].flag & LFthin) {
		xsyslog("lve0 is thin on %s\n", l->name);
		return -1;
	}
	memset(buf, 0, sizeof buf);
	if (pvio(l->pool->pv[l->lve[0].pid], buf, Xblk, l->lve[0].off * Xextent, OWRITE) != Xblk)
		return -1;
	return 0;
}

/* The wip indicates this lun is a work in progress */	
int
savemeta(LV *l, ulong wip)	/* write lv metadata to storage, called with l locked */
{
	uchar buf[Xblk];
	uchar *p;
	vlong offset;
	int i, n;
	ushort s;

	memset(buf, 0, sizeof buf);
	p = buf;
	memmove(p, VSLVMAGIC, 4);
	p += 8;				/* skip magic (4B) + checksum (4B) */
	PBIT32(p, (wip | l->mode) & LVSAVMSK);
	p += BIT32SZ;
	PBIT32(p, l->lun);
	p += BIT32SZ;
	PBIT64(p, l->length);
	p += BIT64SZ;
	PBIT32(p, l->ctime);
	p += BIT32SZ;
	PBIT32(p, -1); // set starg inactive for obsolete AoE shadow
	p += BIT32SZ;
	PBIT32(p, l->snaps);
	p += BIT32SZ;
	PBIT32(p, l->vlan);
	p += BIT32SZ;
	PBIT32(p, l->nqc);
	p += BIT32SZ;
	PBIT32(p, l->nlve);
	p += BIT32SZ;
	PBIT32(p, l->frstdat);
	p += BIT32SZ;
	p = pstring(p, l->name);
	p = pstring(p, l->pool->name);
	p = pstring(p, l->label);
	memmove(p, l->qc, 1024);
	p += 1024;
	PBIT32(p, l->nmmac);
	p += BIT32SZ;
	for (i = 0; i < l->nmmac; i++) {
		memmove(p, l->mmac[i], 6);
		p += 6;
	}
	PBIT32(p, l->prevsnap);
	p += BIT32SZ;
	memmove(p, l->serial, 20);
	p += 20;
	PBIT64(p, l->snaplimit);
	p += BIT64SZ;
	for (i = 0; i < Nsched; i++) {
		PBIT16(p, l->sched[i].retain);
		p += BIT16SZ;
		PBIT8(p, l->sched[i].class);
		p += BIT8SZ;
		PBIT8(p, l->sched[i].mon);
		p += BIT8SZ;
		PBIT8(p, l->sched[i].mday);
		p += BIT8SZ;
		PBIT8(p, l->sched[i].wday);
		p += BIT8SZ;
		PBIT8(p, l->sched[i].hour);
		p += BIT8SZ;
		PBIT8(p, l->sched[i].min);
		p += BIT8SZ;
	}
	p = pstring(p, l->rmtname);
	p = pstring(p, l->rmtlv);
	PBIT16(p, l->copyclass);
	p += BIT16SZ;
	PBIT16(p, 0); // VSX-4583
	p += BIT16SZ;
	PBIT64(p, l->lastoffset);
	p += BIT64SZ;
	PBIT32(p, l->nse);
	p += BIT32SZ;
	PBIT32(p, l->copysnap);
	p += BIT32SZ;
	USED(p);
	/* set cksum last */
	s = onesum(buf, Xblk);
	PBIT16(buf+4, s);
	if (!validlve(l->lve, l)) {
		xsyslog("bad lve0 for %s: flag:%02x pid:%d off:%ud\n",
			l->name, l->lve[0].flag, l->lve[0].pid, l->lve[0].off);
		return -1;
	}
	if (l->lve[0].flag & LFthin) {
		xsyslog("lve0 is thin on %s\n", l->name);
		return -1;
	}
	n = pvio(l->pool->pv[l->lve[0].pid], buf, Xblk, l->lve[0].off * Xextent, OWRITE);
	if (n != Xblk) {
		xsyslog("savemeta: write failure %T:%lld (%d)\n", l->pool->pv[l->lve[0].pid]->targ, offset, n);
		return -1;
	}
	return 0;
}

/* 
	*xe = *e results in:

	warning: xlate.c:1634 returning packed structure

	And I don't have time to figure out if that's ok.
*/
void
e2e(LVE *xe, LVE *e)
{
	xe->flag = e->flag;
	xe->off = e->off;
	xe->pid = e->pid;
}

/*
 * "Mind the cows, dear." -- Hyacinth Bucket
 *
 *  Here we check for a copy on write.
 *  If we don't have exactly one reference count, we allocate a new
 *  extent and copy the old one to it.
 *  If we can't get a new one, we return failure to the caller.
 * 
 */
 
/* return true if everythings is okay, called with l wlocked */
static 
int
moo(LV *l, ulong xtnt, LVE *e, PVIO *pvv)
{
	ushort *r;
	PV *pv, *npv;
	PVE pve;
	PVPool *pvp;
	LVE xe;
	ushort oldr;

	if (!validlve(e, l)) {
		xsyslog("bad lve in moo for %s: lve:%uld flag:%02x pid:%d off:%ud\n",
			l->name, xtnt, e->flag, e->pid, e->off);
		return -1;
	}
	if (!validlve(l->lve, l)) {
		xsyslog("bad lve0 in moo for %s: flag:%02x pid:%d off:%ud\n",
			l->name, l->lve->flag, l->lve->pid, l->lve->off);
		return -1;
	}
	pvp = l->pool;
	wlock(pvp);
/*1*/	if (waserror()) {
		if (xaddu(&pvp->remapwarn, 1) < 10)
			xsyslog("%s\n", u->errstr);
		wunlock(pvp);
		return -1;
	}
	e2e(&xe, e);
	oldr = 0;
	r = nil;
	if (e->flag & LFthin)
		pv = l->pool->pv[l->lve->pid];
	else {
		pv = l->pool->pv[xe.pid];
		if (!pv)
			error("nil pv in COW for lv:%s extent: %lud", l->name, xtnt);
		r = pv->ref + xe.off;
		oldr = *r;
	}
	if ((e->flag & LFthin) || REFCNT(*r) > 1) {	/* remap */
		runlock(pvv->x); // xlock flip is protected by wlock(l)
		wlock(pvv->x);
		pvv->flags |= PVIOxwlock;
		npv = pv;
		pve = newpve1(pvp, &npv, e->flag & LFthin);
		if (pve.pid == -1)
			error("Allocation failure for LV %s, ran out of space in pool %s", l->name, pvp->name);
/*2*/		if (waserror()) {
			wlock(npv);
			/* The flush probably failed, just clear and mark as 
			   needing a flush */
			npv->ref[pve.off] = REFnf; 
			npv->frstfree = 0;
			npv->flags &= ~PVFfull;
			wunlock(npv);
			xincu(&pvp->efree);
			nexterror();
		}
		npv->ref[pve.off] |= REFused;
		switch (npv->state) {
		case PVSbroke:
		case PVSoosync:
			npv->ref[pve.off] |= REFdirty;
		}
		/*
		 * pvxtntcopy might sleep, but if we drop the lock on l and
		 * reaquire it, we violate lock ordering and get deadlock.  So
		 * until we know that it's a performance problem, we'll leave
		 * it as is.
		 */
		if (!(e->flag & LFthin)) {
//			qunlock(l);		/* because pvxtntcopy might sleep */
			if (pvxtntcopy(npv, pve.off, pv, xe.off) < 0) {
//				qlock(l);
				error("Remap extent copy failure from %T.%d to %T.%d for LV %s in pool %s",
					pv->targ, xe.off, npv->targ, pve.off, l->name, pvp->name);
			}
			xincu(&cows);
//			qlock(l);
		}
		if (flushpve(npv, pve.off) < 0)
			error("Remap extent meta flush failure on %T.%d", pv->targ, pve.off);
		if ((e->flag & LFthin) || e->pid != pve.pid || e->off != pve.off) {
			if (e->flag & LFthin) {
				l->thin--;
				e->flag &= ~LFthin;
			} else
				l->exts[e->pid]--;
			e->pid = pve.pid;
			e->off = pve.off;
			e->flag |= LFnf;
			l->exts[e->pid]++;
			if (!validlve(e, l)) {
				xsyslog("constructed bad lve in moo for %s: lve:%uld flag:%02x pid:%d off:%ud\n",
					l->name, xtnt, e->flag, e->pid, e->off);
				error("bad extent allocation");
			}
		}
		/*
		 * we have now committed to this extent.  If a future failure occurs, it means
		 * we lose this extent (it has a ref cnt, but no one points to it) and have
		 * to reclaim it by some other means.
		 */
/*2*/		poperror();

		if (r) {
			updnse(l, xtnt, &xe);
			*r -= 1;
			*r |= REFnf;
			if (flushpve(pv, xe.off) < 0) {
				error("Remap PV metadata update failure on %T.%d for LV %s",
					pv->targ, xe.off, l->name);
				/*
				 * why not *r++?  Because we've already remapped
				 * for the LV.  Some future flushpve might actually be able to
				 * succeed and the worst that happens is that on some future boot
				 * we reload the larger count and the extent has
				 * too many refs and is potentially lost in the pool.  Same tool
				 * to correct ref counts above could fix this.
				 */
			}
		}
		pv = npv;
		wunlock(pvv->x); // xlock flip is protected by wlock(l)
		rlock(pvv->x);
		pvv->flags &= ~PVIOxwlock;
	}
	if ((e->flag & LFdirty) == 0) {
		e->flag |= LFdirty;
		l->dirty++;
		e->flag |= LFnf;
	}
	if ((pv->ref[e->off] & REFused) == 0) {
		pv->ref[e->off] |= REFused;
		switch (pv->state) {
		case PVSbroke:
		case PVSoosync:
			pv->ref[e->off] |= REFdirty;
		}
		if (flushpve(pv, e->off) < 0) {
			pv->ref[e->off] &= ~(REFdirty|REFused);
			error("Remap used PVE flush failure on %T:%d", pv->targ, e->off);
		}
	}
	if (e->flag & LFnf) {
		e->flag &= ~LFnf;
		if (setlve(l, xtnt, e) < 0) {
			pv->ref[e->off] = oldr;
			if (flushpve(pv, e->off) < 0)
				xsyslog("Warning: could not clean up PVE allocation");
			e2e(e, &xe);
			error("Remap LV metadata update failure for LV %s", l->name);
		}
	}
/*1*/	poperror();
	wunlock(pvp);
	xchgu(&pvp->remapwarn, 0);
	return 0;
}

static
LV *
convM2LV(uchar *p, int n, PVPool *pvp)
{
	uchar *ep;
	char *pool;
	int i;
	LV *l;

	l = mallocz(sizeof *l, 1);
	if (l == nil) {
		xsyslog("convM2LV: LV allocation failure\n");
		return nil;
	}
	ep = p + n;
	p += 8;		/* skip magic and checksum */
	l->mode = GBIT32(p);
	p += BIT32SZ;
	l->lun = GBIT32(p);
	p += BIT32SZ;
	l->length = GBIT64(p);
	p += BIT64SZ;
	l->ctime = GBIT32(p);
	p += BIT32SZ;
	p += BIT32SZ; // skip starg for obsolete AoE shadow
	l->snaps = GBIT32(p);
	p += BIT32SZ;
	l->vlan = GBIT32(p);
	p += BIT32SZ;
	l->nqc = GBIT32(p);
	p += BIT32SZ;
	l->nlve = GBIT32(p);
	p += BIT32SZ;
	l->frstdat = GBIT32(p);
	p += BIT32SZ;
	p = gstring(p, ep, &l->name);
	p = gstring(p, ep, &pool);
	p = gstring(p, ep, &l->label);
	if (pool == nil || l->name == nil || l->label == nil) {
		xsyslog("convM2LV: string allocation failure\n");
		freelv(l);
		free(pool);
		return nil;
	}
	memmove(l->qc, p, 1024);
	p += 1024;
	l->nmmac = GBIT32(p);
	p += BIT32SZ;
	if (l->nmmac > 255)
		l->nmmac = 0;
	for (i = 0; i < l->nmmac; i++) {
		memmove(l->mmac[i], p, 6);
		p += 6;
	}
	l->prevsnap = GBIT32(p);
	p += BIT32SZ;
	memmove(l->serial, p, 20);
	/* Revert to lv name if no serial is found */
	if (*l->serial == 0) 
		strncpy(l->serial, l->name, 20);
	p += 20;
	l->snaplimit = GBIT64(p);
	p += BIT64SZ;
	for (i = 0; i < Nsched; i++) {
		l->sched[i].retain = GBIT16(p);
		p += BIT16SZ;
		l->sched[i].class = GBIT8(p);
		p += BIT8SZ;
		l->sched[i].mon = GBIT8(p);
		p += BIT8SZ;
		l->sched[i].mday = GBIT8(p);
		p += BIT8SZ;
		l->sched[i].wday = GBIT8(p);
		p += BIT8SZ;
		l->sched[i].hour = GBIT8(p);
		p += BIT8SZ;
		l->sched[i].min = GBIT8(p);
		p += BIT8SZ;
	}
	p = gstring(p, ep, &l->rmtname);
	p = gstring(p, ep, &l->rmtlv);
	l->copyclass = GBIT16(p);
	p += BIT16SZ;
	l->copysnap = GBIT16(p);
	p += BIT16SZ;
	l->lastoffset = GBIT64(p);
	p += BIT64SZ;
	l->nse = GBIT32(p);
	p += BIT32SZ;
	l->copysnap += GBIT32(p); // VSX-4583
	p += BIT32SZ;
	USED(p);
	if (l->mode & LVWIP) {
		xsyslog("Contact support@coraid.com: Refusing to load unfinished LV %s\n", l->name);
		free(pool);
		freelv(l);
		return nil;
	}
	if (strcmp(pvp->name, pool) != 0) {
		xsyslog("Error: LV %s: pool %s does not match expected pool %s\n",
			l->name, pool, pvp->name);
		free(pool);
		freelv(l);
		return nil;
	}
	l->pool = pvp;
	free(pool);
	/* These strings should be nil if unset */
	if (strlen(l->rmtlv) == 0) {
		free(l->rmtlv);
		l->rmtlv = nil;
	}
	if (strlen(l->rmtname) == 0) {
		free(l->rmtname);
		l->rmtname = nil;
	}
	return l;
}

void
hexdump(void *vp, uint n)
{
	uchar *u = vp;
	uint i, x, y;
	char buf[64], *p, *e;

	for (i=0; i<n;){
		p = buf;
		e = p + sizeof buf;
		p = seprint(p, e, "%02ux: ", i);
		for (x=0; i<n && x<4; x++) {
			for (y=0; i<n && y<4; y++, i++)
				p = seprint(p, e, "%02ux", u[i]);
			p = seprint(p, e, " ");
		}
		print("%s\n", buf);
	}
}

static void
abeyance(LV *l1)
{
	PVPool *pvp;
	LV *l2;

	switch (xlrfork(RFPROC|RFMEM|RFNOWAIT, "abeyance_%s", l1->name)) {
	case -1:
		xsyslog("Failed to create abeyance process for LV %s\n", l1->name);
		return;
	case 0:
		while (1) {
			sleep(2*60*1000);
			wlock(&lk);
			pvp = l1->pool;
			wlock(l1);
			if (allpvs(pvp, l1)) {
				l2 = lookuplv(l1->name);
				if (l2) {
					xsyslog("Deleting residual LV %s from rollback\n", l1->name);
					if (zmeta(l1) < 0)
						xsyslog("warning: Cannot zero out metadata from rolled back LV: %s\n", l1->name);
					rlock(pvp);
					decrefs(l1);
					wunlock(l1);
					freelv(l1);
					if (flushrefs(pvp) < 0)
						xsyslog("Warning: error writing pool %s PV Table\n", pvp->name);
					runlock(pvp);
				}
				else {
					xsyslog("Restoring LV %s from incomplete rollback\n", l1->name);
					l1->mode &= ~LVDELPEND;
					l1->flags &= ~LVFpartial;
					if (savemeta(l1, 0) < 0)
						xsyslog("Warning: error clearning pending delete in failed rollback\n");
					l1->next = vols;
					vols = l1;
					newlv(l1);
					if (l1->mode & LVLUN) {
						newlun(l1);
						luns[l1->lun] = l1;
						if (l1->mode & LVONL)
							tannounce(l1);
					}
					wunlock(l1);
				}
				wunlock(&lk);
				xlexits(0);
			}
			wunlock(l1);
			wunlock(&lk);
		}
	}
}

static 
LV *
loadmeta(PV *pv, uint offset)	/* load a lv, called with lk locked */
{
	LV *l;
	uchar buf[Xblk];
	ushort s;
	int n;

	if (xlread(pv, buf, Xblk, offset * Xextent) != Xblk) {
		xsyslog("loadmeta: failure reading LV metadata on %T:%ud\n", pv->targ, offset);
		return nil;
	}

	if (memcmp(buf, VSLVMAGIC, 4) != 0) {
		xincu(&pv->ldlverrs);
		if (lvmddebug)
			xsyslog("loadmeta: LV metadata extent %T.%ud missing header magic\n", pv->targ, offset);
		return nil;
	}
	s = GBIT16(buf+4);
	buf[4] = buf[5] = 0;
	if (onesum(buf, Xblk) != s) {
		xincu(&pv->ldlverrs);
		if (lvmddebug)
			xsyslog("loadmeta: Invalid LV metadata checksum on %T.%ud\n", pv->targ, offset);
		return nil;
	}
//hexdump(buf, 128);
	l = convM2LV(buf, Xblk, pv->pool);
	if (l == nil)
		return nil;
	/*
	 * There's no lock on l here because it's not in the vol list so
	 * no one else can see it.  Locking it here would violate the
	 * lock ordering, but for the same reason it's not necessary,
	 * it couldn't cause deadlock in this instance, but it's less
	 * confusing to just not lock it.
	 */
	n = (l->frstdat + Xlveperblk - 1) / Xlveperblk;
	l->lve = vtalloc(n, Xblk);
	if (l->lve == nil) {
		free(l);
		return nil;
	}
	if (loadvt(l, pv, offset * Xextent + Xblk, 1) < 0) {
		xsyslog("loadmeta: LV table load failed: %T.%ud\n", pv->targ, offset);
		return nil;
	}
	l->loadtime = time(0);
	l->flags |= LVFpartial;		/* partial until it's validated */
	if (l->mode & LVDELPEND) {
		abeyance(l);
		return nil;
	}
	l->next = vols;
	return vols = l;
}

/*
 * subtlety here.  the lve has the pv id and the offset for
 * each extent.  we know the pv and offset for the label and
 * the first Xlveperext - Xlveperblk, but we don't know where
 * the rest are.  but after we read the first extent's worth
 * of LVEs we do.  they are always in that first set.
 */
 
static 
int
loadvt(LV *l, PV *pv0, vlong offset, int partial)		/* read the lve structures, called with l locked */
{
	PV *pv;
	uchar buf[Xblk];
	ulong cnt;
	int rem, i, n, next, b;
	uchar *p;
	
	p = (uchar *)l->lve;
	pv = pv0;
	if (partial) {
		b = (l->frstdat + Xlveperblk - 1) / Xlveperblk;
		cnt = l->frstdat * sizeof (LVE);
		next = (b + Xblkperext - 1) / Xblkperext;
	} else {
		cnt = l->nlve * sizeof (LVE);
		next = l->frstdat;
	}
	rem = Xextent - Xblk;
	for (i = 0; cnt > 0 && i < next; i++) {
		if (i > 0) {
			if (!validlve(l->lve + i, l)) {
				xsyslog("bad lve in loadvt for %s: lve:%d flag:%02x pid:%d off:%ud\n",
					l->name, i, l->lve[i].flag, l->lve[i].pid, l->lve[i].off);
				return -1;
			}
			if (l->lve[i].flag & LFthin) {
				xsyslog("attempting to read thin metadata extent on %s: %d\n", l->name, i);
				return -1;
			}
			rem = Xextent;
			pv = l->pool->pv[l->lve[i].pid];
			if (pv == nil) { // possible on boot
				return -2;
			}
			offset = (vlong) l->lve[i].off * Xextent;
		}
		while (rem > 0 && cnt > 0) {
			n = pvio(pv, buf, Xblk, offset, OREAD);
			if (n != Xblk) {
				xsyslog("Failure reading LVT for %s on %T:%lld\n", l->name, pv->targ, offset);
				return -1;
			}
			n = Xlveperblk * sizeof (LVE);
			if (n > cnt)
				n = cnt;
			memmove(p, buf, n);
			p += n;
			offset += Xblk;
			rem -= Xblk;
			cnt -= n;
		}
	}
	if (cnt > 0)
		xsyslog("loadvt: ran out of extents to read\n");
	if (!partial) {
		for (i = 0; i < l->nlve; ++i) {
			if (!validlve(l->lve + i, l)) {
				xsyslog("bad lve in loadvt for %s: lve:%d flag:%02x pid:%d off:%ud\n",
					l->name, i, l->lve[i].flag, l->lve[i].pid, l->lve[i].off);
				return -1;
			}
		}
	}
	return 0;
}

static int
snapup(LV *up, LV *p)		/* called with up and p locked */
{
	LVE e1, e2, *ep, *et;
	LVEBUF *b;
	int i, any;
	int ps;
	uchar buf[Xblk];

	et = (LVE *)buf + Xlveperblk;
	for (i = 0; i < p->nlve; ) {
		any = 0;
		for (ep = (LVE *)buf; i < p->nlve && ep < et; ++i, ++ep) {
			if (!fetchlve(p, i, &e1) || !fetchlve(up, i, &e2)) {
				xsyslog("Failed to read metadata snap update\n");
				return -1;
			}
			if ((e1.flag & LFdirty) && !(e2.flag & LFdirty)) {
				e2.flag |= LFdirty;
				up->dirty++;
				any++;
			}
			e2e(ep, &e2);
		}
		if (any) {
			qlock(&lclock);
			b = getlveblk(up, i-1);
			if (!b) {
				xsyslog("Failure to read metadata in snap\n");
				qunlock(&lclock);
				return -1;
			}
			memmove(b->buf, buf, (uchar *)ep - buf);
			if (putlveblk(up, i-1, b->buf) < 0) {
				xsyslog("Failure to write metadata in snap\n");
				qunlock(&lclock);
				return -1;
			}
			mvhead(b);
			qunlock(&lclock);
		}
	}
	ps = up->prevsnap;
	up->prevsnap = p->prevsnap;
	if (savemeta(up, 0) >= 0)
		return 0;
	/*
	 * If this fails we end up with metadata on disk for up that's too dirty.  This
	 * is ok, we just copy too much data on shadow.
	 */
	up->prevsnap = ps;
	xsyslog("unable to save LV metadata for nextsnap element %s\n", up->name);
	return -1;
}

LV *
snaptolv(char *snapname)	/* called with lk locked */
{
	LV *pl;
	char *pname, *dot;

	pl = nil;
	pname = strdup(snapname);
	if (!pname) {
		uerr("cannot find LV for snapshot %s", snapname);
		goto x;
	}
	dot = strrchr(pname, '.');
	if (!dot) {
		uerr("can't find LV for snapshot %s", snapname);
		goto x;
	}
	*dot = '\0';
	pl = lookuplv(pname);
	if (!pl)
		uerr("cannot find LV %s", pname);
x:
	free(pname);
	return pl;
}

/* This does the real removal. The wlock should be held and the LV is locked */
/* Note: You call this function with l locked, but on success the LV is gone */
int
rmlv(LV *l)
{
	LV **pp, *p, *ns, **xp, *pl;
	PVPool *pvp;
	int snap, ps;
	ulong nse;

	snap = l->mode & LVSNAP;
	pvp = l->pool;
	if (waserror()) {
		return -1;
	}
	if (l->flags & LVFsuspended) 
		error("LV %s suspended\n", l->name);
	pl = nil;
	xp = nil;
	ns = nil;
	nse = 0;
	ps = 0;
	pp = &vols;
	for (; p=*pp; pp=&p->next) {
		if (p == l)
			xp = pp;
		if (snap)
		if (snapstrcmp(p->name, l->name) == 0)
		if (p->prevsnap == l->snaps)
			ns = p;
	}
	if (snap) {
		pl = snaptolv(l->name);
		nse = l->nse;
		if (pl == nil)
			error("LV: %s error acquire parent LV\n", l->name);
		if (ns == nil)
			error("cannot delete snap -- missing nextsnap element");
		wlock(ns);
		if (waserror()) {
			wunlock(ns);
			nexterror();
		}
		ps = ns->prevsnap;
		if (snapup(ns, l) < 0)
			error("cannot snap up snapshot dirty extents to nextsnap element");
		poperror();
		wunlock(ns);
	}
	if (zmeta(l) < 0) {
		/* Rollback the next snap's previous snap */
		if (snap)
			ns->prevsnap = ps;
		error("cannot zero out lv metadata on disk");
	}
	/* we're now committed to killing LV regardless of error */
	poperror();
	rlock(pvp);
	decrefs(l);
	*xp = l->next;
	invalblk(l);
	wunlock(l);
	deldir(l->dir);
	freelv(l);
	if (flushrefs(pvp) < 0)
		xsyslog("Warning: error writing pool %s PV Table\n", pvp->name);
	runlock(pvp);
	/* I don't want to grab this lock until I've released the pool lock */
	if (snap) {
		wlock(pl);
		pl->nse -= nse;
		wunlock(pl);
	}
	return 0;
}

/*
 * Called with lk and l locked. Return parent LV for snapshots.
 * Errors jump to the second waserror in xlrmlv.
 */

static LV *
xlrmlvchk(LV *l)
{
	LV *pl;
	LVL *sl;

	pl = nil;

	if (l->flags & LVFsuspended)
		error("LV %s suspended", l->name);
	if (l->mode & LVLUN)
		error("cannot remove LV %s assigned to LUN %T; remove LUN", l->name, lun2targ(l->lun));
	if (l->mode & LVSNAP) {
		pl = snaptolv(l->name);
		if (!pl)
			nexterror();
		if (isshadow(pl) && pl->snaps == l->snaps)
			error("cannot remove latest shadow snapshot %s %uld",
			      l->name, l->snaps);
	} else {
		if (snaplist(l, &sl, nil) < 0)
			error("cannot retrieve snaplist for %s: %r", l->name);
		if (sl) {
			freelvl(sl);
			error("cannot remove LV with snapshots");
		}
	}
	return pl;
}

void
xlrmlv(PVPool *pvp, char *name)	/* remove a logical volume */
{
	LV *l, *pl;

	wlock(&lk);
	if (waserror()) {
		wunlock(&lk);
		return;
	}
	l = lookuplv(name);
	if (l == nil || l->pool != pvp)
		error("cannot find LV %s in pool %s", name, pvp->name);
	wlock(l);
	if (waserror()) {	// xlrmlvchk errors jump to here
		wunlock(l);
		nexterror();
	}
	pl = xlrmlvchk(l);	// refactor xlrmlvchk() & if-else-if after 1.5
	if (pl) {		// l is a snapshot
		wlock(pl);
		if (isshadowsend(pl) && l->snaps >= pl->copysnap) {
			wunlock(pl);
			error("cannot remove %s before shadow sent %uld>=%uld",
			      name, l->snaps, pl->copysnap);
		}
		wunlock(pl);
	} else if (isshadow(l)) {
		error("cannot remove LV, unshadow %s first", name);
	}
	/* Note l is locked, but it will be unlocked and removed if rmlv is successful */
	if (rmlv(l) < 0) {
		nexterror();
	}
	poperror();
	wunlock(&lk);
	poperror();
	xsyslog("LV %s: removed from pool %s\n", name, pvp->name);
}

void
xlmvlv(PVPool *pvp, char *old, char *new)	/* renumber a snapshot */
{
	LV *l, *pl, *pln;
	LVL *sl, *s;
	int oldnum, newnum;

	wlock(&lk);
	if (waserror()) {
		wunlock(&lk);
		return;
	}
	l = lookuplv(old);
	if (l == nil || l->pool != pvp)
		error("cannot find LV %s in pool %s", old, pvp->name);

	oldnum = issnap(l);

	if (oldnum == 0)
		error("LV %s in pool %s is not a snapshot", old, pvp->name);

	if (lookuplv(new))
		error("LV %s already exists", new);

	pl = snaptolv(old);
	if (pl == nil)
		nexterror();

	pln = snaptolv(new);
	if (pln == nil)
		nexterror();

	if (pl != pln)
		error("LV %s and %s are not the same", pl->name, pln->name);

	newnum = issnapname(new);

	if (newnum >= oldnum)
		error("new snapshot %d is not earlier than old snapshot %d",
		      newnum, oldnum);

	if (snaplist(pl, &sl, nil) < 0)
		error("cannot retrieve snaplist for %s: %r", old);

	if (waserror()) {
		freelvl(sl);
		nexterror();
	}
	if (newnum <= l->prevsnap)
		for (s = sl; s; s = s->next)
			if (s->l->snaps >= newnum)
				error("%s blocks renaming %s to %s",
				      s->l->name, old, new);
	if (pl->prevsnap != oldnum) {
		for (s = sl; s; s = s->next)
			if (s->l->prevsnap == oldnum)
				break;
		if (s)
			pl = s->l;
		else
			error("cannot find next snap in snaplist for %s", old);
	}
	wlock(pl);
	wlock(l);
	if (waserror()) {
		wunlock(l);
		wunlock(pl);
		nexterror();
	}
	free(l->name);
	l->name = strdup(new);
	l->snaps = newnum;
	if (savemeta(l, 0) < 0) {
		free(l->name);
		l->name = strdup(old);
		l->snaps = oldnum;
		error("failure updating %s metadata", l->name);
	}
	pl->prevsnap = newnum;
	if (savemeta(pl, 0) < 0) {
		free(l->name);
		l->name = strdup(old);
		l->snaps = oldnum;
		pl->prevsnap = oldnum;
		// don't attempt to savemeta(l, 0) to restore old values
		// because it will likely fail too
		error("failure updating %s metadata", pl->name);
	}
	deldir(l->dir);
	newlv(l);
	poperror();
	wunlock(l);
	wunlock(pl);
	poperror();
	freelvl(sl);
	poperror();
	wunlock(&lk);
	xsyslog("LV %s: renamed to %s\n", old, new);
}

void
freelve(PVPool *pvp, LV *lv, ulong xtnt, LVE *e) /* called with lv locked */
{
	PV *pv;
	ushort *r;
	uint a;

	if (!validlve(e, lv)) {
		xincu(&pvp->flverrs);
		if (lvmddebug)
			xsyslog("attempting to free invalid lve for %s: "
				"flag:%02x pid:%d off:%ud\n",
				lv->name, e->flag, e->pid, e->off);
		return;
	}
	if (e->flag & LFthin)
		return;
	pv = pvp->pv[e->pid];
	if (!pv)
		return;
	wlock(pv);
	r = &pv->ref[e->off];
	a = REFCNT(*r);
	if (a > 0) {
		if (a == 1) {
			if (lv->mode & LVSNAP)
				lv->nse--;
		}
		a--;
		if (a == 0) {
			*r = REFnf;	/* mark as needing flushing */
			if (pv->flags & PVFpartial && e->off == pv->npve-1) {
				pv->flags &= ~PVFpartial;
				pv->npve--;
				xdecu(&pvp->etotal);
				xdecu(&totalext);
				pv->length = (vlong)pv->npve * Xextent;
				updpv(pv);
			} else {
				pv->frstfree = 0;
				pv->flags &= ~PVFfull;
				xincu(&pvp->efree);
			}
			wunlock(pv);
			return;
		} else
			*r = (*r & 0xf000) | a | REFnf;
	} else {
		xincu(&pvp->flverrs);
		if (lvmddebug)
			xsyslog("freelve: volume %s had a free ref @ %uld "
				"(%04ux), %T:%d\n",
				lv->name, xtnt, *r, pv->targ, e->off);
	}
	wunlock(pv);
	return;
}

/*
 * Go thru all the lves and dec references.  Mark them updated
 * so we can flush them when we're done. 
 */

static 
void
decrefs(LV *l)	/* decrement reference counts and update, called with l and l->pool locked */
{
	PVPool *pvp;
	LVE e;
	ulong i, flverrs;

	pvp = l->pool;
	flverrs = pvp->flverrs;
	for (i = 0; i < l->nlve; ++i) {
		if (fetchlve(l, i, &e) == nil)
			break;
		freelve(pvp, l, i, &e);
	}
	if (flverrs != pvp->flverrs)
		xsyslog("decrefs: LV %s had %uld unexpected free refs\n",
			l->name, pvp->flverrs);
}

static 
void
incpvref(PVPool *pvp, LVE *p)	/* increment reference count, called with pvp locked */
{
	ushort *r;
	int a;

	r = &pvp->pv[p->pid]->ref[p->off];
	a = *r & 0xfff;
	if (a == 0xfff) {
		xsyslog("Tried to overflow reference count on PV %T\n", pvp->pv[p->pid]->targ);
		return;
	}
	a++;
	*r = (*r & 0xf000) | a | REFnf;
}

static
int
uniqctime(LV *l, ulong ctime)
{
	LVL *p, *op;
	int ret;

	if (snaplist(l, &op, nil) < 0)
		return -1;
	ret = 0;
	for (p = op; p; p = p->next) 
		if (p->l->ctime == ctime) {
			ret = -1;
			break;
		}

	freelvl(op);
	return ret;
}

static 
int
maxedout(LV *l)	/* check to make sure we're not going to overflow a ref, called with l and l->pool locked */
{
	int i, ret;
	PVPool *pvp;
	LVE e;
	ushort *r;

	ret = 0;
	pvp = l->pool;
	for (i = 0; i < l->nlve; ++i) {
		if (fetchlve(l, i, &e) == nil)
			break;
		if (e.flag & LFthin)
			continue;
		r = &pvp->pv[e.pid]->ref[e.off];
		if (REFCNT(*r) == 0xfff) {
			ret = 1;
			break;
		}
	}
	return ret;
}

static 
PV *
emptiest(PVPool *pvp)	/* return the pv in this pool with the most free extents, called with pvp locked */
{
	uint n, max;
	int i, npv;
	PV *pv, *maxp;
	
	max = 0;
	maxp = nil;
	for (i = npv = 0; i < Npvsperpool && npv < pvp->npv; i++) {
		pv = pvp->pv[i];
		if (pv == nil)
			continue;
		npv++;
		if (pv->flags & (PVFlost|PVFfull))
			continue;
		n = pvspace(pv);
		if (n > max) {
			max = n;
			maxp = pv;
		}
	}
	return maxp;
}

/* alloc a span from this pv or fail. */
static PVE
allocpve(PVPool *pvp, PV *pv, int span)
{
	int i;
	ushort *r, *re;
	PVE e;

	e.pid = -1;
	wlock(pv);
	if (pv->flags & PVFlost) {
		wunlock(pv);
		return e;
	}
	r = pv->ref + pv->frstfree;
	re = pv->ref + pv->npve;
	for (; r<re; r++) {
		for (i=0; i<span; i++) {
			if (REFCNT(r[i]) != 0) {
				r += i;
				break;
			}
		}
		if (i == span) {
			for (i=0; i<span; i++) {
				r[i] = 1 | REFnf;
				xdecu(&pvp->efree);
			}
			e.off = r - pv->ref;
			e.pid = pv->id;
			pv->frstfree = e.off + span;
			break;
		}
	}
	wunlock(pv);
	return e;
}

/*
 * Search for touched refs and get them to the meta target, called
 * with pvp rlocked.
 */
int
flushrefs(PVPool *pvp)
{
	PV *p;
	int i, rv, npv;

	rv = 0;
	npv = 0;
	for (i = 0; i < Npvsperpool && npv < pvp->npv; i++)
		if (p = pvp->pv[i]) {
			npv++;
			if (flushpv(p) < 0)
				rv = -1;
		}
	return rv;
}

static
int
flushpve(PV *pv, int ref)
{
	int x, i, n;
	ushort *p;
	uvlong off;

	x = ref / Xrefperblk;
	p = pv->ref + x*Xrefperblk;
	x *= Xblk;
	x += Xblk;
	n = &pv->ref[pv->npve] - p;
	if (n > Xrefperblk)
		n = Xrefperblk;
	for (i=0; i<n; i++)
		p[i] &= ~REFnf;
	off = pv->offset + x;
	if (pvio(pv->meta, p, Xblk, off, OWRITE) != Xblk) {
		xsyslog("flushpve: error writing metadata for %T\n", pv->meta->targ);
		return -1;
	}
	return 0;
}

int
flushpv(PV *p)		/* scan for refs to flush */
{
	vlong offset;
	ushort *r, *base;
	int n, m, i, f;
	PV *pv;

	pv = p->meta;
	offset = p->offset;
	offset += Xblk;
	n = p->npve;
	r = p->ref;
	while (n > 0) {
		base = r;
		f = 0;
		m = n;
		if (m > Xrefperblk)
			m = Xrefperblk;
		for (i = 0; i < m; i++, r++)
			if (*r & REFnf) {
				f++;
				*r &= ~REFnf;
			}
		if (f) {
			if (pvio(pv, base, Xblk, offset, OWRITE) != Xblk) {
				xsyslog("Cannot sync PVT on %T:%lld\n", pv->targ, offset);
				/* reset the last entry so it gets flushed next time */
				r[-1] |= REFnf;
				return -1;
			}
		}
		offset += Xblk;
		n -= m;
	}
	return 0;
}

LV *
lookuplv(char *name)	/* called with lk locked */
{
	LV *l;

	for (l = vols; l; l = l->next)
		if (strcmp(l->name, name) == 0)
			break;
	return l;
}

static 
int
poolspace(PVPool *pvp)	/* how many extents free in pool, called with pvp locked */
{
	int i;
	uint cnt;
	PV *p;
	
	cnt = 0;
	for (i = 0; i < Npvsperpool; i++)
		if (p = pvp->pv[i])
			cnt += pvspace(p);
	return cnt;
}

static 
uint
pvspace(PV *pv)		/* what's the free space on this pv? */
{
	int n;
	uint cnt;
	ushort *q;

	rlock(pv);
	if (pv->flags & PVFlost) { /* Sorry, does not count */
		runlock(pv);
		return 0;
	}
	q = pv->ref;
	n = pv->npve;
	cnt = 0;
	while (n-- > 0) {
		if ((REFCNT(*q)) == 0)
			cnt++;
		q++;
	}
	runlock(pv);
	return cnt;
}

void
printlv(LV *l, int all)	/* print an lv structure */
{
	LVE e;
	int i;

	rlock(l);
	print("lv: %p: ", l);
	print("next=%p dir=%p cfgino=%p name=%s mode=0x%ulx lun=%d length=%lld frstdat=%d\n\t"
		"date=%uld pool=%p rmtname=%s snaps=%uld nqc=%d nlve=%uld flags=%ld\n",
		l->next, l->dir, l->cfgino, l->name, l->mode, l->lun, l->length, 
		l->frstdat, l->ctime, l->pool, l->rmtname, l->snaps, l->nqc, l->nlve, l->flags);
	if (all == 0) {
		runlock(l);
		return;
	}
	print("qc:\n");
	dump(l->qc, l->nqc);
	print("FL   PID      OFFSET\n");
	for (i = 0; i < l->nlve; i++) {
		if (fetchlve(l, i, &e) == nil)
			break;
		printlve(&e);
	}
	runlock(l);
}

void
printlve(LVE *e)	/* print one of the lv table elements */
{
	print("%02ux %5d %11ud\n", e->flag, e->pid, e->off);
}

void
xlclone(LV *l, char *dst)
{
	if (invalidname(dst))
		return;
	wlock(&lk);
	wlock(l);
	snapclone(l, ORDWR, time(nil), dst, nil, 0, nil);
	wunlock(l);
	wunlock(&lk);
}

void
xlrollback(LV *l)
{
	PVPool *pvp;
	LV *lv, *snap, *nlv;
	char snapname[32];
	ulong snapnse;

	if (isshadow(l)) {
		uerr("Cannot rollback shadow LV");
		return;
	}
	wlock(&lk);
	wlock(l);
	if (waserror()) {
		wunlock(l);
		wunlock(&lk);
		return;
	}
	if (l->mode & LVLUN)
		error("Cannot roll back LV %s with LUN %T", l->name, lun2targ(l->lun));
	if (l->mode & LVSNAP)
		error("Cannot rollback a snapshot, please enter an LV");
	if (l->prevsnap == 0)
		error("No snapshots found for %s", l->name);
	snprint(snapname, sizeof (snapname), "%s.%lud", l->name, l->prevsnap);
	snap = lookuplv(snapname);
	if (snap == nil)
		error("Cannot find snapshot for rollback: %s\n", snapname);
	l->mode |= LVDELPEND;
	if (savemeta(l, 0) < 0) {
		l->mode &= ~LVDELPEND;
		error("Failure to initiate rollback on LV %s", l->name);
	}
	if (vols == l)
		vols = l->next;
	else {
		for (lv = vols; lv && lv->next && lv->next != l; lv = lv->next) ;
		if (!lv) {
			l->mode &= ~LVDELPEND;
			savemeta(l, 0);
			error("Internal error: unknown LV: %s", l->name);
		}
		lv->next = l->next;
	}

	deldir(l->dir);
	if (snapclone(snap, ORDWR, time(nil), l->name, nil, 0, &nlv) < 0) {
		l->mode &= ~LVDELPEND;
		savemeta(l, 0);
		error("Failed to create rolled back LV: %s\n", l->name);
	}
	wlock(nlv);
	if (waserror()) {
		wunlock(nlv);
		nexterror();
	}
	nlv->mode = l->mode & ~LVDELPEND;
	nlv->prevsnap = l->prevsnap;
	nlv->snaps = l->snaps;
	nlv->vlan = l->vlan;
	nlv->nmmac = l->nmmac;
	nlv->nrmac = l->nrmac;
	nlv->snaplimit = l->snaplimit;
	memcpy(nlv->sched, l->sched, sizeof l->sched);
	memmove(nlv->mmac, l->mmac, Nmacmask * 6);
	memmove(nlv->rmac, l->rmac, Nmacmask * 6);
	/* The number of snap extents from the last snap are erased, except for meta extents */
	snapnse = snap->nse;
	nlv->nse = l->nse - snap->nse + snap->frstdat;
	snap->nse = snap->frstdat;
	if (waserror()) {
		snap->nse = snapnse;
		nexterror();
	}
	if (savemeta(snap, 0) < 0) {
		error("Failed to save metadata to snap: %s\n", snap->name);
	}
	if (savemeta(nlv, 0) < 0)
		error("Failed to save metadata in rollback\n");
	wunlock(nlv);
	poperror();
	if (zmeta(l) < 0)
		xsyslog("warning: Cannot zero out metadata from rolled back LV: %s\n", l->name);
	pvp = l->pool;
	rlock(pvp);
	decrefs(l);
	invalblk(l);
	poperror();
	poperror();
	wunlock(l);
	freelv(l);
	if (flushrefs(pvp) < 0)
		xsyslog("Warning: error writing pool %s PV Table\n", pvp->name);
	runlock(pvp);
	wunlock(&lk);

	xsyslog("LV %s: rolled back to %s\n", nlv->name, snapname);
}

/*
 * allocate [pl]vt table padding up to Xblk chunk for
 * simplifying i/o.
 */
void *
vtalloc(ulong nvt, ulong size)
{
	ulong n;
	void *p;

	if (nvt == 0 || size == 0)
		return nil;
	n = nvt * size;
	n += Xblk - (nvt % (Xblk/size))*size;
	if (n < nvt || n < size) {
		xsyslog("vtalloc: allocation overflow: nvt=%uld size=%uld n=%uld\n", nvt, size, n);
		return nil;
	}
	p = mallocz(n, 1);
	return p;
}

static int
sched2mode(LV *lv, Snapsched *s)	/* called with lv locked */
{
	if (sched2retain(lv, s) > 0) 
		return LVCANPRUNE;
	return 0;
}

/* 
  * Return -1 on general error
  * Return -2 if we attempt to create a snap with an existing ctime. Happens with snap shadow
  * Called with wlock on lk and old locked
  * The address of the newly created snap/clone is optionally returned through rlv
  * rlv is safe for the caller to dereference because lk is held by the caller
  */
int
snapclone(LV *old, int mode, ulong ctime, char *dst, Snapsched *sch, int snapnum, LV **rlv)	/* create a snapshot or a clone */
{
	LV *new, *xl, *lastsnap;
	char *newname, buf[512], *err, prevsnap[64];
	int uf, snap, oldsnaps;
	PVPool *pvp;

	pvp = old->pool;
	if (rlv)
		*rlv = nil;
	
	/* special fail case, don't use normal err handling */
	if (dst == nil && uniqctime(old, ctime) == -1) {
		uerr("snap already exists");
		return -2;
	}
	/* limitprune may call rmlv, which locks pvp */
	if (dst == nil && limitprune(old) < 0) {
		uerr("snap could overflow snaplimit");
		return -1;
	}
	/* retainprune may call rmlv, which locks pvp */
	if (dst == nil && retainprune(old, sch) < 0) {
		uerr("snap would exceed retain count");
		return -1;
	}
	if (dst == nil && old->prevsnap) {
		snprint(prevsnap, sizeof prevsnap, "%s.%lud", old->name, old->prevsnap);
		lastsnap = lookuplv(prevsnap);
		if (lastsnap == nil) {
			uerr("cannot update LV %s", prevsnap);
			return -1;
		}
		rlock(lastsnap);
		if (savemeta(lastsnap, 0) < 0) {
			runlock(lastsnap);
			uerr("LV %s: metadata update failed", prevsnap);
			return -1;
		}
		runlock(lastsnap);
	}
	rlock(pvp);
/*1*/	if (waserror()) {
		runlock(pvp);
		return -1;
	}
	if (dst == nil && old->snaplimit == SLunset)
		error("snaplimit must be set");
	if (maxedout(old))
		error("too many snaps/clones for LV %s", old->name);
	if (dst == nil && old->mode & LVSNAP)
		error("cannot snap a snap");
	if (dst) {
		xl = lookuplv(dst);
		if (xl)
			error("can't create clone, LV %s already exists in pool %s", xl->name, xl->pool->name);
		newname = strdup(dst);
		snap = 0;
	} else {
		snprint(buf, sizeof buf, "%s.%uld", old->name,
			snapnum ? snapnum : old->snaps+1);
		if (lookuplv(buf))
			error("can't create snap; name conflict");
		newname = strdup(buf);
		snap = 1;
	}
	new = mallocz(sizeof *new, 1);
	if (new == nil)
		error("memory allocation failure for LV");
	new->name = newname;
	new->label = strdup("");
	if (new->name == nil || new->label == nil)
		error("memory allocation failure for LV elements");
	wlock(new);
/*2*/	if (waserror()) {
		wunlock(new);
		freelv(new);
		nexterror();
	}
	if (mode == ORDWR)
		new->mode = LVREAD|LVWRITE;
	else {
		new->mode = LVREAD;
		new->mode |= LVSNAP;
	}
	new->mode |= sched2mode(old, sch) | LVTHIN;
	new->length = old->length;
	new->lun = -1;
	new->ctime = ctime;
	new->pool = pvp;
	new->snaplimit = SLign;
	oldsnaps = old->snaps;
	if (dst) {
		new->snaps = 0;
	} else {			/* old only changes here */
		if (snapnum)
			old->snaps = snapnum;
		else
			old->snaps++;
		if (sch)
			memcpy(new->sched, sch, sizeof(Snapsched));
		new->snaps = old->snaps;
		new->prevsnap = old->prevsnap;
		old->prevsnap = old->snaps;
	}
/*3*/	if (waserror()) {
		if (snap)
			old->prevsnap = new->prevsnap;
		old->snaps = oldsnaps;
		nexterror();
	}
	new->frstdat = old->frstdat;
	new->nqc = 0;
	new->nmmac = 0;
	new->nrmac = 0;
	new->nlve = old->nlve;
	setserial(new);
	if (err = alloclvt(new, old, pvp->pv[old->lve[0].pid], snap ? AClrDirty | AFreeOrphan : AMkDirty | AFreeOrphan))
		error("failure allocating LV Table for %s: %s", dst ? "clone" : "snap", err);
	uf = 0;
/*4*/	if (waserror()) {
		decrefs(new);
		if (snap)
			old->nse -= new->nse;
		if (uf)
			flushrefs(pvp);
		invalblk(new);
		nexterror();
	}
	if (savemeta(new, LVWIP) < 0)
		error("failure writing LV metadata");
	if (flushrefs(pvp) < 0)
		error("failure writing pool PV Table state");
	if (savemeta(new, 0) < 0)
		error("failure updating LV metadata");
	uf = 1;
	if (savemeta(old, 0) < 0)
		error("failure updating original LV metadata");
	USED(uf);
	new->next = vols;
	vols = new;
	if (dst == nil && isshadow(old))
		snapcopyck(1);
/*4*/	poperror();
/*3*/	poperror();
/*2*/	poperror();
	wunlock(new);
/*1*/	poperror();
	newlv(new);
	runlock(pvp);
	if (dst)
		xsyslog("Clone %s: created from %s in pool %s\n", new->name, old->name, pvp->name);
	else
		xsyslog("Snap %s: %s snap created from %s created in pool %s\n", new->name, fmtsnapclass(new->sched), old->name, pvp->name);
	if (rlv) 
		*rlv = new;
	old->flags |= LVFallsnaps;
	return 0;
}

// XXX add label to pv's for advisory "com.coraid.vsx <some id>"


static 
LV *
isused(int lun)	/* return true if this lun already appears, called with lk locked */
{
	LV *l;
	
	for (l = vols; l; l = l->next)
		if (l->mode & LVLUN && l->lun == lun)
			return l;
	return l;
}

/* There is no snapname.0, so this will work */
int
issnapname(char *name)
{
	int snap;
	char *sc, *e;

	sc = strrchr(name, '.');
	if (sc == nil) {
		return 0;
	}
	if (strlen(++sc) == 0) {
		return 0;
	}
	snap = strtol(sc, &e, 10);
	if (*e) {
		return 0;
	}
	return snap;
}

int
issnap(LV *l)
{
	if ((l->mode & LVSNAP) == 0)
		return 0;

	return issnapname(l->name);
}


// XXX call xlvolchged(void *a (really LV *), vlong newlength); on size change
// 'A' would be used as a key to find the length to change

void
xlmklun(LV *l, char *arg)
{
	int lun, t, sh;
	LV *xl;

	wlock(&lk);
	wlock(l);
	if (waserror()) {
		wunlock(l);
		wunlock(&lk);
		return;
	}
	if (shelf == -1)
		error("base shelf address unset");
	t = parsetarget(arg);
	if (t < 0)
		nexterror();	//parsetarget sets uerr
	sh = SH(t);
	if (sh < shelf || sh >= shelf + 16)
		error("shelf %d outside VSX working range [%d - %d]", sh, shelf, shelf+15);
	lun = ((sh - shelf) << 8) | (t & 0xff);
	if (lun < 0 || lun > Nluns)
		error("illegal LUN overspecification");	/* this should not happen */
	if (l->lun != -1)
		error("volume already assigned to LUN %T", lun2targ(l->lun));
	xl = isused(lun);
	if (xl)
		error("LUN %T already in use on LV %s", t, xl->name);
	if (l->mode & LVSNAP)
		error("cannot make a LUN with a snap LV.  Clone snap LV and then mklun");
	if (isshadowrecv(l))
		error("cannot make a LUN with a shadowrecv LV");
	l->mode |= LVLUN;
	l->lun = lun;
	if (savemeta(l, 0) < 0) {
		l->mode &= ~LVLUN;
		l->lun = -1;
		error("failure writing LV metadata");
	}
	/* we're committed now */
	newlun(l);
	luns[l->lun] = l;
	poperror();
	wunlock(l);
	wunlock(&lk);
	xsyslog("LUN %T: assigned to LV %s\n", lun2targ(l->lun), l->name);
	// XXX must change dirtab when size of volume changes (grow/shrink)
}

void
xloffline(LV *l)
{
	wlock(l);
	l->mode &= ~LVONL;
	if (savemeta(l, 0) < 0)
		uerr("LUN set offline, but could not write LV metadata; LUN may start online next boot");
	xsyslog("%T is now offline\n", lun2targ(l->lun));
	wunlock(l);
}

void
xlonline(LV *l)
{
	wlock(l);
	if (l->flags & LVFsuspended) {
		uerr("LV %s suspended", l->name);
	} else {
		l->mode |= LVONL;
		if (savemeta(l, 0) < 0)
			uerr("LUN set online, but could not write LV metadata; LUN may start offline next boot");
		xsyslog("%T is now online\n", lun2targ(l->lun));
		tannounce(l);
	}
	wunlock(l);
}

void
xlresetserial(LV *l)
{
	wlock(l);
	setserial(l);
	savemeta(l, 0);
	wunlock(l);
}

void
xlclrres(LV *lv)
{
	wlock(lv);
	lv->nrmac = 0;
	memset(lv->rmac, 0, sizeof lv->rmac);
	if (lv->lun != -1)
		rmrr(lv->lun);
	savemeta(lv, 0);
	wunlock(lv);
	xsyslog("LV %s: reservations cleared\n", lv->name);
}

int
lun2targ(int lun)
{
	if (lun == -1)
		return lun;
	return lun += shelf<<8;
}

int
rmrr(int lun)
{
	char *cfn;
	int fd;

	cfn = smprint("/n/rr/%d/ctl", lun);
	fd = -1;
	if (cfn == nil) {
		xsyslog("rmrr: allocation error\n");
		goto e;
	}
	fd = open(cfn, OWRITE);
	if (fd < 0) {
		xsyslog("rmrr: error opening %s: %r\n", cfn);
		goto e;
	}
	if (write(fd, "reset", 5) < 0) {
		xsyslog("rmrr: error writing on %s: %r\n", cfn);
		goto e;
	}
	free(cfn);
	close(fd);
	return 0;

e:	free(cfn);
	if (fd != -1)
		close(fd);
	return -1;
}

void
xlrmlun(LV *l)
{
	int lun;

	wlock(&lk);
	wlock(l);
	if (waserror()) {
		wunlock(l);
		wunlock(&lk);
		return;
	}
	if ((l->mode & LVLUN) == 0)	/* should not happen */
		error("LV %s not assigned to LUN", l->name);
	if (l->mode & LVONL)
		error("LUN %T is online - offline to remove", lun2targ(l->lun));
	lun = l->lun;
	l->lun = -1;
	l->mode &= ~LVLUN;
	if (savemeta(l, 0) < 0) {
		l->lun = lun;
		l->mode |= LVLUN;
		error("failure writing LV metadata");
	}
	rmrr(lun);
	deldir(l->lundir);
	luns[lun] = nil;
	poperror();
	wunlock(l);
	wunlock(&lk);
	xsyslog("LUN %T: disassociated from LV %s\n", lun2targ(lun), l->name);
}

void
xlsetmode(LV *l, int mode)
{
	wlock(l);
	if (waserror()) {
		wunlock(l);
		return;
	}
	if (mode == OREAD)
		l->mode &= ~LVWRITE;
	else if (mode == ORDWR) {
		if (l->mode & LVSNAP)
			error("can't set snapshot to readwrite");
		l->mode |= LVWRITE;
	} else
		error("unknown mode %02ux", mode);
	if (savemeta(l, 0) < 0)
		error("mode set, but could not write LV metadata; mode may reset on next boot");
	poperror();
	xsyslog("LV %s: mode %s\n", l->name,
		mode == OREAD ? "readonly" : "readwrite");
	wunlock(l);
}

/*
 * This will temporarily offline all luns and update legacy
 * luns and PVs as needed 
 */
void
xlupdatelegacy(void)
{
	int h, i;
	LV *l;
	char online[Nluns]; /* Yeah, I know it is dumb */

	memset(online, 0, sizeof online);
	rlock(&lk);
	h = hasleg();
	if (h == -1) {
		runlock(&lk);
		return; /* hasleg sets uerr */
	}
	if (h == 0) {
		runlock(&lk);
		xsyslog("No legacy volumes detected\n");
		return;
	}
	for (l = vols, i = 0; l; l = l->next, i++) {
		if (l->mode & LVONL) {
			online[i] = 1;
			xloffline(l);
		}
	}
	if (updleglvs() == 1) /* updleglvs sets uerr */
		xsyslog("Legacy volumes successfully updated\n");
	for (l = vols, i = 0; l; l = l->next, i++) {
		if (online[i]) {
			xlonline(l);
		}
	}
	runlock(&lk);
}

/* Sets the mainmem flags, see pool(2) for details */
void
xlmemflag(char *f)
{
	int o;

	o = mainmem->flags;
	mainmem->flags = atoi(f);
	xsyslog("mainmem flag was 0x%x, now 0x%x\n", o, mainmem->flags);
}

void
xlmemstir(int msleep)
{
	static int brake, pid;
	int w;

	if (pid) {
		brake = 1;
		for (w = 0; brake && w < 100; w++) {
			sleep(100);
		}
		if (brake) {
			uerr("Can not stop %d\n", pid);
			return;
		}
	}
	if (msleep == 0) {
		pid = 0;
		return;
	}
	switch (w = rfork(RFPROC|RFMEM|RFNOWAIT)) {
	case -1:
		uerr("rfork failed: %r");
		break;
	case 0:
		heapstirrer(&brake, -1, msleep);
		exits(nil);
	default:
		pid = w;
		break;
	}
}

/*
 * Extents are locked to protect users during COW duplication.  Extents
 * are wlocked in moo() when they're empty and being filled, and rlocked
 * during any other access.  This keeps the users from finding an empty extent.
 */

void
xlatefini(PVIO *pvv)	/* finished with IO */
{
	int xtnt;

	if (pvv->flags & PVIOflush) {
		xtnt = pvv->offset/Xextent;

		if (pvv->pv->ref[xtnt] & REFnf)
			flushpve(pvv->pv, xtnt);
	}
	if (pvv->x)
		if (pvv->flags & PVIOxwlock)
			xwunlock(pvv->x);
		else
			xrunlock(pvv->x);
}

static int
xlatex(PVIO *pvv, LV *l, vlong offset, int count, int mode)
{
	ulong r, xtnt;
	LVE e;

	memset(pvv, 0, sizeof *pvv);
	if (offset + count > l->length)
		count = l->length - offset;
	if (count <= 0) {
		return 0;
	}
	xtnt = offset/Xextent + l->frstdat;
	pvv->x = xrlock(l, xtnt);
	if (!fetchlve(l, xtnt, &e)) {
		return 0;
	}

	if (mode == OWRITE && moo(l, xtnt, &e, pvv) < 0) {
		return 0;
	}
	r = offset % Xextent;
	if (r + count > Xextent)
		count = Xextent - r;
	pvv->count = count;
	if(e.flag & LFthin) {
		pvv->offset = 0;
		pvv->pv = &virtpv;
	}
	else {
		pvv->offset = (vlong)e.off * Xextent;
		pvv->offset += r;
		pvv->pv = l->pool->pv[e.pid];
		if (mode == OWRITE)
			pvv->flags |= PVIOflush;
	}
	return 1;
}

int
xlate(PVIO *pvv, LV *l, vlong offset, int count, int mode)
{
	int ret;

	rlock(&lk);

	if (mode == OWRITE)
		wlock(l);
	else
		rlock(l);

	ret = xlatex(pvv, l, offset, count, mode);

	if (ret == 0)
		xlatefini(pvv);

	if (mode == OWRITE)
		wunlock(l);
	else
		runlock(l);

	runlock(&lk);
	return ret;
}

/*
 * x[rw]lock and x[rw]unlock control access to extents for reading and
 * writing.  The busy table has a number of entires that identify
 * extents that have some operation going that prevents access by
 * others.  If the slots are full the proc will sleep until a slot is
 * freed.
 */
 
void
initxlock(void)
{
	xlockwait.l = &xqlk;
}

/*
 * Coordinate i/o to extents. Block writes into PV extents that are
 * being copied because of reslivering. Protect LVE mapping from
 * changes due to CoW remapping.
 */

int
xlkhsh(void *a)	/* xlock hash */
{
	ulong h;
	
	h = (ulong)a;
	return h % NXLOCKS;
}

XLock *
getxlk(void *a, int k)	/* find or create xlock entry */
{
	int h, j;
	XLock *x, *y;
	
	y = nil;
	h = j = xlkhsh(a);
	do {
		x = busy + h;
		if (x->a == a && x->k == k) {
			x->ref++;
			return x;
		}
		if (y == nil && x->ref == 0) 
			y = x;
		h = (h+1) % NXLOCKS;
	} while (h != j);
	if (y) {
		y->a = a;
		y->k = k;
		y->ref = 1;
		return y;
	}
	return nil;
}

void
xlkfree(XLock *x)
{
	qlock(&xqlk);
	assert(x->ref > 0);
	if (--x->ref == 0)
		rwakeup(&xlockwait);
	qunlock(&xqlk);
}

XLock *
xwlock(void *a, int k)
{
	XLock *x;
	
	qlock(&xqlk);
	while ((x = getxlk(a, k)) == 0) {
		xlockwaits++;
		rsleep(&xlockwait);
	}
	qunlock(&xqlk);
	wlock(x);
	return x;
}

XLock *
xrlock(void *a, int  k)
{
	XLock *x;
	
	qlock(&xqlk);
	while ((x = getxlk(a, k)) == 0) {
		xlockwaits++;
		rsleep(&xlockwait);
	}
	qunlock(&xqlk);
	rlock(x);
	return x;
}

void
xwunlock(XLock *x)
{
	wunlock(x);
	xlkfree(x);
}
	
void
xrunlock(XLock *x)
{
	runlock(x);
	xlkfree(x);
}

static
int
cntfreerefs(PV *pv)		/* called with pv locked */
{
	int cnt, i;
	
	cnt = 0;
	for (i = 0; i < pv->npve; i++)
		if (REFCNT(pv->ref[i]) == 0)
			cnt++;
	return cnt;
}

void
xlmask(LV *lv, int argc, char *argv[])
{
	int i, j, add, x;
	uchar ea[6];

	/* first validate all the arguments */
	for (i=0; i<argc; i++) {
		switch (*argv[i]) {
		case '+':
		case '-':
			if (parseether(ea, argv[i]+1) < 0) {
				uerr("invalid mac address on directive %s", argv[i]);
				return;
			}
			break;
		default:
			uerr("invalid mask directive %s", argv[i]);
			return;
		}
	}
	wlock(lv);
	/* process */
	x = 0;
	for (i=0; i<argc; i++) {
		add = *argv[i] == '+';
		parseether(ea, argv[i]+1);
		for (j=0; j<lv->nmmac; j++)
			if (memcmp(lv->mmac[j], ea, 6) == 0)
				break;
		if (j != lv->nmmac) {	/* found it */
			if (add) {
				uerr("mask %E exists on LV %s", ea, lv->name);
				goto e;
			}
			xsyslog("LV %s: remove %E from the mask list\n", lv->name, lv->mmac[j]);
			memmove(lv->mmac[j], lv->mmac[--lv->nmmac], 6);
			x++;
			continue;
		}
		if (!add) {
			uerr("mask %E does not exist on LV %s", ea, lv->name);
			goto e;
		}
		if (lv->nmmac >= Nmasks) {
			uerr("too many masks on lv %s at %s, max %d", lv->name, argv[i], Nmasks);
			goto e;
		}
		memcpy(lv->mmac[lv->nmmac++], ea, 6);
		xsyslog("LV %s: added %E to the mask list\n", lv->name, ea);
		x++;
	}
e:	if (x)
	if (savemeta(lv, 0) < 0)
		uerr("mask processed, but failure writing LV metadata; masks may not persist across reboot");
	wunlock(lv);
}

void
xlresize(LV *lv, char *size)
{
	LV olv;
	LVE e;
	LVL *sl;
	PVPool *pvp;
	vlong length;
	uint nxts, nm;
	ulong i, oldnlve;
	int serr;
	char *err;

	pvp = lv->pool;
	rlock(&lk);
	wlock(lv);
	rlock(pvp);
	if (waserror()) {
		runlock(pvp);
		wunlock(lv);
		runlock(&lk);
		return;
	}
	if (lv->mode & LVSNAP)
		error("cannot resize snapshots");
	length = atosize(size, &serr);
	if (length <= 0 || serr)
		error("Invalid LV length: %s", size);
	if (length < lv->length) {
		if (snaplist(lv, &sl, nil) < 0)
			error("cannot retrieve snaplist for %s: %r", lv->name);
		if (sl) {
			freelvl(sl);
			error("cannot shrink LV with snapshots -- remove snaps first");
		}
	}
	if (isshadowsend(lv)) {
		if (lv->rmtlvsize == 0)
			error("LV shadow target size unknown");
		if (lv->rmtlvsize < length)
			error("LV shadow target too small to accommodate new LV size");
	}
	oldnlve = lv->nlve;
	nxts = (length+Xextent-1) / Xextent;
	if (length > lv->length) {				/* expand */
		nm = metaextents(nxts, Xlveperblk);
		memmove(&olv, lv, sizeof (LV));
		lv->length = length;
		lv->frstdat = nm;
		lv->nlve = nxts + nm;
		lv->lve = nil;
		memset(lv->exts, 0, sizeof(lv->exts));
		if (err = alloclvt(lv, &olv, nil, lv->mode & LVTHIN ? AThin : 0 |AClrDirty|ANoInc)) {
			memmove(lv, &olv, sizeof (LV));
			error("LV resize failed: %s", err);
		}
		invalblk(lv);
		if (savemeta(lv, LVWIP) < 0)
			error("failure writing LV metadata - change will not persist across reboot");
		if (zmeta(&olv) < 0)
			xsyslog("Warning: failure to zero metadata on resize\n");
		/*
		 * The metadata extents will be unique so we don't
		 * have to worry about freelve calling updunique.
		 */
		for (i = 0; i < olv.frstdat; ++i) {
			if (fetchlve(&olv, i, &e))
				freelve(pvp, &olv, i, &e);
		}
		invalblk(&olv);
		if(olv.lve)
			free(olv.lve);
	}
	else {					/* reduce */
		for (i = lv->frstdat + nxts; i < lv->nlve; ++i) {
			if (fetchlve(lv, i, &e))
				freelve(pvp, lv, i, &e);
		}
		
		lv->nlve = lv->frstdat + nxts;
		lv->length = length;
		if (savemeta(lv, LVWIP) < 0)
			error("failure writing LV metadata - change will not persist across reboot");
	}
	if (flushrefs(pvp) < 0)
		error("failure writing pool PV Table state - change may not persist across reboot");
	if (savemeta(lv, 0) < 0)
		error("failure updating LV metadata");
	if ((lv->flags & LVLUN) && (lv->flags & LVONL))
		tannounce(lv);
	xsyslog("LV %s: resized from %uld to %uld extents in pool %s\n", lv->name, oldnlve, lv->nlve, lv->pool->name);
	poperror();
	runlock(pvp);
	wunlock(lv);
	runlock(&lk);
}
/*
 * This is similar to updunique. If the LV is sharing an extent with
 * his last snapshot bump the nse counter.
 */
static void
updnse(LV *l, ulong n, LVE *lve) /* called with lk, l locked */
{
	LVE e;
	LV *snap;
	char snapname[32];

	/* Nothing to worry about, just leave */
	if (l->prevsnap == 0 || l->mode & LVSNAP) {
		return;
	}
	/* Perhaps LV should have a ptr to the last snapshot, in addition to the number */
	snprint(snapname, sizeof (snapname), "%s.%lud", l->name, l->prevsnap);
	snap = lookuplv(snapname);
	if (snap == nil) {
		xsyslog("udpnse: cannot find previous snapshot: %s\n", snapname);
		return;
	}
	if (n - l->frstdat + snap->frstdat < snap->nlve) {
		if (fetchlve(snap, n - l->frstdat + snap->frstdat, &e) == nil) {
			xsyslog("LV %s: unable to fetch LVE %uld\n", snapname, n);
		} else if (lve->pid == e.pid && lve->off == e.off) {
			l->nse++;
			snap->nse++;
		}
	}
}

void 
xlmaintmode(int init)
{
	int fd, n;
	char buf[16];

	iotimeo = Ntiomax;	/* disabled by default */
	fd = open(Mmconf, OREAD);
	if (fd < 0) {
		if (init == 0)
			xsyslog("xlmaintmode: failed to open %s: %r\n", Mmconf);
	} else {
		n = read(fd, buf, sizeof buf);
		if (n < 0 && init == 0)
			xsyslog("xlmaintmode: failed to read %s: %r\n", Mmconf);
		else {
			if (n == sizeof buf)
				n--;
			buf[n] = '\0';
			if (strcmp(buf, "enable") == 0)
				iotimeo = Ntiomaxmaint;
		}
		close(fd);
	}
}

void 
xliotimeo(char *secs)
{
	ulong ioto;

	ioto = strtoul(secs, nil, 10);
	if (ioto >= 10)
		iotimeo = ioto * 1000;
}

static int
canpoolsplitpv(PV *pv)
{
	if (pv->flags & PVFlost) {
		uerr("%T is lost", pv->targ);
		return 0;
	}
	if (pv->state != PVSdouble) {
		uerr("%T is not mirrored", pv->targ);
		return 0;
	}
	if (pv->offset != 0) {
		uerr("%T is an old legacy PV", pv->targ);
		return 0;
	}
	if (targlenclean(pv->targ) == 0)	// sets uerr
		return 0;
	if (targlenclean(pv->mirror) == 0)	// sets uerr
		return 0;
	return 1;
}

static int
lunsoffline(PVPool *pvp)	/* called with lk locked */
{
	LV *l;
	int n;

	n = 0;
	for (l = vols; l; l = l->next) {
		if (l->pool != pvp)
			continue;
		if (l->mode & LVSNAP)
			continue;
		if (l->mode & LVONL) {
			uerr("%T is online", lun2targ(l->lun));
			return -1;
		}
		if (isshadowrecv(l)) {
			uerr("%s has shadowrecv", l->name);
			return -1;
		}
		if (l->dirty - l->frstdat > 0) {
			uerr("LV %s needs a snap", l->name);
			return -1;
		}
		++n;
	}
	return n;
}

static int
clearluns(PVPool *pvp, int *luns)	/* called with lk locked */
{
	LV *l;
	int n;

	n = 0;
	for (l = vols; l; l = l->next) {
		if (l->pool != pvp)
			continue;
		if (l->mode & LVSNAP)
			continue;
		wlock(l);
		luns[n] = l->lun;
		if (l->lun != -1) {
			l->mode &= ~LVLUN;
			l->lun = -1;
			if (savemeta(l, 0) < 0) {
				wunlock(l);
				uerr("%s savemeta failure", l->name);
				return -1;
			}
		}
		wunlock(l);
		++n;
	}
	return 0;
}

static int
restoreluns(PVPool *pvp, int *luns)	/* called with lk locked */
{
	LV *l;
	int n;

	n = 0;
	for (l = vols; l; l = l->next) {
		if (l->pool != pvp)
			continue;
		if (l->mode & LVSNAP)
			continue;
		if (luns[n] != -1) {
			wlock(l);
			l->mode |= LVLUN;
			l->lun = luns[n];
			if (savemeta(l, 0) < 0) {
				wunlock(l);
				uerr("%s savemeta failure", l->name);
				return -1;
			}
			wunlock(l);
		}
		++n;
	}
	return 0;
}

static int
poolsplitx(PVPool *pvp, void *a, int count, vlong offset)
{
	char buf[9000], *s, *e;
	int i, npv, nlvs, *luns, mirror, targ;
	PV* pv;

	if (pvp->npv == 0) {
		uerr("%s is empty", pvp->name);
		return 0;
	}
	if ((nlvs = lunsoffline(pvp)) < 0)	// sets uerr
		return 0;

	for (i = npv = 0; i < Npvsperpool && npv < pvp->npv; i++) {
		pv = pvp->pv[i];
		if (pv == nil)
			continue;
		++npv;
		rlock(pv);
		if (!canpoolsplitpv(pv)) {	// sets uerr
			runlock(pv);
			return 0;
		}
		runlock(pv);
	}
	luns = nil;
/*1*/	if (waserror()) {
		free(luns);
		return 0;
	}
	if (nlvs > 0) {
		luns = malloc(nlvs * sizeof *luns);
		if (!luns)
			error("malloc failure");

		if (clearluns(pvp, luns) < 0)	// sets uerr
			nexterror();
	}
	s = buf;
	e = s + sizeof buf;

	for (i = npv = 0; i < Npvsperpool && npv < pvp->npv; i++) {
		pv = pvp->pv[i];
		if (pv == nil) {
			s = seprint(s, e, " -");
			continue;
		}
		++npv;
		wlock(pv);
/*2*/		if (waserror()) {
			wunlock(pv);
			nexterror();
		}
		targ = pv->targ;
		mirror = pv->mirror;
		pv->state = PVSsingle;
		pv->mirror = -1;
		pv->targ = mirror;
		if (updpv(pv) < 0) {
			error("%T metadata write failure", pv->targ);
		}
		pv->targ = targ;
		if (updpv(pv) < 0) {
			error("%T metadata write failure", pv->targ);
		}
/*2*/		poperror();
		wunlock(pv);
		s = seprint(s, e, " %T", mirror);
	}
	if (nlvs > 0) {
		if (restoreluns(pvp, luns) < 0)	// sets uerr
			nexterror();
	}
/*1*/	poperror();
	free(luns);
	seprint(s, e, "\n");
	return readstr(offset, a, count, buf + 1);
}

int
poolsplit(void *arg, void *a, int count, vlong offset)
{
	PVPool *pvp;
	int ret;

	if (offset)
		return 0;	
	pvp = arg;
	rlock(&lk);
	rlock(pvp);
	ret = poolsplitx(pvp, a, count, offset);
	runlock(pvp);
	runlock(&lk);
	if (ret) {
		writeconfig();
		xsyslog("Pool %s: split\n", pvp->name);
	}
	return ret;
}

void
xlrestorepool(int argc, char *argv[])
{
	int i, t, u;
	char *name;
	PVPool *pvp;

	if (argc < 2) {
		uerr("no pool name and target(s)");
		return;
	}
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-')
			continue;
		t = parsetarget(argv[i]); // sets uerr
		if (t == -1)
			return;
		rlock(&lk);
		u = targused(t);
		runlock(&lk);
		if (u != -1) {
			uerr("target %T already in use", t);
			return;
		}
		if (targlenclean(t) == 0) // sets uerr
			return;
	}
	name = argv[0];
	pvp = xlmkpool(name, ""); // sets uerr
	if (pvp == nil)
		return;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-')
			continue;
		t = parsetarget(argv[i]);
		if (xlinitpv(pvp, i - 1, t, -1, 0) < 0) {
			xlrmpool(name);
			uerr("%s removed because of %s error", name, argv[i]);
			return;
		}
	}
	writeconfig();
	xsyslog("Pool %s: restored\n", name);
	kickass();
}

void
xlprevsnap(LV *lv, char *prevsnap)
{
	ulong ps, old;
	char *e;

	ps = strtoul(prevsnap, &e, 0);
	if (*e) {
		uerr("Invalid prevsnap value: %s", prevsnap);
		return;
	}
	wlock(lv);
	old = lv->prevsnap;
	lv->prevsnap = ps;
	savemeta(lv, 0);
	xsyslog("LV %s: prevsnap was %uld, now %uld\n", lv->name, old, lv->prevsnap);
	wunlock(lv);
}

/* Note that 0 is never a valid lvname.N value */
ulong
snapname2num(char *name)
{
	char *e, *n;
	ulong num;

	n = strchr(name, '.');
	if (n == nil || *++n == 0) {
		uerr("Invalid snapshot name: %s\n", name);
		return 0;
	}
	num = strtoul(n, &e, 0);
	if (*e) {
		uerr("Invalid snapshot name: %s\n", name);
		return 0;
	}
	return num;
}

/* WARNING: This assumes that the snapshots present in memory represent the actual,
	correct order of snapshots for an LV. This will overwrite any discrepancies.
	This could fix a problem with a bad prevsnap value, or cause massive data
	corruption for snapshots. */
void
xlfixsnaplist(void)
{
	LVL *op, *p;
	ulong num;
	LV *lv;
	int m;

	m = 0;
	rlock(&lk);
	if (waserror()) {
		runlock(&lk);
		return;
	}
	for (lv = vols; lv; lv = lv->next) {
		if (lv->mode & LVSNAP) {
			continue;
		}
		num = 0;
		snaplist(lv, &op, nil);
		for (p = op; p; p = p->next) {
			wlock(p->l);
			if (p->l->prevsnap != num) {
				xsyslog("%s: prevsnap %uld, updating prevsnap to %uld\n",p->l->name, p->l->prevsnap, num);
				p->l->prevsnap = num;
				savemeta(p->l, 0);
				m++;
			}
			num = snapname2num(p->l->name); /* sets uerr */
			wunlock(p->l);
			if (num == 0) {
				freelvl(op);
				nexterror();
			}
		}
		freelvl(op);
		wlock(lv);
		if (lv->prevsnap != num) {
			xsyslog("%s: prevsnap %uld, updating prevsnap to %uld\n", lv->name, lv->prevsnap, num);
			lv->prevsnap = num;
			savemeta(lv, 0);
			m++;
		}
		wunlock(lv);
	}
	poperror();
	runlock(&lk);
	if (m)
		xsyslog("Please reboot/failover this VSX\n");
}

void
xlthick(LV *lv)
{
	LVE *e;
	ulong i, j, k, fl, th;
	PVPool *pvp;
	LVEBUF *p;
	PV *npv;
	char lves[Xlveperblk];
	ulong exts[Npvsperpool];
	
	fl = 0;
	i = 0;
	p = 0;
	wlock(lv);
	pvp = lv->pool;
	npv = pvp->pv[lv->lve->pid];
	if (waserror()) {
		wunlock(lv);
		return;
	}
	if (lv->mode & LVSNAP)
		error("Snap %s: cannot thick provision snapshot", lv->name);
	if (lv->thin > poolspace(lv->pool))
		error("not enough free space in pool");
	if (waserror()) {
		if (p == nil)
			nexterror();
		for (j = i == 0 ? lv->frstdat : 0; j < Xlveperblk; j++) {
			if (lves[j]) {
				e = (LVE*) p->buf + j;
				freelve(pvp, lv, i + j, e);
				e->flag = LFthin;
			}
		}
		/* this will probably fail, but try anyway */
		qlock(&lclock);
		putlveblk(lv, i, p->buf);
		qunlock(&lclock);
		rlock(pvp);
		flushrefs(pvp);	
		runlock(pvp);
		nexterror();
	}
	for (i = 0; lv->thin && i < lv->nlve; i += j) {
		memset(lves, 0, sizeof lves);
		memset(exts, 0, sizeof exts);
		qlock(&lclock);
		p = getlveblk(lv, i);
		if (p == nil) {
			qunlock(&lclock);
			error("getlveblk failed %s:%uld", lv->name, i);
			return;
		}
		mvhead(p);
		qunlock(&lclock);
		j = i == 0 ? lv->frstdat : 0;
		th = 0;
		while (j < Xlveperblk) {
			if (i + j >= lv->nlve) {
				break;
			}
			e = (LVE*) p->buf + j;
			if (!validlve(e, lv)) {
				error("invalid lve in xlthick for %s: lve:%uld flag:%02x pid:%d off:%ud\n",
					lv->name, i + j, e->flag, e->pid, e->off);
			}
			if (e->flag & LFthin) {
				wlock(pvp);
				if (allocpve1(e, pvp, &npv, 1) < 0) {
					wunlock(pvp);
					error("allocpve1 failure");
				}
				wunlock(pvp); 
				exts[e->pid]++;
				th++;
				fl++;
				lves[j] = 1;
			}
			j++;
		}
		if (th) {
			rlock(pvp);
			if (flushrefs(pvp) < 0) {
				runlock(pvp);
				xsyslog("LV %s: failure to flush refs\n", lv->name);
				error("LV %s: failure to flush refs", lv->name);
			}
			runlock(pvp);
			qlock(&lclock);
			if (putlveblk(lv, i, p->buf) < 0) {
				qunlock(&lclock);
				xsyslog("LV %s: failure to update metadata\n", lv->name);
				error("LV %s: failure to update metadata", lv->name);		
			}
			qunlock(&lclock);
			lv->thin -= th;
			for (k = 0; k < Npvsperpool; k++)
				lv->exts[k] += exts[k];
		}	
	}
	poperror();
	poperror();
	lv->mode &= ~LVTHIN;
	savemeta(lv, 0);
	wunlock(lv);
	xsyslog("LV %s: %uld extents set thick\n", lv->name, fl);
	xsyslog("LV %s: thick provisioned\n", lv->name);
}

void 
xlthin(LV *lv)
{
	LVE *e, *eb;
	ushort *r;
	ulong i, j, k, pt, fl;
	PVPool *pvp;
	LVEBUF *p, bak;
	char lves[Xlveperblk];
	ulong exts[Npvsperpool];

	fl = 0;
	p = nil;
	i = 0;
	wlock(lv);
	pvp = lv->pool;
	if (waserror()) {
		wunlock(lv);
		return;
	}
	wlock(pvp);
	if (waserror()) {
		wunlock(pvp);
		nexterror();
	}
	if (lv->mode & LVSNAP)
		error("Snap %s: snapshots are already thin provisioned", lv->name);
	if (waserror()) {
		if (p == nil) 
			nexterror();
		/* unwrap the freelve work */
		for (k = 0; k < Xlveperblk; k++) {
			if (lves[k]) {
				e = (LVE*) p->buf + k;
				eb = (LVE*) bak.buf + k;
				e->flag = eb->flag;
				e->pid = eb->pid;
				e->off = eb->off;
				r = &pvp->pv[e->pid]->ref[e->off];
				*r = 1;
				xdecu(&pvp->etotal);
			}
		}
		putlveblk(lv, i, p->buf);
		flushrefs(pvp);
		nexterror();
	}
	for (i = 0; i < lv->nlve; i += j) {
		memset(lves, 0, sizeof lves);
		memset(exts, 0, sizeof exts);
		qlock(&lclock);
		p = getlveblk(lv, i);
		if (p == nil) {
			qunlock(&lclock);
			error("xlthin: getlveblk failed %s:%uld", lv->name, i);
		}
		mvhead(p);
		memmove(bak.buf, p->buf, Xblk);
		qunlock(&lclock);
		pt = 0;
		j = i == 0 ? lv->frstdat : 0;
		while (j < Xlveperblk) {
			if (i + j >= lv->nlve) {
				break;
			}
			e = (LVE*) p->buf + j;
			if (!validlve(e, lv)) {
				error("got bad lve in xlthin for %s: lve:%uld flag:%02x pid:%d off:%ud\n",
					lv->name, i + j, e->flag, e->pid, e->off);
			}
			j++;
			if ((e->flag & ~LFnf) == 0 && (pvp->pv[e->pid]->ref[e->off] & REFused) == 0) {
				freelve(pvp, lv, i, e);
				lves[j] = 1;
				exts[e->pid]++;
				e->flag = LFthin | LFnf;
				e->pid = 0;
				e->off = ~0;
				pt++;
				fl++;
			}
		}
		if (pt) {
			qlock(&lclock);
			if (putlveblk(lv, i, p->buf) < 0) {
				qunlock(&lclock);
				xsyslog("xlthin: putlveblk failed\n");
				error("xlthin: putlveblk failed");
			}
			qunlock(&lclock);
			if (flushrefs(pvp) < 0) {
				error("Error flushing references");
			}
		}
		lv->thin += pt;
		for (k = 0; k < Npvsperpool; k++)
			lv->exts[k] -= exts[k];
	}
	poperror();
	poperror();
	wunlock(pvp);
	poperror();
	lv->mode |= LVTHIN;
	savemeta(lv, 0);
	wunlock(lv);
	xsyslog("LV %s: %uld extents set thin\n", lv->name, fl);
	xsyslog("LV %s: thin provisioned\n", lv->name);
}

/* Soli Deo Gloria */
/* Brantley Coile */
