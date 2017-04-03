#include <u.h>
#include <libc.h>
#include <bio.h>
#include <libsec.h>
#include <libcutil.h>
#include "dat.h"
#include "fns.h"
#include "shadow.h"

static void
shadowchks(LV *lv, char *rname, char *rlv) /* called with lv & lk locked */
{
	Remote *r;
	LV *l;

	if (lv->mode & LVSNAP)
		error("cannot set shadow on snapshot LV; choose parent LV");
	if (isshadow(lv))
		error("unshadow first: unshadow %s", lv->name);
	if (!(r = remoteget(rname)))
		error("unknown remote name %s; see help setremote", rname);
	remotefree(r);

	if (strcmp(rlv, "unset") == 0)
		error("%s not allowed", rlv);

	for (l = vols; l; l = l->next) {
		if (isshadow(l))
		if (strcmp(l->rmtname, rname) == 0)
		if (strcmp(l->rmtlv, rlv) == 0)
			error("LV %s is using %s:%s for %s",
			      l->name, rname, rlv,
			      isshadowsend(l) ? "shadowsend" : "shadowrecv" );
	}
}

void
shadowrecv(LV *lv, char *rname, char *srclv, char *opts)
{
	rlock(&lk);
	wlock(lv);
	if (waserror()) {
		wunlock(lv);
		runlock(&lk);
		return;
	}
	shadowchks(lv, rname, srclv);
	if (lv->mode & LVLUN)
		error("remove LUN first: rmlun %T", lun2targ(lv->lun));
	if (*opts == '0' && lv->dirty - lv->frstdat > 0)
		error("%s has dirty data extents contact support@coraid.com",
		      lv->name);
	lv->rmtname = strdup(rname);
	lv->rmtlv = strdup(srclv);
	lv->flags &= ~LVFshstop;
	poperror();
	savemeta(lv, 0);
	wunlock(lv);
	runlock(&lk);
}

int shadowport = 17760;
static char *ldial = "tcp!*!17760";
static char *certfile = "/n/kfs/lib/cert";

void
shadowsend(LV *lv, char *rname, char *tgtlv)
{
	int fd, snaps, shadowsnap;

	rlock(&lk);
	wlock(lv);
	if (waserror()) {
		wunlock(lv);
		runlock(&lk);
		return;
	}
	shadowchks(lv, rname, tgtlv);
	snaps = lv->snaps;
	free(lv->rmtlv);
	lv->rmtlv = strdup(tgtlv);
	poperror();
	wunlock(lv);
	runlock(&lk);

	if ((fd = shadowclient(rname, certfile, lv->flags & LVFshdebug)) < 0) {
		uerr("%r");
		xsyslog("shadowsend LV %s %r\n", lv->name);
		return;
	} else if ((shadowsnap = shadowsendhs(lv, fd)) < 0) {
		uerr("%r");
		xsyslog("shadowsend LV %s failed %r\n", lv->name);
		close(fd);
		return;
	}
	fprint(fd, "END\n");
	close(fd);
	xsyslog("shadowsend %s %s to %s\n", rname, lv->name, tgtlv);
	rlock(&lk);
	wlock(lv);
	lv->flags &= ~LVFshstop;
	lv->mode |= LVSEND;
	lv->rmtname = strdup(rname);
	if (shadowsnap > 0 || snaps != lv->snaps) {
		lv->copysnap = shadowsnap - 1;
		snapcopyck(1);
	} else {
		lv->copysnap = lv->snaps;
		xsyslog("shadowsend LV %s has no snapshots to send\n",
			lv->name);
	}
	savemeta(lv, 0);
	wunlock(lv);
	runlock(&lk);
}

LV *
shadowrecvlv(char *rname, char *srclv)
{
	LV *l;

	rlock(&lk);
	for (l = vols; l; l = l->next) {
		if (isshadowrecv(l) && strcmp(l->rmtname, rname) == 0) {
			if (srclv) {
				if (strcmp(l->rmtlv, srclv) == 0)
					break;
			} else
				break;
		}
	}
	runlock(&lk);
	return l;
}

