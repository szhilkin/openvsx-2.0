#include <u.h>
#include <libc.h>
#include <bio.h>
#include <libsec.h>
#include "dat.h"
#include "fns.h"
#include "shadow.h"

static char *connfail = "handshake connection error";

static int
nextsnaptocopy(LVL *snaps, int rmtsnap)
{
	LVL *s;

	for (s = snaps; s; s = s->next) {
		if (s->l->snaps > rmtsnap)
			return s->l->snaps;
	}
	return 0;
}

static int
checksnap(int *match, LVL *snaps, char *class, char *num, char *time)
{
	int snap, snapclass;
	ulong timestamp;
	LVL *s;

	snapclass = atoi(class);
	snap = atoi(num);
	timestamp = strtoul(time, nil, 10);

	for (s = snaps; s; s = s->next) {
		if (s->l->snaps == snap) {
			*match |= 1;
			if (s->l->sched[0].class != snapclass) {
				werrstr("snap %s class %s does not match %d",
					num, class, s->l->sched[0].class);
				return -1;
			}
			if (s->l->ctime != timestamp) {
				werrstr("snap %s timestamp %s does not match %uld",
					num, time, s->l->ctime);
				return -1;
			}
		} else if (s->l->ctime == timestamp) {
			werrstr("snap %s timestamp %s matches snap %uld",
				num, time, s->l->snaps);
			return -1;
		}
	}
	return snap;
}

static int
recvsnaps(Biobuf *br, Cmdbufidx *cbi, LVL *snaps)
{
	int snap, common;

	snap = common = 0;

	while (Bnextcmd(br, nil, cbi, -1) >= 0) {
		if (cbi->cmd->index == SHSsnap) {
			if ((snap = checksnap(&common, snaps,
					      cbi->cb->f[1],
					      cbi->cb->f[3],
					      cbi->cb->f[4])) < 0)
				break;
		} else if (cbi->cmd->index == SHScopy) {
			if (snap && !common) {
				werrstr("no common snapshot");
				break;
			}
			return snap;
		} else {
			werrstr("unexpected command %s", cbi->cb->f[0]);
			break;
		}
	}
	return -1;
}

int
shadowsendhs(LV *lv, int fd)
{
	Biobuf brd, bwr, *br, *bw;
	Cmdbufidx cbidx, *cbi;
	LVL *snaps;
	int lastsnap, nextsnap, copysnap;
	vlong size, lastoffset;
	char *dfile;

	nextsnap = -1;
	br = &brd;
	bw = &bwr;
	cbi = &cbidx;
	cbiinit(cbi);
	snaps = nil;

	if (Binit(br, fd, OREAD) == Beof) {
		werrstr("error #1");
		return -1;
	}
	if (Binit(bw, fd, OWRITE) == Beof) {
		Bterm(br);
		werrstr("error #2");
		return -1;
	}
	if (lv->flags & LVFshdebug) {
		dfile = smprint("/tmp/shadowsendhs.%s", lv->rmtname);
		cbi->dbfd = create(dfile, OWRITE|OTRUNC, 0644);
		free(dfile);
	}
	if (Bprint(bw, "VER 1\n") == Beof) {
		werrstr(connfail);
		goto x;
	}
	if (Bprint(bw, "SEND %s %s\n", lv->name, lv->rmtlv) == Beof) {
		werrstr(connfail);
		goto x;
	}
	if (Bnextcmd(br, bw, cbi, SHSver) < 0) {
		goto x;
	}
	if (strcmp(cbi->cb->f[1], "1") > 0) {
		werrstr("version mismatch");
		goto x;
	}
	if (Bnextcmd(br, nil, cbi, SHSsend) < 0) {
		goto x;
	}
	if (strcmp(cbi->cb->f[1], lv->name) != 0
	    || strcmp(cbi->cb->f[2], lv->rmtlv) != 0) {
		werrstr("configuration mismatch %s->%s %s->%s",
		       cbi->cb->f[1], cbi->cb->f[2], lv->name, lv->rmtlv);
	}
	if (Bnextcmd(br, nil, cbi, SHSsize) < 0) {
		goto x;
	}
	size = strtoll(cbi->cb->f[1], nil, 10);

	if (size < lv->length) {
		werrstr("target size %lld smaller than %lld", size, lv->length);
		goto x;
	}
	wlock(lv);
	lv->rmtlvsize = size;
	wunlock(lv);
	rlock(&lk);
	if (snaplist(lv, &snaps, nil) < 0) {
		runlock(&lk);
		goto x;
	}
	runlock(&lk);
	if ((lastsnap = recvsnaps(br, cbi, snaps)) < 0) {
		goto x;
	}
	if (cbi->cmd->index != SHScopy) {
		werrstr("no COPY received");
		goto x;
	}
	if (lastsnap > lv->snaps) {
		werrstr("remote last snap %d exceeds local %uld",
			lastsnap, lv->snaps);
		goto x;
	}
	if ((nextsnap = nextsnaptocopy(snaps, lastsnap)) < 0) {
		goto x;
	}
	copysnap = atoi(cbi->cb->f[2]);
	if (Bnextcmd(br, nil, cbi, SHSdata) < 0) {
		goto x;
	}
	if (nextsnap > 0 && copysnap > 0) {
		if (nextsnap == copysnap) {
			lastoffset = strtoll(cbi->cb->f[1], nil, 10);
			wlock(lv);
			lv->lastoffset = lastoffset;
			wunlock(lv);
		}
	}
x:
	Bterm(br);
	Bterm(bw);
	free(cbi->cb);
	freelvl(snaps);
	if (cbi->dbfd >= 0)
		close(cbi->dbfd);
	return nextsnap;
}
