// Copyright © 2010 Coraid, Inc.
// All rights reserved.

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

int fflag;

void
usage(void) 
{
	fprint(2,"usage: %s RNAME source_LV [...] to LV [...]\n", argv0);
	exits("usage");
}

static int
shadowrecv(char *lv, char *rname, char *source)
{
	if (islv(lv) == 0) {
		werrstr("%s is not an LV", lv);
		return -1;
	}
	if (lvctlwrite(lv, "shadowrecv %s %s %d", rname, source, fflag) < 0)
		return -1;
	return 0;
}

void
main(int argc, char **argv) 
{
	int i, to;

	ARGBEGIN {
	case 'f':
		fflag = 1;
		break;
	default:
		usage();
	} ARGEND
	if (argc < 4)
		usage();
	if ((to = serieserrs(argc, argv, Yesto|SameOK, 1)) < 0)
		errfatal("%r");
	if (!to)
		errfatal("\'to\' not found");
	else if (argc == 4) {
		if (shadowrecv(argv[3], argv[0], argv[1]) < 0)
			errfatal("%r");
	} else {
		for (i = 1; i < to; i++) {
			if (shadowrecv(argv[i+to], argv[0], argv[i]) < 0)
				errskip(to - i, argv + i);
		}
	}
	exits(nil);
}
