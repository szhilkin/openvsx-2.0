#include <u.h>
#include <libc.h>
#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s address [ { encrypt | null } certhash ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv) 
{
	ARGBEGIN {
	default:
		usage();
	} ARGEND

	if (isinactive())
		errfatal("%r");

	mountremote();

	switch (argc) {
	case 1:
		if (writefile("/n/remote/security", "%s\n", argv[0]) < 0)
			errfatal("%r");
		break;
	case 3:
		if (writefile("/n/remote/security", "%s %s %s\n",
			      argv[0], argv[1], argv[2]) < 0)
			errfatal("%r");
		break;
	default:
		usage();
	}
	exits(nil);
}
