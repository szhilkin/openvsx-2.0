// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Remve physical volumes from a pool

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h> 

int fflag; // Also ignore write error

void
usage(void)
{
	fprint(2, "usage: %s PV [...]\n", argv0);
	exits("usage");
}

int
clrpvmeta(char *pv)
{
	ask(pv);

	// We can't check askres != RespondAll as askres
	// is set when the user selects 'a' for all. We need
	// a separate variable.
	if (ctlwrite("clrpvmeta %s", pv) < 0 && fflag == 0)
		return -1;
	return 0;
}

void
main(int argc, char **argv)
{
	ARGBEGIN {
	case 'f':
		askres = RespondAll;
		fflag++;
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
		if (clrpvmeta(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