int
unshadow(LV *lv)
{
	wlock(&lk);	// wlock to exclude tocopy()
	wlock(lv);
	if (waserror()) {
		wunlock(lv);
		wunlock(&lk);
		return -1;
	}
	if (isshadow(lv)) {
		if (stopshadow(lv) < 0)
			error("cannot unshadow LV %s, shadow in progress",
			      lv->name);
		if (isshadowrecv(lv))
			clrsched(lv);
	} else {
		error("%s has no shadowrecv or shadowsend", lv->name);
	}
	poperror();
	free(lv->rmtname);
	free(lv->rmtlv);
	lv->rmtname = lv->rmtlv = nil;
	lv->mode &= ~LVSEND;
	savemeta(lv, 0);
	wunlock(lv);
	wunlock(&lk);
	xsyslog("unshadow %s\n", lv->name);
	return 0;
}

static void
incstotal(LV *l, ulong inc)
{
	qlock(&l->quick);
	if (inc)
		l->total += inc;
	else
		l->total = 0;
	qunlock(&l->quick);
}

static void
incsbytes(LV *l, ulong bytes)
{
	qlock(&l->quick);
	l->bytes += bytes;
	qunlock(&l->quick);
	incfilter(&l->filter, bytes, 0);
}

static int
canshadow(LV *lv)
{
	return !shutdown && (lv->flags & LVFshstop) == 0;
}

static int
xtntcp(LV *lv, ulong lvxtnt, PV *pv, ulong pvxtnt)
{
	vlong sbytes, lvoff;
	char *b;

	wlock(lv);
	if (!lv->b && !(lv->b = malloc(Xextent))) {
		wunlock(lv);
		uerr("malloc failed");
		return -1;
	}
	b = lv->b;
	wunlock(lv);

	lvoff = lvxtnt * Xextent;

	if (lvoff + Xextent > lv->length)
		sbytes = (lv->length - lvoff) & ~0x1ff;
	else
		sbytes = Xextent;

	if (bufxtntcopy(pv, pvxtnt, b, sbytes, OREAD) == 0) {
		if (fprint(lv->fd, "DATA %ulld %lld\n", lvoff, sbytes) < 0) {
			uerr("msg write failed %r");
			return -1;
		}
		if (write(lv->fd, b, sbytes) < sbytes) {
			uerr("xtnt write failed %r");
			return -1;
		}
	} else {
		uerr("xtnt read failed");
		return -1;
	}
	return 0;
}

static
int
gotsnapack(LV *send, LV *l)
{
	Biobuf brd, *br;
	Cmdbufidx cbidx, *cbi;
	int ret;

	br = &brd;
	cbi = &cbidx;
	cbiinit(cbi);

	if (Binit(br, send->fd, OREAD) == Beof) {
		xsyslog("shadow gotsnapack error #1\n");
		return 0;
	}
	cbi->tsecs = 60; // allow 60 seconds for remote snapshot completion

	if (Bnextcmd(br, nil, cbi, SHSsnap) < 0) {
		xsyslog("shadowsend %r\n");
		ret = 0;
	}
	else if (l->sched[0].class != atoi(cbi->cb->f[1])
		   || l->snaps != atoi(cbi->cb->f[3])
		   || l->ctime != strtoul(cbi->cb->f[4], nil, 10)) {
		xsyslog("shadow gotsnapack error #3\n");
		ret = 0;
	} 
	else {
		ret = 1;
	}
	Bterm(br);
	free(cbi->cb);
	return ret;
}

/*
 * look thru all the extents and copy anything that has been
 * written.
 */

