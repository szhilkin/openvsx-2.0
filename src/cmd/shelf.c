// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Set the base shelf address

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

enum {
	Maxbuf = 100
};
int shelf;

void
usage(void) 
{
	fprint(2,"usage: %s [ 0 - 65504 ]\n", argv0);
	exits("usage");
}

void
shelfresponse(void)
{
	char buf[100];
	int n;

	n = read(0, buf, sizeof buf);
	buf[n-1] = 0;
	if (cistrncmp(buf, "y", 1) == 0)
		return;
	else
		sysfatal("action canceled");
	return;
}

void
setshelf(char *p) 
{
	char buf[10];
	int n, i, newshelf;
	Dir *dp;
	Lun *l;

	n = numfiles("/n/xlate/lun", &dp);
	if (n < 0)
		errfatal("%r");

	if (strcmp(p, "unset") == 0) {
		newshelf = -1;
		if (n > 0)
			errfatal("must remove all luns before shelf unset");
	} else {
		newshelf = strtol(p, &p, 0);
		if (*p != 0)
			errfatal("bad shelf value, use [0 - 65504]");
	}

	if (n > 0 && bshelf >= 0 && askres != RespondAll) {
		lvmaxlen();
		qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirintcmp);
		print("Request to change base shelf from %d to %d. Affecting %d lun%s\n",
			bshelf, newshelf, n, n > 1 ? "s" : "");
		print("\t%-*s %9s %9s\n", lvlen, "LV", "OLD LUN", "NEW LUN");
		for (i = 0; i < n; i++) {
			snprint(buf, sizeof buf, "%,T", atoi(dp[i].name) + (bshelf << 8));
			l = getlunstatus(buf);
			if (l == nil) {
				print("Unable to get status for %s\n",buf);
				continue;
			}
			print("\t%-*s %T %T\n", lvlen, l->lv, l->lun, 
				l->loffset + (newshelf << 8));
			freelun(l);
		}
		print("\'n\' to cancel, or \'y\' to change all [n]: ");
		shelfresponse();
	}
	free(dp);
	if (writefile("/n/xlate/targ/shelf", "%d", newshelf) < 0)
		errfatal("%r");
}

void
pntshelf(void)
{
	print("SHELF\n");
	if (bshelf < 0)
		print("unset\n");
	else
		print("%d\n", bshelf);
}	

void
main (int argc, char **argv) 
{	
	ARGBEGIN {
	case 'f':
		askres = RespondAll;
		break;
	default:
		usage();
	} ARGEND
	fmtinstall('T', Tfmt);
	getshelf();
	switch (argc) {
	case 1:
		setshelf(argv[0]);
		break;
	case 0:
		pntshelf();
		break;
	default:
		usage();
	}
	exits(nil);
}
