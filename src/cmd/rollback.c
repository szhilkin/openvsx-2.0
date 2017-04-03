// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Roll back a logical volume

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

void
usage(void) 
{
	fprint(2,"usage: %s LV [LV.snap]\n", argv0);
	exits("usage");
}

void
rollback(char *lv) 
{
	Lvs *lvs;

	lvs = getlvstatus(lv);
	if (lvs == nil)
		errfatal("%s is not an LV", lv);
	
	ask(lv);

	if (lvctlwrite(lv, "rollback") < 0)
		errfatal("%r");
	freelvs(lvs);
}

void
delsnaps(char *lv, int after)
{
	Lvs *lvs;
	Dir *dp;
	char *p;
	int i, j, n;

	n = numfiles("/n/xlate/lv", &dp);
	if (n < 0)
		errfatal("%r");
	lvs = getlvstatus(lv);
	for (i = 0; i < n; ++i) {
		p = strchr(dp[i].name, '.');
		if (p) {
			*p = 0;
			if(strcmp(lv, dp[i].name) == 0) {
				j = atoi(p+1);
				if (j > after) {
					if (askres != RespondAll) {
						print("\'n\' to cancel, \'a\' for all, or \'y\' to rmlv %s.%d [n]: ", lv, j);
						askres = askresponse();
					}
					if (poolctlwrite(lvs->pool, "rmlv %s.%d", lv, j) < 0)
						errfatal("%r");
				}
			}
		}
	}
	freelvs(lvs);
}

void 
failonline(char *lv)
{
	char *b;
	Lvs *np;
	Lun *lun;

	np = getlvstatus(lv);
	if (np == nil)
		errfatal("%s is not an LV", lv);
	if (np->lun >= 0) {
		b = mustsmprint("%,T", np->lun);
		lun = getlunstatus(b);
		free(b);
		if (lun == nil) {
			free(np);
			return;
		}
		if (strcmp(lun->status, "online") == 0)
			errfatal("LUN %,T is online.  First offline the LUN.", np->lun);
		free(lun);
	}
	free(np);
}

void
main(int argc, char **argv) 
{
	char *p, *b;
	
	ARGBEGIN {
	case 'f':
		askres = RespondAll;
		break;
	default:
		usage();
	} ARGEND
	if (argc < 1 || argc > 2)
		usage();
	fmtinstall('T', Tfmt);
	failonline(argv[0]);
	if (argc == 2) {
		b = smprint("/n/xlate/lv/%s", argv[1]);
		p = strchr(argv[1], '.');
		if (access(b, AEXIST) < 0 || !p || strncmp(argv[1], argv[0], p - argv[1]) != 0)
			errfatal("%s is not a valid snapshot", argv[1]);
		if (askres != RespondAll)	
			print("Request to rollback LV %s to %s:\n", argv[0], argv[1]);
		delsnaps(argv[0], atoi(p+1));
	} else if (askres != RespondAll)	
		print("Request to rollback LV %s:\n", argv[0]);
	rollback(argv[0]);
	exits(nil);
}
