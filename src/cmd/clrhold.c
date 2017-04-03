// Copyright © 2011 Coraid, Inc.
// All rights reserved.

#include <u.h>
#include <libc.h>
#include "vsxcmds.h"

void
usage(void)
{
	fprint(2,"usage: %s LV.snap [...]\n", argv0);
	exits("usage");
}

int
clrhold(char *lv)
{
	Lvs *lvs;

	lvs = getlvstatus(lv);
	if (!lvs) {
		return -1;
	}
	if (!lvs->issnap) {
		free(lvs);
		werrstr("%s is not a snapshot", lv);
		return -1;
	}
	free(lvs);
	if (lvctlwrite(lv, "clrhold") < 0)
		return -1;
	return 0;
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
	if (argc == 0)
		usage();
	while (argc-- > 0)
		if (clrhold(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
