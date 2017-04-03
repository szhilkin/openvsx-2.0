// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// list lvs statuses

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

enum {
	Maxargs = 255,
	Maxbuf	= 8*1024
};

int phdr;

void
usage(void) 
{
	fprint(2,"usage: %s [LUN ...]\n", argv0);
	exits("usage");
}

void
printlun(char *lun)
{
	char buf[Maxbuf], *args[Maxargs];
	int n;
	Lun *l;
	Lvs *lvs;

	l = getlunstatus(lun);
	if (l == nil) {
		fprint(2, "error: %r\n");
		return;
	}
	lvs = getlvstatus(l->lv);
	if (lvs == nil) {
		fprint(2, "error: %r\n");
		freelun(l);
		return;
	}
	if (phdr == 0) {
		print("%-9s %7s %*s %10s %4s %5s\n", 
			"LUN", "STATUS", lvlen, "LV", "SIZE(GB)", "MODE", "MASKS");
		phdr++;
	}

	if (readfile(buf, Maxbuf, "/n/xlate/lv/%s/mask", l->lv) < 0)
		n = 0;
	else 
		n = tokenize(buf, args, Maxargs);

	print("%-T %7s %*s %B %4s %5d\n", 
		l->lun, 
		l->status,
		lvlen, l->lv, 
		lvs->length,
		lvs->mode & Write ? "r/w" : "r/o",
		n);
	freelvs(lvs);
	freelun(l);
}

void
printallluns(void)
{
	char buf[10];
	int i, n;
	Dir *dp;

	n = numfiles("/n/xlate/lun", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirintcmp);
	for (i = 0; i < n; i++) {
		snprint(buf, sizeof buf, "%,T", atoi(dp[i].name) + (bshelf << 8));
		printlun(buf);
	}
	free(dp);
}

void
main(int argc, char **argv) 
{
	ARGBEGIN {
	default:
		usage();
	} ARGEND
	fmtinstall('T', Tfmt);
	fmtinstall('B', Bfmt);
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");

	lvmaxlen();
	getshelf();
	if (argc == 0)
		printallluns();
	else
		while (argc-- > 0)
			printlun(*argv++);
	exits(nil);
}
