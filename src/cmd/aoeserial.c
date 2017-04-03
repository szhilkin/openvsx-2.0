#include <u.h>
#include <libc.h>
#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s target serial\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv) 
{
	int fd;

	ARGBEGIN {
	default:
		usage();
	} ARGEND

	if (isinactive())
		errfatal("%r");

	if (argc != 2)
		usage();

	fd = open(smprint("/n/xlate/targ/%s/serial", argv[0]), OWRITE);
	if (fd < 0)
		errfatal("%s is not an AoE target", argv[0]);
	if (write(fd, argv[1], strlen(argv[1])) < 0)
		errfatal("%r");

	exits(nil);
}