static
int
copytoshadow(LV *send, LV *l)
{
	LVE e;
	ulong xcpy, xtnt;
	int i;
	PVPool *pvp;
	vlong lastxtnt;
	char stime[32];

	qlock(&send->quick);
	send->bytes = 0;
	send->total = 0;
	qunlock(&send->quick);
	zfilter(&send->filter);

	wlock(send);
	if (send->lastoffset) {
		lastxtnt = send->lastoffset / Xextent;
		send->lastoffset = 0;
	} else {
		lastxtnt = 0;
	}
	send->copysnapsend = l->snaps;
	wunlock(send);
	if (fprint(send->fd, "COPY %d %uld\n", l->sched[0].class, l->snaps) < 0) {
		print("print failed %r\n");
		return 0;
	}
	xcpy = 0;	
	rlock(l);
	for (i = l->frstdat; i < l->nlve; i++) {
		xtnt = i - l->frstdat;
		if (lastxtnt && xtnt <= lastxtnt)
			continue;
		if (fetchlve(l, i, &e) == nil) {
			runlock(l);
			return 0;
		}
		if (e.flag & LFdirty) {
			incstotal(send, Xextent);
			xcpy++;
		}
	}
	if (lastxtnt)
		xsyslog("shadowsend %s to %s LV %s resuming %uld extents\n",
			l->name, send->rmtname, send->rmtlv, xcpy);
	else
		xsyslog("shadowsend %s to %s LV %s copying %uld extents\n",
			l->name, send->rmtname, send->rmtlv, xcpy);
	xcpy = 0;
	pvp = l->pool;
	for (i = l->frstdat; i < l->nlve; i++) {
		if (shutdown) {
			runlock(l);
			xlexits(0);
		}
		if (send->flags & LVFshstop) {
			xsyslog("shadowsend %s aborted\n", l->name);
			runlock(l);
			incstotal(send, 0);
			return 0;
		}
		xtnt = i - l->frstdat;
		if (lastxtnt && xtnt <= lastxtnt)
			continue;

		if (fetchlve(l, i, &e) == nil) {
			runlock(l);
			return 0;
		}
		if ((e.flag & LFdirty) == 0)
			continue;

		runlock(l); // xtntcp blocks, so we have to release the lock

		if (e.flag & LFthin) {
			xsyslog("attempting to shadow a thin extent: %s: %d\n",
				l->name, i);
			incstotal(send, 0);
			return 0;
		}
		if (xtntcp(send, xtnt, pvp->pv[e.pid], e.off) < 0) {
			xsyslog("shadowsend %s failed at extent %uld after %uld extents copied\n",
				l->name, xtnt, xcpy);
			xsyslog(LOGCOM "shadowsend %s %r\n", l->name);
			incstotal(send, 0);
			return 0;
		}
		incsbytes(send, Xextent);
		rlock(l);
		xcpy++;
	}
	runlock(l);
	incstotal(send, 0);
	schedtostimestr(l->sched, stime, sizeof stime);
	if (fprint(send->fd, "SNAP %d %s %uld %uld\n",
 		   l->sched[0].class, stime, l->snaps, l->ctime) < 0)
		return 0;
	if (!gotsnapack(send, l))
		return 0;
	wlock(send);
	send->copysnap = l->snaps;
	savemeta(send, 0);
	wunlock(send);

	xsyslog("shadowsend %s to %s LV %s copied %uld extents\n",
		l->name, send->rmtname, send->rmtlv, l->dirty - l->frstdat);
	return 1;
}

static LV *
findnextsnap(LV *lv, int nextsnap)
{
	LVL *snaps, *s;
	LV *snap;
	int ret;

	rlock(&lk);
	ret = snaplist(lv, &snaps, nil);
	runlock(&lk);

	if (ret < 0)
		return nil;

	snap = nil;
	for (s = snaps; s; s = s->next) {
		if (s->l->snaps >= nextsnap) {
			snap = s->l;
			break;
		}
	}
	freelvl(snaps);
	return snap;
}

