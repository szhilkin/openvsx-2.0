// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Make a lun read only

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s LV [...]\n", argv0);
	exits("usage");
}

int
readonly(char *lv)
{
	if (islv(lv) == 0) {
		werrstr("%s is not an LV", lv);
		return -1;
	}
	if (lvctlwrite(lv, "readonly") < 0)
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
		if (readonly(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
