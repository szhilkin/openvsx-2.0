#include <u.h>
#include <libc.h>

void
usage(void)
{
	fprint(2, "usage: %s count\n", argv0);
	exits("usage");
}

void
mkem(int count)
{
	while (count-- > 0) 
		if (fork() == 0)
			for (;;)
				sleep(1000);
}

void
catch(void *, char *intr)
{
	if (strncmp("interrupt", intr, 9) == 0) {
		print("ding! (%d)\n", getpid());
		noted(NDFLT);
	}
	print("intr=%s\n", intr);
	noted(NDFLT);
}

void
main(int argc, char **argv)
{
	int count;

	ARGBEGIN {
	default:
		usage();
	} ARGEND;
	if (argc != 1)
		usage();
	notify(catch);
	count = atoi(*argv);
	if (0 <= count && count <= 200)
		mkem(count);
	for (;;) 
		sleep(1000);
}
