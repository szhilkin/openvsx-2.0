// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Remove a pool

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void)
{
	fprint(2, "usage: %s pool [...]\n", argv0);
	exits("usage");
}

int
rmpool(char *p) 
{
	return ctlwrite("rmpool %s", p);
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
		if (rmpool(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
