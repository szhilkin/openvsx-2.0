// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Create a deep copy of lv-name and send it to shelf.slot

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s LV target", argv0);
	exits("usage");
}

void
copy(char *lv, char *ss) 
{
	if (islv(lv) == 0)
		errfatal("%s is not an LV", lv);

sysfatal("not yet implemented");
	if (lvctlwrite(lv, "copy %s %s", lv, ss) < 0)
		errfatal("%r");
}

void
main (int argc, char **argv) 
{	
	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc != 2)
		usage();
	copy(argv[0], argv[1]);
	exits(nil);
}
