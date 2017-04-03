// Copyright © 2012 Coraid, Inc.
// All rights reserved.
// Break the mirror of the secondary

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

void
usage(void) 
{
	fprint(2,"usage: %s PV [...]\n", argv0);
	exits("usage");
}

int
brkmirror(char *pv) 
{
	char *pool;

	pool = getpvpool(pv);
	if (pool == nil) {
		werrstr("%s is not a PV", pv);
		return -1;
	}
	ask(pv);

	if (poolctlwrite(pool, "brkmirror %s", pv) < 0)
		return -1;
	free(pool);
	return 0;
}

void
main(int argc, char **argv) 
{
	ARGBEGIN {
	case 'f':
		askres = RespondAll;
		break;
	default:
		usage();
	} ARGEND
	fmtinstall('T', Tfmt);
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc == 0)
		usage();
	askhdr(argc, argv);
	while (argc-- > 0)
		if (brkmirror(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
