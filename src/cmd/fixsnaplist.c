// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Add physical volumes to a pool

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

void
usage(void)
{
	fprint(2, "usage: %s\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{	
	ARGBEGIN {
	default:
		usage();
	} ARGEND
	ctlwrite("fixsnaplist");
	exits(nil);
}
