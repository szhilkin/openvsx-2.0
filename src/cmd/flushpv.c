// Copyright © 2012 Coraid, Inc.
// All rights reserved.
// Force write of lpv metadata and config

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s PV [...]\n", argv0);
	exits("usage");
}

int
flush(char *pv) 
{
	char *pool;

	if ((pool = getpvpool(pv)) == nil) {
		werrstr("%s is not a PV", pv);
		return -1;
	}
	if (poolctlwrite(pool, "flush %s", pv) <= 0) {
		return -1;
	}
	free(pool);
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
		if (flush(*argv++))
			errskip(argc, argv);
	exits(nil);
}
