// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Set snapshot limits

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

void
usage(void) 
{
	fprint(2, "usage: %s [ size[T,G,M,K] | ignore ] LV [ LV ... ]\n", argv0);
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
	if (argc < 2) 
		usage();
	askhdr(argc -1, argv + 1);
	for (i = 1; i < argc; i++)  {
		snprint(buf, sizeof(buf), "to %s on %s", argv[0], argv[i]);
		ask(buf);
		if (lvctlwrite(argv[i], "snaplimit %s", argv[0]) <= 0) {
			werrstr("%s: %r\n", argv[i]);
			errskip(argc - i, argv + i);
		}
	}
	exits(nil);
}
