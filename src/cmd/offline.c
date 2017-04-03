// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Offline a shelf.slot so it is not visible on the network

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s LUN [...]\n", argv0);
	exits("usage");
}

int
offline(char *lun)
{
	Lun *l;

	l = getlunstatus(lun);
	if (l == nil)
		return -1;

	if (lunctlwrite(l->loffset, "offline") < 0)
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
	getshelf();
	if (argc == 0)
		usage();
	while (argc-- > 0)
		if (offline(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
