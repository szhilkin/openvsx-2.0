// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Reset an LV's serial number

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>



void
usage(void)
{
	fprint(2, "usage: %s LV [ ... ]\n", argv0);
	exits("usage");
}

static void
printserial(char *lv) 
{
	char buf[1024];
	static int phdr;

	if (readfile(buf, 1024, "/n/xlate/lv/%s/serial", lv) < 0)
		errfatal("%r");
	
	if (phdr == 0) {
		phdr++;
		print("%-16s %20s\n", "LV", "Serial Number");	
	}
	print("%-16s %20s\n", lv, buf);
}

static void
setserial(char *lv) 
{
	char buf[1024];

	snprint(buf, 1024, "/n/xlate/lv/%s/ctl", lv);
	if ((writefile(buf, "resetserial")) == -1)
		errfatal("Unknown LV %s",lv);

	printserial(lv);
}

void
main(int argc, char **argv)
{

	ARGBEGIN {	
	default:
		usage();
	} ARGEND

	if (argc > 0)
		while (argc-- > 0)
			setserial(*argv++);
	else
		usage();
	exits(nil);
}
