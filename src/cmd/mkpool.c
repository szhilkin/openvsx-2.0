// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Create a pool

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void)
{
	fprint(2, "usage: %s name [...]\n", argv0);
	exits("usage");
}

int
mkpool(char *p)
{
	if (strchr(p, ' ')) {
		werrstr("pool name can only be one word");
		return -1;
	}
	if (ctlwrite("mkpool %s", p) < 0)
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
		if (mkpool(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
