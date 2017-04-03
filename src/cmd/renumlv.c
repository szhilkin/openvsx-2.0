// Copyright © 2011 Coraid, Inc.
// All rights reserved.

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

void
usage(void)
{
	fprint(2,"usage: %s LV.m [... to] LV.n [...]\n", argv0);
	exits("usage");
}

int
renumlv(char *old, char *new)
{
	char *pool;
	Lvs *lvs;

	lvs = getlvstatus(old);
	if (lvs == nil) {
		werrstr("%s is not an LV\n", old);
		return -1;
	}
	pool = lvs->pool;
	if (poolctlwrite(pool, "mvlv %s %s", old, new) < 0) {
		free(lvs);
		return -1;
	}
	free(lvs);
	return 0;
}

void
main(int argc, char **argv)
{
	int i, to;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if ((to = serieserr(argc, argv, Yesto)) < 0)
		errfatal("%r");
	if (argc < 2)
		usage();
	else if (argc == 2) {
		if (renumlv(argv[0], argv[1]) < 0)
			errfatal("%r");
	} else {
		if (to == 0)
			errfatal("\'to\' not found");
		for (i = 0; i < to; i++)
			if (renumlv(argv[i], argv[i+to+1]) < 0)
				errskip(to - i, argv + i);
	}
	exits(nil);
}
