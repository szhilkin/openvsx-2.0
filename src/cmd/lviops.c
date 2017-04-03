// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Print logical volume IOPS

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

char *secs = "4";

char *Iosizestr[] = { "512", "1024", "2048", "4096", "8192", ">8192" };

void
usage(void)
{
	fprint(2, "usage: %s [-s seconds] [LV ...]\n", argv0);
	exits("usage");
}

static int
writeiops(char *lv) 
{
	char buf[1024];

	snprint(buf, 1024, "/n/xlate/lv/%s/iops", lv);
	return writefile(buf, "%s", secs);
}

static void
printiops(char *lv, Iops *r, Iops *w) 
{
	char *fmt1 = "%-17s %9s %9lld %9lld\n";
	char *fmt2 = "%27s %9lld %9lld\n";
	static int phdr;
	int i;

	if (phdr == 0) {
		print("%-17s %9s %9s %9s\n","LV", "IOPS", "READ", "WRITE");
		phdr++;
	}
	for (i = 0; i < 6; i++) 
		if (i == 0) 
			print(fmt1, lv, Iosizestr[i], r->io[i], w->io[i]);
		else
			print(fmt2, Iosizestr[i],  r->io[i], w->io[i]);
}

static void
getiops(char *lv) 
{
	Iops w, r;
	char buf[1024], *args[16];

	memset(&w, 0, sizeof(Iops));
	memset(&r, 0, sizeof(Iops));	
	if (secs && writeiops(lv) <= 0) 
		errfatal("%s is not an LV", lv);
	if (readfile(buf, 1024, "/n/xlate/lv/%s/iops", lv) < 0) {
		werrstr("%s has no iostats", lv);
		return;
	}
	/* Immediately after the VSX creates a lun it may not have enough iops. That is OK */
	if (getfields (buf, args, 16, 1, "\t\r\n ") == 2) {
		parseiops(&r, args[0]);
		parseiops(&w, args[1]);
	}
	printiops(lv, &r, &w);
}

static void
getalliops(void) 
{
	int i, n;
	Dir *dp;

	n = numfiles("/n/xlate/lv", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirlvcmp);
	for (i = 0; i < n; i++)
		getiops(dp[i].name);
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
			getiops(*argv++);
	else
		getalliops();
	exits(nil);
}
