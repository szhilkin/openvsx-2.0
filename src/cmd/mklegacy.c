// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Create a logical volume from a legacy EM mirror

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s LV pool target\n", argv0);
	exits("usage");
}

void
mklegacy(char *name, char *pool, char *targ)
{
	if (strchr(name, ' '))
		errfatal("LV name can only be one word");

	if (ispool(pool) == 0)
		errfatal("%s is not a pool", pool);

	if (poolctlwrite(pool, "addleg %s %s", name, targ) < 0)
		errfatal("%r");
}

void
main(int argc, char **argv) 
{
	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc != 3)
		usage();
	mklegacy(argv[0], argv[1], argv[2]);
	exits(nil);
}
