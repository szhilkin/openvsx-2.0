// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Display snapshot limits

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>
static int phdr;

static void
usage(void)
{
	fprint(2, "usage: %s  [ LV ... ]\n", argv0);
	exits("usage");
}

static void
printlv(char *lv) 
{
	char buf[1024], *e;
	vlong l;
	Lvs *lvs;

	lvs = getlvstatus(lv);
	if (lvs == nil) {
		fprint(2, "%s: %r\n",lv);
		return;
	}

	if (lvs->issnap) {
		free(lvs);
		return;
	}
	free(lvs);
	if (phdr == 0) {
		print("%-16s  %10s  %10s\n","LV", "LIMIT(GB)", "USED(GB)");
		phdr++;
	}
	if (readfile(buf, 1024, "/n/xlate/lv/%s/snaplimit", lv) < 0) {
		fprint(2, "%s: %r\n",lv);
		return;
	}
	if (strcmp(buf, "ignore") == 0 || strcmp(buf, "unset") == 0) {
		print("%-16s  %10s  %Z\n", lv, buf, lvs->snapext);
	} else {
		l = strtoll(buf, &e, 10);
		if (*e) {
			fprint(2,"%s: Bad length %s\n",lv, buf);
			return;
		}	
		print("%-16s  %B  %Z\n", lv, l, lvs->snapext);
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
	fmtinstall('B', Bfmt);
	fmtinstall('Z', Zfmt);
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc > 0)
		while (argc-- > 0)
			printlv(*argv++);
	else
		printall();
	exits(nil);
}
