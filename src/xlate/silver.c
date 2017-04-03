#include <u.h>
#include <libc.h>
#include <libcutil.h>
#include "dat.h"
#include "fns.h"

typedef struct Qcopy Qcopy;
struct Qcopy {
	ulong xoff;
	PV *pv;
	XCB *xcb;
	XLock *xlk;
};

static int resilver(PV*);

static Qcopy qcopyio[2], *qcp;

static int
silverqinit(void)
{
	Qcopy *e;

	memset(qcopyio, 0, sizeof qcopyio);
	e = qcopyio + nelem(qcopyio);
	for (qcp = qcopyio; qcp < e; qcp++) {
		qcp->xcb = xcballoc();
		if (qcp->xcb)
			continue;
		xsyslog("Unable to allocate xcb in silverqinit\n");
		while (--qcp >= qcopyio)
			xcbfree(qcp->xcb);
		return -1;
	}
	qcp = qcopyio;
	return 0;
}

static void
silverqfree(void)
{
	for (qcp = qcopyio; qcp < &qcopyio[nelem(qcopyio)]; qcp++)
		xcbfree(qcp->xcb);
	memset(qcopyio, 0, sizeof qcopyio);
}

static int
silverq(PV *pv, ulong xoff)
{
	ulong len;
	Qcopy *last;

	/* get the last submit. qcp is current insertion point */
	last = qcopyio;
	if (last == qcp)
		last++;

	/* first, init and read this extent synchronously */
	qcp->xoff = xoff;
	qcp->xlk = xwlock(pv, xoff);	// exclude pviox writes during copy
	len = Xextent;
	if (pv->flags & PVFpartial && xoff == pv->npve - 1)
		len = pv->length % Xextent;
	xcbinit(qcp->xcb, nil, pv->targ, xoff, len, nil, OREAD);
	xcbrun(qcp->xcb, 1);
	if (qcp->xcb->flag & Xfail) {
		xwunlock(qcp->xlk);
		qcp->xlk = nil;
		return -1;
	}

	/* wait for previous submitted write, xlk == nil on silver start */
	if (last->xlk != nil) {
		xcbwait(last->xcb, 0);
		xwunlock(last->xlk);
		last->xlk = nil;
		if (last->xcb->flag & Xfail)
			return -1;
	}

	/* submit async write and go find next read extent */
	xcbinit(qcp->xcb, nil, pv->mirror, xoff, len, nil, OWRITE);
	xcbrun(qcp->xcb, 0);

	/* prime for the next call */
	qcp = last;
	return 0;		
}

static int
silverqflush(int flag, ulong *fxoff)
{
	Qcopy *q;
	int rf;

	rf = 0;
	for (q = qcopyio; q < &qcopyio[nelem(qcopyio)]; q++) {
		if (q->xlk) {
			xcbwait(q->xcb, flag);
			xwunlock(q->xlk);
			q->xlk = nil;
		}
		rf |= q->xcb->flag;
		if (q->xcb->flag & Xfail)
			*fxoff = q->xoff;
	}
	return rf & Xfail ? -1 : 0;
}

/*
 * PV mirror resilver process.
 * We copy the extents that say they are dirty.
 * While we are resilvering, writes will be happening
 * to both the primary and secondary elements of the mirror.
 * This means that the io routines don't have to know where
 * I am in the resilvering process.  It doesn't really matter
 * if I recopy data that is already there.
 *
 * I clear the dirty bits all at once when I'm finished.
 */

//XXX move err to a per process structure.

