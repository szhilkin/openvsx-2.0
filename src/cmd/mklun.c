// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// convert an lv to shelf.slot

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s LV [... to] LUN [...]\n", argv0);
	exits("usage");
}

int
mklun(char *lv, char *ss) 
{
	if (islv(lv) == 0) {
		werrstr("%s is not an LV", lv);
		return -1;
	}
	if (lvctlwrite(lv, "mklun %s", ss) < 0)
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
	if ((to = serieserr(argc, argv, Yesto)) < 0)
		errfatal("%r");
	if (to) {
		for (i = 0; i < to; i++) {
			if (mklun(argv[i], argv[i+to+1]) < 0)
				errskip(to - i, argv + i);
		}
	} else if (argc == 2) {
		if (mklun(argv[0], argv[1]) < 0)
			errfatal("%r");
	} else
		usage();
	exits(nil);
}
