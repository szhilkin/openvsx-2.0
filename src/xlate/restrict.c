#include <u.h>
#include <libc.h>
#include <ip.h>
#include "dat.h"
#include "fns.h"

enum {
	MCread	= 	0,
	MCedit,
	DCnoop	=	0,
	DCadd,
	DCdel,
	Nres	=	Npools*Npvsperpool*2, /* max number of pvs plus mirrors (x2) */
};

struct {
	int res[Nres];
	int xres[Nres];
	int nres;
	Rendez;
	QLock;
} resq;

static	int	resit(int, int);
static	int	aoeerr(Msg*);
static	AoeMac*	amread(int);
static	void	amset(AoeMac*, int);
static	void	amaddself(AoeMac*, int);
static	void	amaddpeer(AoeMac*, int);
static	void	amadd(AoeMac*, uchar*, int);
static	void	resproc(void);
static	void	xresproc(void);

void
trestrict(int targ)
{
	int i, x;

	if (targ == -1)
		return;
	x = -1;
	qlock(&resq);
	for (i=0; i<resq.nres; i++) {
		if (resq.res[i] == targ) {
			if (resq.xres[i] == -1) { /* try again */
				x = i;
				break;
			} else {
				qunlock(&resq);
				return;
			} 
		}
		if (x == -1)
		if (resq.res[i] == -1)
		if (resq.xres[i] == -1)
			x = i;
	}
	if (x == -1) {
		if (resq.nres == Nres) {
			/* we're pretty much screwed if we get here */
			resq.nres = 0;
			xsyslog("memory exhausted for mask restriction.  giving up on restrict mode\n");
			qunlock(&resq);
			for (;;)
				sleep(10*1000);
		}
		x = resq.nres++;
	}
	resq.res[x] = targ;
	resq.xres[x] = -1;
	rwakeup(&resq);
	qunlock(&resq);
}

void
tunrestrict(int targ)
{
	int i;

	if (targ == -1)
		return;
	qlock(&resq);
	for (i=0; i<resq.nres; i++)
		if (resq.res[i] == targ) {
			resq.res[i] = -1;
			rwakeup(&resq);
			break;
		}
	qunlock(&resq);
}

void
tforget(int targ)
{
	int i;

	if (targ == -1)
		return;
	qlock(&resq);
	for (i = 0; i < resq.nres; i++)
		if (resq.res[i] == targ || resq.xres[i] == targ) {
			resq.res[i] = -1;
			resq.xres[i] = -1;
			break;
		}
	qunlock(&resq);
}	

void
restrictpeer(void)
{
	int i;

	qlock(&resq);
	for (i=0; i<resq.nres; i++)
		if (resq.res[i] != -1)
			resq.xres[i] = -1;
	rwakeup(&resq);
	qunlock(&resq);
}

int
setpeerea(char *buf)		/* called with peerealck */
{
	char *toks[Nmyea+1];
	int n, i;

	n = tokenize(buf, toks, Nmyea+1);
	for (i = 0; i < n; ++i)
		if (dec16(peerea + 6 * (i), 6, toks[i], 12) < 6) {
			uerr("bad address %s", toks[i]);
			break;
		}
	peereaindx = i;
	return i == n;
}

void
getpeerea(void)
{
	char buf[2048];
	int n;

	n = readfile(buf, sizeof buf, "/n/kfs/conf/ha/peerea");
	if (n <= 0)
		return;
	buf[n] = 0;
	wlock(&peerealck);
	setpeerea(buf);		/* sets uerr */
	wunlock(&peerealck);
}

void
resinit(void)
{
	fmtinstall('E', eipfmt);
	resq.l = &resq;
	resq.nres = 0;
	getpeerea();
	if (xlrfork(RFPROC|RFMEM, "resproc") == 0)
		resproc();
	if (xlrfork(RFPROC|RFMEM, "xresproc") == 0)
		xresproc();
}

static
void
killres(void)
{
	int i;

	for (i=0; i<resq.nres; i++) {
		if (resq.xres[i] != -1)
		if (resit(resq.xres[i], 0))
			resq.xres[i] = -1;
	}
}

static
void
xresproc(void)
{
	while (shutdown == 0)
		sleep(1);
	qlock(&resq);
	killres();
	rwakeup(&resq);
	qunlock(&resq);
	sleep(3000);				// give others a chance to fail cleanly
	postnote(PNGROUP, getpid(), "kill");	// game over, folks
	exits(0);
}

