// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Clone logical volumes

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s orig_LV new_LV [...]\n", argv0);
	exits("usage");
}

int
clone(char *orig, char *new) 
{
	if (islv(orig) == 0) {
		werrstr("%s is not an LV", orig);
		return -1;
	}
	if (lvctlwrite(orig, "clone %s", new) < 0)
		return -1;
	return 0;
}

void
main(int argc, char **argv) 
{
	int i;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc < 2)
		usage();
	for (i = 1; i < argc; i++)
		if (clone(argv[0], argv[i]) < 0)
			errskip(argc - i, argv + i);
	exits(nil);
}
