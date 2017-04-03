// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Create a logical volume

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

int tflag;

void
usage(void) 
{
	fprint(2,"usage: %s [-t] pool size[T,G,M,K] name [...]\n", argv0);
	exits("usage");
}

int
mklv(char *pool, char *size, char *name) 
{

	if (strchr(name, ' ')) {
		werrstr("LV name can only be one word");
		return -1;
	}
	if (poolctlwrite(pool, "mklv%s %s %s", tflag ? "thin" : "", name, size) < 0)
		return -1;
	return 0;
}	

void
main(int argc, char **argv) 
{
	int i;

	ARGBEGIN {
	case 't':
		tflag++;
		break;
	default:
		usage();
	} ARGEND

	fmtinstall('B', Bfmt);
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc < 3)
		usage();
	if (ispool(argv[0]) == 0) {
		fprint(2, "error: %s is not a pool\n", argv[0]);
		usage();
	}
	for (i = 2; i < argc; i++)
		if (mklv(argv[0], argv[1], argv[i]) < 0)
			errskip(argc - i, argv + i);
	exits(nil);
}
