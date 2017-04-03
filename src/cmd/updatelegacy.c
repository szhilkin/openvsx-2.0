// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Update legacy PV format

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
getresponse(void)
{
	char buf[2];
	long n;

	memset(buf, 0, sizeof buf);
	n = read(0, buf, sizeof buf);
	if (n <= 0) 
		sysfatal("action canceled");
	buf[n - 1] = 0;
	if (cistrncmp(buf, "y", 1) == 0)
		return;
	else
		sysfatal("action canceled");
	return;
}

void updatelegacy(void)
{
	print("Request to update legacy volumes.\n");
	print("During the update all online luns will temporarily go offline.\n");
	print("\'n\' to cancel, or \'y\' to continue [n]: ");
	getresponse();
	if (ctlwrite("updatelegacy") < 0)
		errfatal("%r");
}

void
main(int argc, char **argv) 
{
	ARGBEGIN {
	default:
		usage();
	} ARGEND

	updatelegacy();
	exits(nil);
}
