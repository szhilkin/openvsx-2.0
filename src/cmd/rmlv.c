// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Remove a logical volume

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>


void
usage(void) 
{
	fprint(2,"usage: %s LV [...]\n", argv0);
	exits("usage");
}

int
rmlv(char *lv) 
{
	Lvs *lvs;

	lvs = getlvstatus(lv);
	if (lvs == nil) {
		werrstr("%s is not an LV", lv);
		return -1;
	}	
	ask(lv);

	if (poolctlwrite(lvs->pool, "rmlv %s", lv) < 0)
		return -1;
	freelvs(lvs);
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
		if (rmlv(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