static void
sendproc(int)
{
	LV *lv, *snap;
	int fd, nextsnap;
	ulong snaps;

	rlock(&lk);
	lv = lookuplv(u->name);
	runlock(&lk);

	if (!lv) {
		xsyslog("shadowsend LV %s look up failure\n", u->name);
		xlexits(nil);
	}
	addfilter(&lv->filter);
l:
	while (canshadow(lv)) {
		if ((fd = shadowclient(lv->rmtname, certfile, 1)) < 0) {
			xsyslog("shadowsend LV %s connection failed %r\n",
				lv->name);
			if (canshadow(lv) && sleep(30000) < 0)
				break;
			continue;
		}
		wlock(lv);
		snaps = lv->snaps;
		lv->fd = fd;
		wunlock(lv);
		nextsnap = shadowsendhs(lv, fd);

		if (nextsnap < 0) {
			xsyslog("shadowsend LV %s failed %r\n", lv->name);
			close(fd);
			if (canshadow(lv) && sleep(30000) < 0)
				break;
			continue;
		}
		if (nextsnap > 0) {
			while ((snap = findnextsnap(lv, nextsnap))) {
				if (copytoshadow(lv, snap)) {
					nextsnap = snap->snaps + 1;
				} else {
					close(fd);
					if (canshadow(lv))
						sleep(30000);
					goto l;
				}
			}
		}
		fprint(fd, "END\n");
		close(fd);
		wlock(lv);
		if (snaps == lv->snaps) {
			lv->pid = 0;
			lv->fd = 0;
			free(lv->b);
			lv->b = 0;
			delfilter(&lv->filter);
			xsyslog("shadowsend LV %s has no snapshots to send\n",
				lv->name);
			if (lv->copysnap != lv->snaps) {
				lv->copysnap = lv->snaps;
				savemeta(lv, 0);
				xsyslog(LOGCOM "LV %s copysnap is now %uld\n",
					lv->name, lv->snaps);
			}
			wunlock(lv);
			xlexits(nil);
		}
		wunlock(lv);
		sleep(30000);
	}
	delfilter(&lv->filter);
	wlock(lv);
	lv->pid = 0;
	lv->fd = 0;
	wunlock(lv);
	xlexits(nil);
}

static jmp_buf shadowsendjmp;

static void
maxshadowslog(int set)
{
	static char setlog = 0;

	if (set && !setlog) {
		setlog = set;
		xsyslog("Shadow connections at %d limit\n", maxshadows);
	}
	if (!set && setlog) {
		setlog = set;
		xsyslog("Shadow connections under %d limit\n", maxshadows);
	}
}

static void
tocopy(void)
{
	LV *lv;
	int pid, shadows;

	shadows = 0;
	rlock(&lk);
	/*
	 * Count active shadow connections.
	 */ 
	for (lv = vols; lv && !shutdown; runlock(lv), lv = lv->next) {
		if (shadows >= maxshadows) {
			runlock(&lk);
			maxshadowslog(1);
			return;
		}
		rlock(lv);
		if (!isshadow(lv))
			continue;
		if (lv->flags & LVFshstop)
			continue;
		if (lv->pid) {
			shadows++;
			continue;
		}
	}
	/*
	 * Start new shadowsends.
	 */ 
	for (lv = vols; lv && !shutdown; runlock(lv), lv = lv->next) {
		if (shadows >= maxshadows) {
			runlock(&lk);
			maxshadowslog(1);
			return;
		}
		rlock(lv);
		if (!isshadowsend(lv))
			continue;
		if (lv->flags & LVFshstop)
			continue;
		if (!(lv->flags & LVFallsnaps))
			continue;
		if (lv->pid)
			continue;
		if (lv->copysnap == lv->snaps)
			continue;
		pid = xlrfork(RFPROC|RFMEM|RFNOWAIT|RFFDG, "%s", lv->name);
		switch (pid) {
		case 0:
			longjmp(shadowsendjmp, u->pid);
			xlexits("longjmp shadowsendjmp failure");
		case -1:
			xsyslog("LV %s: shadowsend process create failed\n",
				lv->name);
			break;
		default:
			shadows++;	// count new shadowsends
			lv->pid = pid;
			lv->fd = -1;	// see stopshadow()
		}
	}
	maxshadowslog(0);
	runlock(&lk);
}

int
snapcopyck(int val) // set new flag and return old flag
{
	static QLock cklk;
	static int ckval;
	int ret;

	qlock(&cklk);
	ret = ckval;
	ckval = val;
	qunlock(&cklk);

	return ret;
}

