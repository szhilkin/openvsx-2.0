#include <u.h>
#include <libc.h>
#include <bio.h>
#include <libsec.h>
#include "dat.h"
#include "fns.h"
#include "shadow.h"

static char *connfail = "LV %s shadowrecvhs connection failure";

void
shadowrecvhs(int fd)
{
	Biobuf brd, bwr, *br, *bw;
	Cmdbufidx cbidx, *cbi;
	LV *lv;
	LVL *sl, *s;
	int ret;
	char *dfile;
	char stime[32];

	br = &brd;
	bw = &bwr;
	cbi = &cbidx;
	cbiinit(cbi);
	sl = nil;

	if (Binit(br, fd, OREAD) == Beof) {
		werrstr("error #1");
		return;
	}
	if (Binit(bw, fd, OWRITE) == Beof) {
		Bterm(br);
		werrstr("error #2");
		return;
	}
	if (Bnextcmd(br, nil, cbi, SHSver) < 0) {
		goto x;
	}
	if (Bnextcmd(br, nil, cbi, SHSsend) < 0) {
		goto x;
	}
	rlock(&lk);
	lv = lookuplv(cbi->cb->f[2]);
	runlock(&lk);

	if (!lv) {
		Bprint(bw, "END target LV %s not found\n", cbi->cb->f[2]);
		werrstr("target LV %s not found", cbi->cb->f[2]);
		goto x;
	}
	if (!isshadowrecv(lv)) {
		Bprint(bw, "END target LV %s is not for shadowrecv\n",
		       cbi->cb->f[2]);
		werrstr("target LV %s is not for shadowrecv",
			cbi->cb->f[2]);
		goto x;
	}
	if (lv->flags & LVFshstop) {
		Bprint(bw, "END target LV %s has stopped shadow\n",
		       cbi->cb->f[2]);
		werrstr("target LV %s has stopped shadow",
			cbi->cb->f[2]);
		goto x;
	}
	if (strcmp(cbi->cb->f[1], lv->rmtlv)) {
		Bprint(bw, "END configuration mismatch %s->%s %s->%s\n",
		       cbi->cb->f[1], cbi->cb->f[2], lv->rmtlv, lv->name);
		werrstr("configuration mismatch %s->%s %s->%s",
		       cbi->cb->f[1], cbi->cb->f[2], lv->rmtlv, lv->name);
		goto x;
	}
	if (lv->pid) {
		Bprint(bw, "END old shadowrecv process still running\n");
		werrstr("old shadowrecv process still running");
		goto x;
	}
	if (lv->flags & LVFshdebug) {
		dfile = smprint("/tmp/shadowrecvhs.%s", lv->rmtname);
		cbi->dbfd = create(dfile, OWRITE|OTRUNC, 0644);
		free(dfile);
	}
	if (Bprint(bw, "VER 1\n") == Beof) {
		werrstr(connfail, lv->name);
		goto x;
	}
	if (Bprint(bw, "SEND %s %s\n", lv->rmtlv, lv->name) == Beof) {
		werrstr(connfail, lv->name);
		goto x;
	}
	if (Bprint(bw, "SIZE %lld\n", lv->length) == Beof) {
		werrstr(connfail, lv->name);
		goto x;
	}
	rlock(&lk);
	ret = snaplist(lv, &sl, nil);
	runlock(&lk);

	if (ret < 0) {
		goto x;
	}
	for (s = sl; s; s = s->next) {
		schedtostimestr(s->l->sched, stime, sizeof stime);
		if (Bprint(bw, "SNAP %d %s %uld %uld\n",
			   s->l->sched[0].class, stime,
			   s->l->snaps, s->l->ctime) == Beof) {
			werrstr(connfail, lv->name);
			goto x;
		}
	}
	if (Bprint(bw, "COPY %d %uld\n", lv->copyclass, lv->copysnap) == Beof) {
 		werrstr(connfail, lv->name);
		goto x;
	}
	if (Bprint(bw, "DATA %lld 0\n", lv->lastoffset) == Beof) {
 		werrstr(connfail, lv->name);
		goto x;
	}
	wlock(lv);
	lv->fd = fd;
	lv->pid = getpid();
	wunlock(lv);
	goto y;
x:
	lv = 0;
y:
	Bterm(br);
	Bterm(bw);
	free(cbi->cb);
	if (cbi->dbfd >= 0)
		close(cbi->dbfd);
	freelvl(sl);
	if (lv)
		shadowrecvgo(lv);
}
