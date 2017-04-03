// Copyright © 2010 Coraid, Inc.
// All rights reserved.

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s RNAME LV [...] to target_LV [...]\n", argv0);
	exits("usage");
}

static int
shadowsend(char *lv, char *rname, char *target)
{
	if (islv(lv) == 0) {
		werrstr("%s is not an LV", lv);
		return -1;
	}
	if (lvctlwrite(lv, "shadowsend %s %s", rname, target) < 0)
		return -1;
	return 0;
}

void
main(int argc, char **argv) 
{
	int i, to;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc < 4)
		usage();
	if ((to = serieserrs(argc, argv, Yesto|SameOK, 1)) < 0)
		errfatal("%r");
	if (!to)
		errfatal("\'to\' not found");
	else if (argc == 4) {
		if (shadowsend(argv[1], argv[0], argv[3]) < 0)
			errfatal("%r");
	} else {
		for (i = 1; i < to; i++) {
			if (shadowsend(argv[i], argv[0], argv[i+to]) < 0)
				errskip(to - i, argv + i);
		}
	}
	exits(nil);
}
