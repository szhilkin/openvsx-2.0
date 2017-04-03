// Copyright © 2012 Coraid, Inc.
// All rights reserved.
// Set Pool allocation mode

#include <u.h>
#include <libc.h>
#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s [striped | concat] pool [...]\n", argv0);
	exits("usage");
}

int
setmode(char *mode, char *pool) 
{
	if (ispool(pool) == 0) {
		werrstr("%s is not a pool", pool);
		return -1;
	}
	if (poolctlwrite(pool, "mode %s", mode) <= 0)
		return -1;
	return 0;
}

void
main(int argc, char **argv) 
{
	int i;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc < 2)
		usage();
	for (i = 1; i < argc; i++)
		if (setmode(argv[0], argv[i]))
			errskip(argc - i, argv + i);
	exits(nil);
}
