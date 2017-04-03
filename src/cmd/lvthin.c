#include <u.h>
#include <libc.h>
#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s LV [...]\n", argv0);
	exits("usage");
}

int 
lvthin(char *lv)
{
	if (islv(lv) == 0) {
		werrstr("%s is not an LV", lv);
		return -1;
	}
	if (lvctlwrite(lv, "thin") < 0)
		return -1;
	return 0;
}

void
main(int argc, char **argv) 
{
	ARGBEGIN {
	case 'f':
		askres = RespondAll;
		break;
	default:
		usage();
	} ARGEND

	if (isinactive())
		errfatal("%r");

	if (argc < 1)
		usage();

	askhdr(argc, argv);
	while (argc-- > 0)
		if (lvthin(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
