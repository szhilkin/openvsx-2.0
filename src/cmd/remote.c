#include <u.h>
#include <libc.h>
#include <bio.h>
#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s [ RNAME ] [ ... ]\n", argv0);
	exits("usage");
}

char *fmt = "%20s %15s %15s\n";

void
printit(char *name, char *ip0, char *ip1)
{
	print(fmt, name, ip0, ip1);
}

int
remote(char *name)
{
	Biobuf *b;
	char *s, *f[4];
	int n, cmp;

	if (!(b = Bopen("/n/remote/name", OREAD)))
		return -1;

	for (; s = Brdstr(b, '\n', 0); free(s)) {
		if ((n = getfields(s, f, nelem(f), 1, " \n")) < 2)
			break;
		if (name) {
			if ((cmp = strcmp(f[0], name)) > 0)
				break;
			else if (cmp == 0) {
				if (n == 2)
					f[2] = " ";
				printit(f[0], f[1], f[2]);
				break;
			}
		} else
			printit(f[0], f[1], f[2]);
	}
	free(s);
	Bterm(b);
	return 0;
}

void
main(int argc, char **argv) 
{
	int i;

	ARGBEGIN {
	default:
		usage();
	} ARGEND

	if (isinactive())
		errfatal("%r");

	mountremote();

	print(fmt, "NAME", "IP ADDRESS", "IP ADDRESS");

	if (argc == 0) {
		if (remote(nil) < 0)
			errfatal("%r");
		exits(nil);
	}
	for (i = 0; i < argc; i++) {
		if (remote(argv[i]) < 0)
			errskip(argc - i, argv + i);
	}
	exits(nil);
}
