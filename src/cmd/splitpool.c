// Copyright © 2012 Coraid, Inc.
// All rights reserved.

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

enum {
	Maxbuf = 1024,
};

static void
usage(void) 
{
	fprint(2, "usage: %s pool [...]\n", argv0);
	exits("usage");
}

static int
splitpool(char *pool)
{
	int n;
	char buf[Maxbuf];
	Pool *p;

	p = getpoolstatus(pool);
	if (p == nil)
		return -1;
	freepool(p);
	n = readfile(buf, sizeof buf, "/n/xlate/pool/%s/split", pool);
	if (n < 0)
		return -1;
	if (n > 0) {
		buf[n] = '\0';
		fprint(1, "Run on another VSX: /restorepool %s %s",
		       pool, buf);
	}
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
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");

	if (argc <= 0)
		usage();

	for (i = 0; i < argc; i++)
		if (splitpool(argv[i]) < 0)
			errskip(argc - i, argv + i);
	exits(nil);
}
