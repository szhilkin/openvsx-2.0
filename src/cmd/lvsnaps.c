// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// List snaps

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

int phdr;
int aflag;
int sflag;

typedef struct LVL LVL;
struct LVL {
	LVL *next;
	char *snap;
	
};

typedef struct Sched Sched;
struct Sched {
	LVL *lvl;
	int s[7];
};

/* Should be large enough, 4k luns + 32 schedules */
/* sched will hold all the schedules for an LV, individual
snapshots will be chain off sched entries */
Sched sched[4096+32];
int nsched; /* Elements used */

void
usage(void) 
{
	fprint(2, "usage: %s [ -s -a ] [LV] [LV.snap] [...]\n", argv0);
	exits("usage");
}

void
addlv(Sched *s, char *snap)
{
	LVL *lvl;

	if (s->lvl == nil) {
		s->lvl = mustmalloc(sizeof(LVL));
		s->lvl->snap = snap;
	} else {
		lvl = s->lvl;
		while (lvl->next)
			lvl = lvl->next;
		lvl->next = mustmalloc(sizeof(LVL));
		lvl->next->snap = snap;
	}
	
}

/* Update sched array, don't print anything */
void
schedlv(char *snap)
{
	int time[7], i;

	memset(time, 0, sizeof time);
	getsnapsched(snap, 7, time);
	for (i = 0; i < nsched; i++) {
		if (memcmp(sched[i].s, time, sizeof(time)) == 0) {
			addlv(&sched[i], snap);
			return;
		}
	}
	/* Nothing matched, add a new sched entry */
	memcpy(sched[i].s, time, sizeof(time));
	addlv(&sched[i], snap);
	nsched++;
}

static int
schedcmp(Sched *a, Sched *b)
{
	int i;

	for (i = 0; i < 7; i++) {
		if (i == 1) //Don't compare retain count
			continue;
		if (a->s[i] != b->s[i])
			return a->s[i] - b->s[i];
	}
	return 0;
} 

void
printsnaplv(char *snap)
{
	Lvs *lvs;
	Tm *t;
	char *sched;
	int args[256];

	lvs = getlvstatus(snap);
	if (lvs == nil) {
		fprint(2, "error: %r\n");
		return;
	}

	if (phdr == 0) {
		print("%-*s %15s %10s %8s %12s %8s\n", lvlen,
		      "SNAPSHOT", "DATE", "SIZE(GB)", "CLASS",
		      "SCHEDULE", "HOLD");
		phdr++;
	}

	t = localtime(lvs->ctime);
	getsnapsched(snap, 256, args);
	sched = schedstr(args);
	print("%-*s %04d%02d%02d.%02d%02d%02d %B %8s %12s %8s\n",
	      lvlen, snap,
	      t->year+1900, t->mon+1, t->mday, t->hour, t->min, t->sec,
	      lvs->length, lvs->class, sched,
	      lvs->mode & LVNOHOLD ? "disabled" : "enabled");
	if (aflag) {
		print("%*s%s%s\n", Pahdr, "Pool", delim, lvs->pool); 
		print("%*s%s%d %,ZGB\n", 
			Pahdr, "Total Exts", delim, lvs->totalext, lvs->totalext);
		print("%*s%s%d %,ZGB\n", 
			Pahdr, "Dirty Exts", delim, lvs->dirtyext, lvs->dirtyext);
		print("%*s%s%d %,ZGB\n", 
			Pahdr, "Thin Exts", delim, lvs->thinext, lvs->thinext);
	}
	free(sched);
	free(lvs);
}

/* Dump all schedule entries */
void
dumpsched(void)
{
	int i;
	LVL *lvl, *p;
	qsort(sched, nsched, sizeof (Sched), (int (*)(void *, void *))schedcmp);
	for (i = 0; i < nsched; i++) {
		lvl = sched[i].lvl;
		while (lvl) {
			printsnaplv(lvl->snap);
			p = lvl;
			lvl = lvl->next;
			free(p);
		}
		sched[i].lvl = nil;
	}
	nsched = 0;
}

void
schedlvs(char *lv)
{
	char *p;
	int i, n, l;
	Dir *dp;

	n = numfiles("/n/xlate/lv", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirlvcmp);
	l = lv ? strlen(lv) : 0;
	for (i = 0; i < n; i++)
		if ((p = strchr(dp[i].name, '.')) == nil) {
			if (nsched != 0)
				dumpsched();
		} else {
			if (lv) {
				if (p - dp[i].name == l && strncmp(dp[i].name, lv, l) == 0) {
					schedlv(dp[i].name);
				}
			} else {
				schedlv(dp[i].name);
			}
		}
	if (nsched != 0)
		dumpsched();
	free(dp);
}

void 
printsnaplvs(char *snap)
{
	char *p;
	int i, n, l;
	Dir *dp;

	n = numfiles("/n/xlate/lv", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirlvcmp);
	l = snap ? strlen(snap) : 0;
	for (i = 0; i < n; i++)
		if (p = strchr(dp[i].name, '.'))
			if (snap) {
				if (p - dp[i].name == l && strncmp(dp[i].name, snap, l) == 0)
					printsnaplv(dp[i].name);
			} else
				printsnaplv(dp[i].name);
	free(dp);
}

void
main(int argc, char **argv) 
{
	ARGBEGIN {
	case 'a':
		aflag++;
		break;
	case 's':
		sflag++;
		break;
	default:
		usage();
	} ARGEND
	fmtinstall('Z', Zfmt);
	fmtinstall('T', Tfmt);
	fmtinstall('B', Bfmt);
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	lvmaxlen();
	if (lvlen < 8)
		lvlen = 8;
	if (sflag) {
		if (argc > 0) {
			while (argc-- > 0)
				if (strchr(*argv, '.'))
					printsnaplv(*argv++); /* Nothing to compare */
				else
					schedlvs(*argv++);
		} else
			schedlvs(nil);
	} else {
		if (argc > 0) {
			while (argc-- > 0) 
				if (strchr(*argv, '.')) 
					printsnaplv(*argv++);
				else
				printsnaplvs(*argv++);
		} else 	
			printsnaplvs(nil);
	}
	exits(nil);
}
