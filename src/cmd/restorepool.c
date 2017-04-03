// Copyright © 2012 Coraid, Inc.
// All rights reserved.

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

static void
usage(void)
{
	fprint(2, "usage: %s name target [...]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv) 
{
	int i;
	char *args;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc < 2)
		usage();

	if (strchr(argv[0], ' ')) {
		errfatal("pool name can only be one word");
	}
	args = argv[0];
	for (i = 1; i < argc; ++i)
		if ((args = smprint("%s %s", args, argv[i])) == nil)
			errfatal("%r");

	if (ctlwrite("restorepool %s", args) < 0)
		errfatal("%r");

	exits(nil);
}
