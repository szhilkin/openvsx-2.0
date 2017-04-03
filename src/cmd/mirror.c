// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Mirror a physical volume

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

int cflag;


void
usage(void) 
{
	fprint(2,"usage: %s PV [... to] target [...]\n", argv0);
	exits("usage");
}

int
mirror(char *pv, char *targ) 
{
	char *pool;
	char *clean;
	pool = getpvpool(pv);
	if (pool == nil) {
		werrstr("%s is not a PV", pv);
		return -1;
	}
	clean = cflag ? "clean" : "";

	if (poolctlwrite(pool, "%smirror %s %s", clean, pv, targ) < 0)
		return -1;
	free(pool);
	return 0;
}

void
main(int argc, char **argv) 
{
	int i, to;

	ARGBEGIN {
	case 'c':
		cflag++;
		break;
	default:
		usage();
	} ARGEND
	if ((to = serieserr(argc, argv, Yesto)) < 0)
		errfatal("%r");
	if (argc < 2)
		usage();
	else if (argc == 2) {
		if (mirror(argv[0], argv[1]) < 0)
			errfatal("%r");
	} else {
		if (to == 0)
			errfatal("\'to\' not found");
		for (i = 0; i < to; i++)
			if (mirror(argv[i], argv[i+to+1]) < 0)
				errskip(to - i, argv + i);
	}
	exits(nil);
}