void
mirproc(void)
{
	PVPool *pvp;
	PV *pv, opv;
	int i, n;
	
	for (;; sleep(1000)) {
		if (shutdown)
			xlexits(0);
		if (mirrorck == 0)
			continue;
loop:		rlock(&lk);
		for (pvp = pools; pvp; pvp = pvp->next) {
			rlock(pvp);
			for (i = 0; i < Npvsperpool; i++) {
				pv = pvp->pv[i];
				if (pv == nil || pv->state != PVSoosync || (pv->flags & PVFlost))
					continue;
				runlock(&lk);		/* we're commited to rescan the lists with fresh locks */
				runlock(pvp);
				wlock(pv);
				opv = *pv;
				pv->state = PVSresilver;
				//XXX check to see that we aren't in PVSresilver 
				//XXX state when we read the metadata from on load
				pv->flags |= PVFsilvering;
				n = resilver(pv);
				pv->flags &= ~PVFsilvering;
			
				if (n) {
					pv->sysalrt = 0;
					pv->state = PVSdouble;
					pv->flags &= ~PVFfullsilver;
				} else {
					pv->state = PVSbroke;
					pv->sysalrt = 1;
				}
				if (updpv(pv) < 0 && n) {
					pv->state = opv.state;
					pv->flags |= (opv.flags & PVFfullsilver);
					pv->sysalrt = opv.sysalrt;
				}
				wunlock(pv);
				if (n)
					writeconfig();
				if (flushpv(pv) < 0)
					xsyslog(LOGCOM
						"mirproc %T flushpv failed\n",
						pv->targ);
				goto loop;
			}
			runlock(pvp);
		}
		mirrorck = 0;
		runlock(&lk);
	}
}

static int
resilver(PV *pv)		/* make a reflection, called with pv locked */
{
	int i, npve, ncopy, copied;
	ulong xtnt;
	ushort r, *refb;

	npve = pv->npve;
	refb = malloc(npve * sizeof(ushort));
	if (refb == nil) {
		xsyslog("Unable to begin silver %T->%T due to memory allocation failure\n",
			pv->targ, pv->mirror);
		return 0;
	}
	memmove(refb, pv->ref, npve * sizeof(ushort));

	copied = 0;
	for (i = 0, ncopy = 0; i < npve; ++i)
		if (refb[i] & REFused)
			if (pv->flags & PVFfullsilver)
				++ncopy;
			else if (refb[i] & REFdirty)
				++ncopy;

	statinit(&mirrorstat, pv->mirror, pv->targ, (uvlong)ncopy * Xblkperext * Xblk);

	xsyslog("Silver %T->%T will copy %d extents\n",
		pv->targ, pv->mirror, ncopy);

	/*
	 * Because targxtntcopy sleeps, we're going to release the lock on pv
	 * and reaquire it before returning.  npve can't change, and we want
	 * pv->flags to be able to change so we can abort.  That leaves pv->targ
	 * and pv->mirror.  If they change out from under us, things could
	 * get weird.
	 */
	wunlock(pv);

	if (silverqinit() < 0) {
		free(refb);
		print("Unable to begin silver %T->%T due to silverqinit failure\n", pv->targ, pv->mirror);
		return 0;
	}
	for (i = 0; i < npve; i++) {
		if (shutdown)
			xlexits(0);
		if (pv->flags & PVFabort) {
			print("Silver %T->%T aborted after %d of %d extents copied\n",
			      pv->targ, pv->mirror, copied, ncopy);
			break;
		}
		r = refb[i];
		if ((r & REFused) == 0)
			continue;
		if (!(pv->flags & PVFfullsilver) && !(r & REFdirty))
			continue;
		if (silverq(pv, i) < 0)
			break;
		statinc(&mirrorstat, Xextent, 0);
		copied++;
	}
	if (silverqflush(i != npve ? Xstop : 0, &xtnt) < 0) {
		xsyslog("Silver %T->%T failure at extent %ld after %d of %d extents copied\n",
			pv->targ, pv->mirror, xtnt, copied, ncopy);
		i = 0;	/* to get a return 0 */
	}
	silverqfree();
	free(refb);
	statclr(&mirrorstat);
	wlock(pv);
	if (i != npve)
		return 0;
	for (i = 0; i < npve; i++) {
		r = refb[i];
		if (r & REFdirty) {
			pv->ref[i] &= ~REFdirty;
			pv->ref[i] |= REFnf;
		}
	}
	xsyslog("Silver %T->%T completed after %d extents copied\n",
		pv->targ, pv->mirror, copied);
	return 1;
}
