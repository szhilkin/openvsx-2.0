// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Swap primary and secondary

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

void
usage(void) 
{
	fprint(2,"usage: %s mirrorTarget [...]\n", argv0);
	exits("usage");
}

int
promote(char *mirror) 
{
	char *pool;

	pool = getmirpool(mirror);
	if (pool == nil) {
		werrstr("%s is not a mirrored target", mirror);
		return -1;
	}
	if (poolctlwrite(pool, "promote %s", mirror) < 0)
		return -1;

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
	fmtinstall('T', Tfmt);
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc == 0)
		usage();
	while (argc-- > 0)
		if (promote(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
