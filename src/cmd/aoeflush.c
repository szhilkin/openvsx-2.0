// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// flush stale aoe targets (targets with no paths to them)

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
	if (writefile("/n/xlate/targ/ctl", "flush") < 0)
		errfatal("%r");
	exits(nil);
}
