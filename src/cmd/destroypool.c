#include <u.h>
#include <libc.h>
#include "vsxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s pool [...]\n", argv0);
	exits("usage");
}

int 
destroypool(char *pool)
{
	if (ispool(pool) == 0) {
		werrstr("%s is not a pool", pool);
		return -1;
	}
	ask(pool);
	if (ctlwrite("destroypool %s", pool) < 0)
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
		if (destroypool(*argv++) < 0)
			errskip(argc, argv);
	exits(nil);
}
