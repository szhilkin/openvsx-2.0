// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Set snapshot schedule

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

void
usage(void) 
{
	fprint(2, "usage: %s class time retain LV [...]\n", argv0);
	exits("usage");
}	

void
main(int argc, char **argv) 
{
	int i;

	ARGBEGIN {
	case 'f':
		askres = RespondAll;
		break;
	default:
		usage();
	} ARGEND

	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc < 4) 
		usage();
	for (i = 3; i < argc; i++)  {
		if (lvctlwrite(argv[i], "snapsched %s %s %s", argv[0], argv[1], argv[2]) <= 0) {
			werrstr("%s: %r\n", argv[i]);
			errskip(argc - i, argv + i);
		}
	}
	exits(nil);
}