void
shadowsendproc(void)
{
	int n;

	if ((n = setjmp(shadowsendjmp))) {
		sendproc(n);
		xlexits(nil);
	}
	for (;; sleep(1000)) {
		if (shutdown) {
			xlexits(0);
		}
		if (snapcopyck(0)) {
			tocopy();
			n = 0;
		} else if ((++n % 30) == 0) // hack: check every 30s when idle
			tocopy();
	}
}

static int
recvdatablk(Biobuf *br, char *buf, int bytes)
{
	long bread;

	while (bytes > 0) {
		bread = Bread(br, buf, bytes);
		if (bread < 0)
			return -1;
		buf += bread;
		bytes -= bread;
	}
	return 0;
}

static void
updlp(LV *lv)
{
	char snapname[32];
	LV *lp;

	rlock(&lk);
	wlock(lv);
	lv->nse++;
	snprint(snapname, sizeof(snapname), "%s.%uld", lv->name, lv->prevsnap);
	lp = lookuplv(snapname);
	if (lp == nil) {
		xsyslog("Unable to obtain %s\n",snapname);
		wunlock(lv);
		runlock(&lk);
		return;
	}
	wlock(lp);
	lp->nse++;
	wunlock(lp);
	wunlock(lv);
	runlock(&lk);
}

static int
recvdata(LV *lv, Biobuf *br, vlong offset, long bytes)
{
	vlong xtoff;
	LVE e;
	PV *pv;
	int nse;
	char *b;
	PVE pve;

	nse = 0;
	wlock(lv);
	if (!lv->b && !(lv->b = malloc(Xextent))) {
		wunlock(lv);
		werrstr("buffer malloc failure");
		return -1;
	}
	b = lv->b;

	if (bytes == Xextent && (offset % Xextent) == 0) { // full extent
		xtoff = offset/Xextent + lv->frstdat;

		if (fetchlve(lv, xtoff, &e) == nil) {
			wunlock(lv);
			werrstr("fetchlve %lld failed", xtoff);
			return -1;
		}
		if (!(e.flag & (LFdirty|LFthin))) {
			pv = lv->pool->pv[e.pid];
			if (pv && REFCNT(pv->ref[e.off]) > 1) {	// avoid COW
				wlock(lv->pool);
				pve = newpve1(lv->pool, &pv, 0);
				wunlock(lv->pool);
				if (pve.pid == -1) {
					wunlock(lv);
					werrstr("newpve1 failed");
					return -1;
				}
				freelve(lv->pool, lv, xtoff, &e);
				e.flag = LFnf;
				e.pid = pve.pid;
				e.off = pve.off;
				setlve(lv, xtoff, &e);
				nse = 1;
			}
		}
	}
	wunlock(lv);
	if (nse)
		updlp(lv);
	if (recvdatablk(br, b, bytes) < 0) {
		return -1;
	}
	if (bytes != lvio(lv, b, bytes, offset, OWRITE)) {
		werrstr("lvio bytes %ld uerr %s percentr %r",
			bytes, u->err);
		return -1;
	}
	wlock(lv);
	lv->lastoffset = (xtoff - lv->frstdat) * Xextent;
	wunlock(lv);
	return 0;
}

static LV *
recvsnap(LV *lv, short snapclass, char *stime, int snapnum, ulong ctime)
{
	int ret;
	Snapsched s, *sp;
	LV *rlv;

	if (snapclass == LVSmanual) {
		sp = nil;
	} else {
		memset(&s, 0, sizeof s);
		s.class = snapclass;
		parsestime(&s, stime);
		s.retain = sched2retain(lv, &s);
		sp = &s;
	}
	wlock(&lk);
	wlock(lv);

	ret = snapclone(lv, OREAD, ctime, nil, sp, snapnum, &rlv);
	if (ret < 0) {
		werrstr("snap %s.%d create failed: %s",
			lv->name, snapnum, u->err);
	} else {
		lv->copyclass = 0;
		lv->copysnap = 0;
		lv->lastoffset = 0;
		savemeta(lv, 0);
	}
	wunlock(lv);
	wunlock(&lk);
	if (ret >= 0) {
		fprint(lv->fd, "SNAP %d %s %d %uld\n",
		       snapclass, stime, snapnum, ctime);
		return rlv;
	}
	return nil;
}

