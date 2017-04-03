// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Snapshot a volume

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
snap(char *lv) 
{
	char buf[128];

	if (islv(lv) == 0) {
		werrstr("%s is not an LV", lv);
		return -1;
	}
	if (readfile(buf, 128, "/n/xlate/lv/%s/snap", lv) < 0) {
		werrstr("LV %s: %r", lv);
		return -1;
	}
	return 0;
}

void
main (int argc, char **argv) 
{	
	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc == 0)
		usage();
	while(argc-- > 0)
		if (snap(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
