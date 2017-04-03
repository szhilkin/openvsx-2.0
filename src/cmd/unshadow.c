// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Create a snapshot of lv-name.The snapshot will then be copied to shelf.slot. 

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s LV [...]\n", argv0);
	exits("usage");
}

int
unshadow(char *lv) 
{
	if (islv(lv) == 0) {
		werrstr("%s is not a LV", lv);
		return -1;
	}
	ask(lv);

	if (lvctlwrite(lv, "unshadow") < 0)
		return -1;
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
	while (argc-- > 0)
		if (unshadow(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
