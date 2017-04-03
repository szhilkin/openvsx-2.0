// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// broadcast an aoe discover

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s\n", argv0);
	exits("usage");
}	

void
main (int argc, char **argv) 
{	
	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc)
		usage();
	if (writefile("/n/xlate/targ/ctl", "discover") < 0)
		errfatal("%r");
	exits(nil);
}
