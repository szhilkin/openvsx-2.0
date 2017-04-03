// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Remve physical volumes from a pool

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h> 

void
usage(void)
{
	fprint(2, "usage: %s PV [...]\n", argv0);
	exits("usage");
}

int
rmpv(char *pv)
{
	char *pool;

	if ((pool = getpvpool(pv)) == nil) {
		werrstr("%s is not a PV", pv);
		return -1;
	}
	if (askres == RespondOne) {
		print("\'n\' to cancel, \'a\' for all, or \'y\' to rmpv %s ", pv);
		print("from %s [n]: ", pool);
		askres = askresponse();
	}

	if (poolctlwrite(pool, "rmpv %s", pv) < 0)
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
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc == 0)
		usage();
	askhdr(argc, argv);
	while(argc-- > 0)
		if (rmpv(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
