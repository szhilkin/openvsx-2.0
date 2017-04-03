#include <u.h>
#include <libc.h>
#include <bio.h>
#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s [ address ] [ ... ]\n", argv0);
	exits("usage");
}

char *fmt = "%15s %7s %-40s\n";

void
printit(char *ip, char *crypt, char *hash)
{
	if (strcmp(crypt, "null") ==- 0)
		crypt = "no";
	else
		crypt = "yes";

	print(fmt, ip, crypt, hash);
}

int
security(char *ipaddr)
{
	Biobuf *b;
	char *s, *f[4];
	int cmp;

	if (!(b = Bopen("/n/remote/security", OREAD)))
		return -1;

	for (; s = Brdstr(b, '\n', 0); free(s)) {
		if (getfields(s, f, nelem(f), 1, " \n") < 3)
			break;
		if (ipaddr) {
			if ((cmp = strcmp(f[0], ipaddr)) > 0)
				break;
			else if (cmp == 0) {
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

	print(fmt, "IP ADDRESS", "ENCRYPT", "HASH");

	if (argc == 0) {
		if (security(nil) < 0)
			errfatal("%r");
		exits(nil);
	}
	for (i = 0; i < argc; i++) {
		if (security(argv[i]) < 0)
			errskip(argc - i, argv + i);
	}
	exits(nil);
}
