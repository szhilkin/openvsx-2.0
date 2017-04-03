// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// mac masking a LUN

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

enum {
	Maxargs = 255,
	Maxbuf	= 8 * 1024
};

int phdr;
int pall;

void
usage(void) 
{
	fprint(2,"usage: %s [{+|-}mac ...] [LV ...]\n", argv0);
	exits("usage");
}

int
mask(char *mac, char *lv)
{
	if (islv(lv) == 0) {
		werrstr("%s is not an LV", lv);
		return -1;
	}
	if (lvctlwrite(lv, "mask %s", mac) < 0)
		return -1;
	return 0;
}

void
masks(int n, int c, char **v)
{
	int i, j;

	if (n == c) {
		fprint(2, "error: no LV specified\n");
		return;
	}

	for (i = 0; i < n; i++)
		for (j = n; j < c; j++)
			if (mask(v[i], v[j]) < 0)
				errskip(n - i, v + i);
}

void
printlv(char *lv)
{
	char buf[Maxbuf], *args[Maxargs];
	int i, j, n;
	Lvs *lvs;

	lvs = getlvstatus(lv);
	if (lvs == nil) {
		fprint(2, "error: %s is not an LV\n", lv);
		return;
	}

	if (readfile(buf, Maxbuf, "/n/xlate/lv/%s/mask", lv) < 0) {
		fprint(2, "error: %s is not an LV\n", lv);
		return;
	}
	if ((n = tokenize(buf, args, Maxargs)) < 0) {
		fprint(2, "error: %s has wrong mask file format\n", lv);
		return;
	}
	if (n == 0 && pall) {
		freelvs(lvs);
		return;
	}

	if (phdr == 0) {
		print("%-*s %9s %-s\n", lvlen, "LV", "LUN", "MASK(S)");
		phdr++;
	}
	print("%-*s %T ", lvlen, lv, lvs->lun);
	for (i = 0; i < n; i += j) {
		if (i)
			print("%27s", " ");
		for (j = 0; i+j < n && j < 4; j++)
			print("%s ", args[i+j]);
		print("\n");
	}
	if (i == 0)
		print("\n");

	freelvs(lvs);
}

void
printalllvs(void)
{
	int i, n;
	Dir *dp;

	pall++;
	n = numfiles("/n/xlate/lv", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirlvcmp);
	for (i = 0; i < n; i++)
		printlv(dp[i].name);
	free(dp);
}

int
nmac(int c, char **v)
{
	int i;

	for (i = 0; i < c; i++)
		if (*v[i] != '-' && *v[i] != '+')
			break;
	return i;
}

void
main(int argc, char **argv) 
{
	int n;

	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	fmtinstall('T', Tfmt);

	lvmaxlen();
	if (argc == 1) {
		printalllvs();
		exits(nil);
	}

	argv0 = *argv++;
	argc--;
	if (strcmp(*argv, "-?") == 0)
		usage();

	if ((n = nmac(argc, argv)) > 0)
		masks(n, argc, argv);
	else
		while (argc-- > 0)
			printlv(*argv++);
	exits(nil);
}
