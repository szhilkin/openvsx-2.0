// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Enable | Disable longer I/O timeouts while performing SR(X) maintenance

#include <u.h>
#include <libc.h>

#include <vsxcmds.h>

static char Mmconf[] = "/n/sys/config/maintenancemode";

void
usage(void) 
{
	fprint(2,"usage: %s [ enable | disable ]\n", argv0);
	exits("usage");
}

void 
getmm(void)
{
	char buf[16];

	print("MAINTENANCE MODE\n");
	if (readfile(buf, sizeof buf, Mmconf) < 0)
		errfatal("%r");
	else {
		buf[15] = '\0';
		if (strcmp(buf, "enable") == 0)
			print("enabled\n");
		else
			print("disabled\n");
	}
}

void 
setmm(char *mmode)
{
	if (writefile(Mmconf, mmode) < 0)
		errfatal("%r");
}

void
main(int argc, char **argv) 
{
	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc > 1)
		usage();
	if (argc)
		setmm(argv[0]);
	else
		getmm();
	exits(nil);
}
