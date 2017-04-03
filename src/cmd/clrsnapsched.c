// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Clear snapshot schedule

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

void
usage(void) 
{
	fprint(2, "usage: %s class time LV [LV ...]\n", argv0);
	exits("usage");
}	

void
main(int argc, char **argv) 
{
	int i;
	char buf[256];

	ARGBEGIN {
	case 'f':
		askres = RespondAll;
		break;
	default:
		usage();
	} ARGEND

	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc < 3) 
		usage();
	askhdr(argc - 2, argv + 2);
	for (i = 2; i < argc; i++)  {
		snprint(buf, sizeof(buf), "%s %s from %s", argv[0], argv[1], argv[i]);
		ask(buf);
		if (lvctlwrite(argv[i], "clrsnapsched %s %s", argv[0], argv[1]) <= 0) {
			werrstr("%s: %r\n", argv[i]);
			errskip(argc - i, argv + i);
		}
	}
	exits(nil);
}
