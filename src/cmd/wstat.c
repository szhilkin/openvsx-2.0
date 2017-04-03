// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// Presents work file status

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

enum {
	Maxargs	= 1024,
	Maxbuf	= 8*1024
};

void
usage(void) 
{
	fprint(2,"usage: %s\n", argv0);
	exits("usage");
}

int
widcmp(char **a, char **b)
{
	char *ca, *cb;
	int ia, ib;

	ca = *a;
	cb = *b;
	ia = atoi(ca);
	ib = atoi(cb);
	if (ia == ib)
		return 0;
	else
		return ia < ib ? -1 : 1;
}

int aw;		//length of action commands	
int phdr;

char *
timefmt(vlong sec)
{
	if (sec < 68400)
		return mustsmprint("%02lld:%02lld:%02lld", 
			sec / 3600, 			//hours
			sec % 3600 / 60, 		//minutes
			sec % 3600 % 60);		//seconds
	else
		return mustsmprint("%02lld:%02lld:%02lld:%02lld", 
			sec / 86400, 			//days
			sec % 86400 / 3600,		//hours
			sec % 86400 % 3600 / 60,	//minutes
			sec % 86400 % 3600 % 60);	//seconds
}

void
printwstat(char *line)
{
	char *args[8], *b, *t;
	vlong rate, comp, total;
	int n;

	n = tokenize(line, args, 8);
	if (n != 7)
		errfatal("wrong file format");
	rate  = atoll(args[4]);
	comp  = atoll(args[5]);
	total = atoll(args[6]);

	b = mustsmprint("%s %s->%s", args[1], args[2], args[3]);
	t = rate ? timefmt((total - comp) / rate) : strdup("unknown");
	if (phdr == 0) {
		print("%-*s %7s %10s %11s\n", aw, "ACTION", "DONE(%)", "RATE(MB/s)", "TTC");
		phdr++;
	}
	print("%-*s %7lld %10.2f %11s\n", aw, b, (comp * 100) / total, rate / 1000000.0, t);
	free(b);
	free(t);
}
		
void
readwstat(char *wstat)
{
	char *args[Maxargs], *sp, *ep;
	int n, i, j, w;

	n = getfields(wstat, args, Maxargs, 1, "\n");
	if (n < 0)
		errfatal("getfields: %r");
	qsort(args, n, sizeof *args, (int (*)(void *, void *))widcmp);
	for (i = 0; i < n; i++) {
		sp = strchr(args[i], ' ') + 1;
		if (sp == nil)
			errfatal("strchr: %r");
		ep = sp;
		for (j = 0; j < 3; j++, ep++)
			if ((ep = strchr(ep, ' ')) == nil)
				errfatal("strchr: %r");
		w = ep - sp;
		if (w > aw)
			aw = w;
	}
	for (i = 0; i < n; i++)
		printwstat(args[i]);
}

void
main(int argc, char **argv) 
{
	char buf[Maxbuf];
	int n;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc)
		usage();
	n = readfile(buf, Maxbuf, "/n/xlate/work");
	if (n < 0)
		errfatal("%r");
	else if (n > 0)
		readwstat(buf);
	exits(nil);
}
