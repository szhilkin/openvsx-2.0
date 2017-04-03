// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Display snapshot schedule

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>
static int phdr;

static void
usage(void)
{
	fprint(2, "usage: %s LV [...]\n", argv0);
	exits("usage");
}


static void
printlv(char *lv) 
{
	char buf[8192], *a, *b, *sch, retain[32];
	int k, i, plv, t[256];
	Lvs *l;

	plv = 0;
	l = getlvstatus(lv);
	if (l == nil) {
		fprint(2, "%s: %r\n",lv);
		return;
	}
	if (l->issnap) {
		free(l);
		return;
	}
	free(l);
	a = "%-16s %7s %13s %10s\n";
	b = "%24s %13s %10s\n"; 
	if (readfile(buf, 1024, "/n/xlate/lv/%s/snapsched", lv) < 0) {
		errfatal("%s: %r\n",lv);
	}
	k = getsnapsched(lv, 256, t);
	if (phdr == 0) {
		print(a,"LV", "CLASS", "TIME", "RETAIN");
		phdr++;
	}
	for (i = 0; i < k; i += 7) {
		if (t[i + 1] == -1)
			strncpy(retain, "hold", 32);
		else  
			snprint(retain, sizeof retain, "%d", t[i + 1]);
		sch = schedstr(&t[i]);
		if (plv == 0) {
			print(a, lv, clsstr(t[i]), sch, retain);
			plv++;
		} else 
			print(b, clsstr(t[i]), sch, retain);
		free(sch);
	}
}

static void
printall(void)
{
	int i, n;
	Dir *dp;

	n = numfiles("/n/xlate/lv", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirlvcmp);
	for (i = 0; i < n; i++)
		printlv(dp[i].name);
	free(dp);
}

void
main(int argc, char **argv) 
{
	ARGBEGIN {	
	default:
		usage();
	} ARGEND
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc > 0)
		while (argc-- > 0)
			printlv(*argv++);
	else
		printall();
	exits(nil);
}
