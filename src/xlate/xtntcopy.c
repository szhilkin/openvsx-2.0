#include <u.h>
#include <libc.h>
#include "dat.h"
#include "fns.h"

static void xcbvalid(XCB *);

XCB	xcb[NXCB];
QLock	xcblk;
Rendez	xcbavail;

int
pvxtntcopy(PV *npv, int noff, PV *pv, int off)
{
	return xtntcopy(npv, -1, noff, pv, -1, off, Xextent);
}

int
targxtntcopy(int dst, int doff, int src, int soff, int len)
{
	return xtntcopy(nil, dst, doff, nil, src, soff, len);
}

int
bufxtntcopy(PV *pv, int xoff, void *buf, int len, int mode)
{
	return xtntio(pv, -1, xoff, len, buf, mode);
}

XCB *
xcballoc(void)
{
	XCB *x;

	qlock(&xcblk);
again:
	for (x = xcb; x < &xcb[NXCB]; x++)
		if (x->flag == Xfree)
			break;
	if (x >= &xcb[NXCB]) {
		xcbwaits++;
		rsleep(&xcbavail);
		goto again;
	}
	x->flag = 0;
	qunlock(&xcblk);
	return x;
}

XCB *
xcbinit(XCB *x, PV *pv, int targ, ulong xoff, ulong len, void *buf, int mode)
{
	xcbvalid(x);
	if (pv && targ != -1)
		panic("xcbinit e0");
	if (pv == nil && targ == -1)
		panic("xcbinit e1");
	if (mode != OREAD && mode != OWRITE)
		panic("xcbinit e2");

	x->flag = 0;
	x->pv = pv;
	x->targ = targ;
	x->off = (vlong) xoff * Xextent;
	if (pv && x->off + len > pv->length)
		len = pv->length - x->off;
	x->length = len;
	x->buf = buf != nil ? buf : x->xbuf;
	x->mode = mode;
	return x;
}

void
xcbfree(XCB *x)
{
	xcbvalid(x);
	qlock(&xcblk);
	x->flag = Xfree;
	rwakeup(&xcbavail);
	qunlock(&xcblk);
}

void
xcbrun(XCB *x, int wait)
{
	xcbvalid(x);
	qlock(&x->qlk);
	rwakeupall(&x->worker);
	if (wait)
		rsleep(&x->user);
	qunlock(&x->qlk);
}

void
xcbwait(XCB *x, int flag)
{
	xcbvalid(x);
	qlock(&x->qlk);
	if (x->flag == 0 || x->nworking) {
		x->flag |= flag;
		rsleep(&x->user);
	}
	qunlock(&x->qlk);
}

static void
xcbvalid(XCB *x)
{
	if (x < xcb || x >= &xcb[NXCB])
		panic("invalid xcb");
}

int
xtntio(PV *pv, int targ, ulong xoff, ulong len, void *buf, int mode)
{
	XCB *x;
	int flag;

	x = xcballoc();
	xcbinit(x, pv, targ, xoff, len, buf, mode);
	xcbrun(x, 1);
	flag = x->flag;
	xcbfree(x);
	return flag & Xfail ? -1 : 0;
}

/*
 * copy extent from:
 *	pv/targ to npv/ntarg
 *	pv to buf
 *	buf to npv
 */

int
xtntcopy(PV *npv, int ntarg, ulong noff, PV *pv, int targ, ulong off,
	 ulong len)
{
	int flag;
	XCB *x;

	x = xcballoc();
	xcbinit(x, pv, targ, off, len, nil, OREAD);
	xcbrun(x, 1);
	if (x->flag & Xfail)
		goto out;
	xcbinit(x, npv, ntarg, noff, len, nil, OWRITE);
	xcbrun(x, 1);
out:
	flag = x->flag;
	xcbfree(x);
	return flag & Xfail ? -1 : 0;
}

static void
worker(XCB *x)	/* worker for this xcb */
{
	int n, f;
	PV *pv;
	int targ;
	vlong off, length;
	char *db;

	qlock(&x->qlk);
	for (;;) {
		rsleep(&x->worker);
		if ((x->flag & Xexit) || shutdown) {
			qunlock(&x->qlk);
			xlexits(nil);
		}
		x->nworking++;
		pv = x->pv;
		targ = x->targ;
		f = 0;
		for (;;) {
			if (shutdown)
				break;
			if (x->length <= 0 || x->flag & (Xstop|Xfail))
				break;
			off = x->off;
			x->off += Xblk;
			length = x->length > Xblk ? Xblk : x->length;
			x->length -= length;
			db = x->buf;
			if (db == nil)
				panic("inappropriate use of xtnt workers");
			x->buf += Xblk;

			if (fakextntioread && x->mode == OREAD)
				continue;
			else if (fakextntiowrite && x->mode == OWRITE)
				continue;

			qunlock(&x->qlk);

			if (pv)
				n = pvio(pv, db, length, off, x->mode);
			else
				n = edio(targ, db, length, off, x->mode);

			qlock(&x->qlk);

			if (n != length) {
				f = 1;
				break;
			}
		}
		if (f)
			x->flag |= Xfail;
		x->nworking--;
		if (x->nworking == 0) {
			x->flag |= Xdone;
			rwakeupall(&x->user);
		}
	}
}


void
xtntcopyinit(void) /* for each xcb, make nprocs to work it */
{
	XCB *x;
	int i;
	
	xcbavail.l = &xcblk;
	for (x = xcb; x < &xcb[NXCB]; x++) {
		x->worker.l = &x->qlk;
		x->user.l = &x->qlk;
		x->xbuf = malloc(Xextent);
		x->flag = Xfree;
		if (x->xbuf == nil)
			panic("unable to alloc Xextent for xtntcopy");
		for (i = 0; i < nworkers; i++)
			if (xlrfork(RFPROC|RFMEM, "worker%ld", x-xcb) == 0)
				worker(x);
	}
}

