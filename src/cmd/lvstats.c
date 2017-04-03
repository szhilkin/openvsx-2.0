// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Print logical volume I/O statistics

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

char *secs ="4";

void
usage(void)
{
	fprint(2, "usage: %s [-s seconds] [LV ...]\n", argv0);
	exits("usage");
}

static int
writeio(char *lv) 
{
	char buf[1024];

	snprint(buf, 1024, "/n/xlate/lv/%s/iostats", lv);
	return writefile(buf, "%s", secs);
}

static void
printstats(char *lv, Ios *r, Ios *w) 
{	
	char *fmt = "%-17s %10s %6lldms %6lldms %10s %6lldms %6lldms\n";
	char rbuf[16], wbuf[16];
	static int phdr;

	if (phdr == 0) {
		print("%-17s %10s %8s %8s %10s %8s %8s\n", "LV", "READ", "AVG", "MAX",
			"WRITE", "AVG", "MAX");
		phdr++;
	}
	
	print(fmt, lv, mbstr(rbuf, 16, r->bytes), r->lavg, r->lmax,
		mbstr(wbuf, 16, w->bytes), w->lavg, w->lmax);
}

static void
getstats(char *lv) 
{
	char buf[1024], *args[16];
	Ios r, w;
	Lvs *l;

	l = getlvstatus(lv);
	if (l == nil) 
		errfatal("%r");
	
	if (l->issnap) {
		free(l);
		return;
	}
	free(l);
	if (secs && writeio(lv) <= 0) 
		errfatal("%r");
	if (readfile(buf, 1024, "/n/xlate/lv/%s/iostats", lv) < 0) {
		werrstr("%s has no iostats", lv);
		return;
	}
	memset(&r, 0, sizeof(Ios));
	memset(&w, 0, sizeof(Ios));

	/* Immediately after the VSX creates a lun it may not have enough stats. That is OK */
	if (getfields (buf, args, 16, 1, "\t\r\n ") == 2) {
		parseiostats(&r, args[0]);
		parseiostats(&w, args[1]);
	}
	printstats(lv, &r, &w);	
}

static void
getallstats(void) 
{
	int i, n;
	Dir *dp;

	n = numfiles("/n/xlate/lv", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirlvcmp);
	for (i = 0; i < n; i++)
		getstats(dp[i].name);
	free(dp);
}

void
main(int argc, char **argv)
{

	ARGBEGIN {
	case 's':
		secs = ARGF();
		break;	
	default:
		usage();
	} ARGEND

	if (argc > 0)
		while (argc-- > 0)
			getstats(*argv++);
	else
		getallstats();
	exits(nil);
}