char *shadowclose = "shadowclose";

static void
recvproc(int)
{
	Biobuf brd, *br;
	Cmdbufidx cbidx, *cbi;
	LV *lv, *rlv;
	ulong datacnt;

	rlock(&lk);
	lv = lookuplv(u->name);
	runlock(&lk);

	if (!lv) {
		xsyslog("LV %s: shadowrecv look up failure\n", u->name);
		xlexits(nil);
	}
	br = &brd;
	cbi = &cbidx;
	cbiinit(cbi);

	if (Binit(br, lv->fd, OREAD) == Beof) {
		xsyslog("shadow recvproc error #1\n");
		return;
	}
	xsyslog("LV %s: shadowrecv connected\n", lv->name);
copy:
	if (!canshadow(lv)) {
		werrstr("shadow close");
		goto x;
	}
	if (Bnextcmd(br, nil, cbi, -1) < 0) {
		goto x;
	}
	switch (cbi->cmd->index) {
	case SHScopy:
		wlock(lv);
		lv->copyclass = atoi(cbi->cb->f[1]);
		lv->copysnap = atoi(cbi->cb->f[2]);
		wunlock(lv);
		break;
	case SHSend:
		xsyslog("LV %s: shadowrecv disconnected\n", lv->name);
		goto y;
	}
	datacnt = 0;
data:
	if (!canshadow(lv)) {
		werrstr("shadow close");
		goto x;
	}
	if (Bnextcmd(br, nil, cbi, -1) < 0) {
		goto x;
	}
	switch (cbi->cmd->index) {
	case SHSdata:
		if (recvdata(lv, br, strtoll(cbi->cb->f[1], nil, 10),
			     atoi(cbi->cb->f[2])) < 0)
			goto x;

		if ((++datacnt % 64) == 0) {
			rlock(lv);
			savemeta(lv, 0);
			runlock(lv);
		}
		goto data;
	case SHSsnap:
		rlv = recvsnap(lv,
			       atoi(cbi->cb->f[1]),
			       cbi->cb->f[2],
			       atoi(cbi->cb->f[3]),
			       strtoul(cbi->cb->f[4], nil, 10));
		if (!rlv)
			goto x;

		xsyslog("LV %s: shadowrecv copied %uld extents\n",
			rlv->name, rlv->dirty - rlv->frstdat);
		goto copy;
	default:
		werrstr("unexpected command %s", cbi->cb->f[0]);
		break;
	}
x:
	xsyslog("LV %s: shadowrecv error: %r\n", lv->name);
	fprint(lv->fd, "END %r\n");
y:
	Bterm(br);
	free(cbi->cb);
	wlock(lv);
	lv->pid = 0;
	lv->fd = 0;
	free(lv->b);
	lv->b = 0;
	savemeta(lv, 0);
	wunlock(lv);
}

static jmp_buf shadowrecvjmp;

void
shadowrecvproc(void)
{
	int n;

	if ((n = setjmp(shadowrecvjmp))) {
		recvproc(n);
		xlexits(nil);
	}
	shadowserver(ldial, certfile);
	xlexits(nil);
}

void
shadowrecvgo(LV *lv)
{
	free(u->name);
	u->name = smprint("%s", lv->name);
	longjmp(shadowrecvjmp, 1);
}

void
shadowdebug(LV *lv, char *arg)
{
	if (isshadow(lv)) {
		wlock(lv);
		if (strcmp(arg, "on") == 0)
			lv->flags |= LVFshdebug;
		else if (strcmp(arg, "server") == 0)
			shadowserverdebug = 1;
		else {
			lv->flags &= ~LVFshdebug;
			shadowserverdebug = 0;
		}
		wunlock(lv);
	}
}

int
isxlate(void)
{
	return 1;
}