static
void
resproc(void)
{
	int i;

	qlock(&resq);
	for (;;) {
		rsleep(&resq);
		if (shutdown) {
			qunlock(&resq);
			xlexits(0);
		}
		for (i=0; i<resq.nres; i++) {
			if (resq.res[i] != -1) {
				if (resq.xres[i] == -1)	/* restrict */
				if (resit(resq.res[i], 1)) 
					resq.xres[i] = resq.res[i];
			} else if (resq.xres[i] != -1)	/* unrestrict */
				if (resit(resq.xres[i], 0))
					resq.xres[i] = -1;
		}
	}
}

static
void
amdebug(AoeMac *am)
{
	int i;

	if (am == nil) {
		xsyslog("am: nil\n");
		return;
	}
	xsyslog("am: mcmd=%d merror=%d dircnt=%d\n", am->mcmd, am->merror, am->dircnt);
	for (i=0; i<am->dircnt; i++)
		xsyslog("\tdcmd=%d ea=%E\n", am->dir[i].dcmd, am->dir[i].ea);
}

static
int
resit(int targ, int claim)
{
	Msg *m, *n;
	AoeMac *am;
	Aoe *a;

	if (waserror()) {
		xsyslog("%s\n", u->errstr);
		return 0;
	}
	/* fetch mac list */
	am = amread(targ);
	if (am == nil) {
		if (!shutdown)
			error("error fetching mask list for %T\n", targ);
		return 0;
	}
	am->mcmd = MCedit;
	amset(am, DCdel);
	if (claim == 0) {
		if (restricted && shutdown) {	// secondary adds self and peer on failover
			amaddself(am, DCadd);
			amaddpeer(am, DCadd);
		}
	} else {
		amaddself(am, DCadd);	
		if (restricted == 0)		// primary adds peer
			amaddpeer(am, DCadd);
	}
	m = prepmsg(targ, 24 + 4 + 8*am->dircnt);
	if (m == nil) {
		free(am);
		error("resit: allocation failure pm: %r\n");
	}
	poperror();
	a = (Aoe *) m->data;
	a->proto = ACMASK;
	convML2M(am, m->data+24, m->count-24);
	free(am);
	u->tag->maxwait = 1000;
	n = tagsend(u->tag, m);
	if (n == nil || aoeerr(n)) {
		am = nil;
	}
	free(m);
	free(n);
	return am != nil;
}

static
int
aoeerr(Msg *m)
{
	Aoe *a;

	a = (Aoe *)m->data;
	if (a->vf & AERR)
		return a->err;
	return 0;
}

static
AoeMac *
amread(int targ)
{
	Msg *m, *n;
	Aoe *a;
	AoeMac *am;
	AoeMDir *d;

	am = mallocz(sizeof *am + 255 * sizeof *d, 1);
	if (am == nil)
		return nil;
	m = prepmsg(targ, 1500);
	if (m == nil) {
		free(am);
		return nil;
	}
	a = (Aoe *) m->data;
	a->proto = ACMASK;
	convML2M(am, m->data + 24, m->count - 24);
	u->tag->maxwait = 2000;
	n = tagsend(u->tag, m);
	if (n == nil || aoeerr(n) || !convM2ML(n->data+24, n->count-24, am)) {
		free(am);
		am = nil;
	}
	free(m);
	free(n);
	return am;
}

static
void
amset(AoeMac *am, int dc)
{
	AoeMDir *d, *e;

	d = am->dir;
	e = d + am->dircnt;
	for (; d<e; d++)
		d->dcmd = dc;
}

static
void
amaddself(AoeMac *am, int d)
{
	int i;

	for (i=0; i<myeaindx; i++)
		amadd(am, &myea[i*6], d);
}

static
void
amaddpeer(AoeMac *am, int d)
{
	int i;

	for (i=0; i<peereaindx; i++)
		amadd(am, &peerea[i*6], d);
}

static
void
amadd(AoeMac *am, uchar *ea, int dc)
{
	AoeMDir *d, *e;

	d = am->dir;
	e = d + am->dircnt;
	for (; d<e; d++) {
		if (memcmp(d->ea, ea, 6) == 0) {
			d->dcmd = dc;
			return;
		}
	}
	if (am->dircnt < 255) {
		memmove(e->ea, ea, 6);
		e->dcmd = dc;
		am->dircnt++;
	}
}
