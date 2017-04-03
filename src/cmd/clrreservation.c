// Copyright © 2012 Coraid, Inc.
// All rights reserved.
// Clear SCSI Reservations on an LV

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
clr(char *lv) 
{
	if (islv(lv) == 0) {
		werrstr("%s is not an LV", lv);
		return -1;
	}
	if (lvctlwrite(lv, "clrreservation") < 0) {
		werrstr("LV %s: %r", lv);
		return -1;
	}
	return 0;
}

void
main (int argc, char **argv) 
{	
	char buf[256];

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc == 0)
		usage();
	askhdr(argc, argv);
	while(argc-- > 0) {
		snprint(buf, sizeof(buf), "on %s", *argv);
		ask(buf);
		if (clr(*argv) < 0)
			errskip(argc, ++argv);
		argv++;
	}
	exits(nil);
}
