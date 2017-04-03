// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// resize logical volumes

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

int sflag;

void
usage(void) 
{
	fprint(2,"usage: %s size[T,G,M,K] LV [...]\n", argv0);
	exits("usage");
}

int
lvresize(char *size, char *lv) 
{
	vlong n;
	Lvs *l;

	n = fstrtoll(size);
	if (n < 0)
		usage();

	ask(lv);

	l = getlvstatus(lv);
	if (l == nil) {
		werrstr("%s is not an LV", lv);
		return -1;
	}
	if (n < l->length && sflag == 0) {
		werrstr("shrinking LV is not supported");
		return -1;
	}
	if (lvctlwrite(lv, "resize %lld", n) < 0)
		return -1;
	return 0;
}

void
main(int argc, char **argv) 
{
	int i;

	ARGBEGIN {
	case 'f':
		askres = RespondAll;
		break;
	case 's':
		sflag++;
		break;
	default:
		usage();
	} ARGEND
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc < 2)
		usage();
	print("Warning:  please verify the file system in use on this LUN supports resizing\n"
	      "before altering LV size.\n");
	askhdr(argc-1, argv+1);
	for (i = 1; i < argc; i++)
		if (lvresize(argv[0], argv[i]) < 0)
			errskip(argc - i, argv + i);
	exits(nil);
}