int
stopshadow(LV *lv)		/* called with lv wlocked */
{
	int i, pid;

	if (lv->fd == 0)
		return 0;

	lv->flags |= LVFshstop;
	pid = lv->pid;
	if (pid && pid != u->pid) {
		for (i = 0; i < 10; i++) {
			postnote(PNPROC, pid, shadowclose);
			if (lv->fd == 0) {
				return 0;
			}
			wunlock(lv);
			sleep(1000);
			wlock(lv);
		}
		return -1;
	}
	return 0;
}

int
reporter(int repfd, char *fmt, ...)
{
	va_list ap;

	if (repfd > 0) {
		va_start(ap, fmt);
		vfprint(repfd, fmt, ap);
		va_end(ap);
	}
	return 0;
}

int
reperrstr(int repfd, char *fmt, ...)
{
	va_list arg;
	char buf[ERRMAX], *e;

	va_start(arg, fmt);
	e = vseprint(buf, buf+ERRMAX, fmt, arg);
	va_end(arg);
	if (repfd > 0)
		write(repfd, buf, e - buf);
	errstr(buf, ERRMAX);
	return 0;
}

int
isshadow(LV *lv)
{
	return lv->rmtname != nil;
}

int
isshadowrecv(LV *lv)
{
	return isshadow(lv) && ((lv->mode & LVSEND) == 0);
}

int
isshadowsend(LV *lv)
{
	return isshadow(lv) && (lv->mode & LVSEND);
}

static Cmdtab shstab[] =
{
	SHSsend,	"SEND",		3,
	SHSver,		"VER",		2,
	SHSsnap,	"SNAP",		5,
	SHSsize,	"SIZE",		2,
	SHScopy,	"COPY",		3,
	SHSdata,	"DATA",		3,
	SHSend,		"END",		0,
};

static int shsntab = nelem(shstab);

void
cbiinit(Cmdbufidx *cbi)
{
	cbi->tab = shstab;
	cbi->ntab = shsntab;
	cbi->cb = nil;
	cbi->dbfd = -1;
	cbi->tsecs = 0;
}

int
Brdcmd(Biobufhdr *br, Cmdbufidx *cbi)
{
	char *line;
	long old;

	if (cbi->tsecs)
		old = cbi->tsecs * 1000;
	else
		old = 30 * 1000;
	old = alarmset(old);
	if (!(line = Brdline(br, '\n'))) {
		alarmclr(old);
		werrstr("connection lost %r");
		return -1;
	}
	alarmclr(old);

	if (cbi->dbfd >= 0) {
		line[Blinelen(br) - 1] = '\0';
		fprint(cbi->dbfd, "%s line: %s\n", argv0, line);
		line[Blinelen(br) - 1] = '\n';
	}
	free( cbi->cb);
	if (!(cbi->cb = parsecmd(line, Blinelen(br)))) {
		werrstr("parsecmd failed");
		return -1;
	}
	if (!(cbi->cmd = lookupcmd(cbi->cb, cbi->tab, cbi->ntab))) {
		if (0) {
			fprint(2, "%s lookup %p %d failed: %r\n",
			       argv0, cbi->tab, cbi->ntab);
		}
		return -1;
	}
	return 0;
}

int
Bnextcmd(Biobufhdr *br, Biobufhdr *bw, Cmdbufidx *cbi, int cmdindex)
{
	char *c, *cc, **f;

	if (bw)
		Bflush(bw);

	if (Brdcmd(br, cbi) < 0)
		return -1;

	if (cmdindex >= 0 && cbi->cmd->index != cmdindex) {
		if (cbi->cmd->index == SHSend) {
			f = cbi->cb->f + 1;
			c = strdup("");
		} else {
			f = cbi->cb->f;
			c = strdup(" unexpected command");
		}
		for (; *f; ++f, free(cc))
			c = smprint("%s %s", cc = c, *f);
		werrstr("%s", c + 1);
		free(c);
		return -1;
	}
	return 0;
}

void
tlsconnfree(TLSconn *tlsc)
{
	if (tlsc) {
		free(tlsc->cert);
		free(tlsc->sessionID);
		free(tlsc);
	}
}
