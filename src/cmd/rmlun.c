// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Dissociate a shelf.slot with a lv

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
rmlun(char *lun)
{
	Lun *l;

	l = getlunstatus(lun);
	if (l == nil)
		return -1;

	if (lvctlwrite(l->lv, "rmlun") < 0)
		return -1;
	freelun(l);
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
	fmtinstall('T', Tfmt);
	getshelf();
	if (argc == 0)
		usage();
	while (argc-- > 0)
		if (rmlun(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
