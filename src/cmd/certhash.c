#include <u.h>
#include <libc.h>
#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv) 
{
	char buf[128];
	int n;

	ARGBEGIN {
	default:
		usage();
	} ARGEND

	if ((n = readfile(buf, sizeof buf, "/n/sys/net/certhash")) < 0)
		errfatal("%r");

	write(1, buf, n);
	exits(nil);
}
