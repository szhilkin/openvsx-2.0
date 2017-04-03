// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// List pools

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

int phdr;
int aflag;
char *pool;

void
usage(void) 
{
	fprint(2, "usage: %s [-a] [PV ...]\n", argv0);
	exits("usage");
}

void
printpvs(char *pv)
{
	char buf[10], state[256];
	int n;
	Pvs *pvs;

	if (strchr(pv, '.') == nil) {
		n = getpv(pool, pv, Pri);
		if (n < 0) {
			fprint(2, "error: %s is not a PV\n", pv);
			return;
		}
		snprint(buf, 10, "%,T", n);
		pv = buf;
	}
	pvs = getpvstatus(pv);
	if (pvs == nil) {
		fprint(2, "error: %r\n");
		return;
	}
	if (pvs->flags & PVFlost)
		snprint(state, sizeof state, "%s-lost", pvs->state);
	else
		strncpy(state, pvs->state, sizeof state);
	if (phdr == 0)
		print("%-9s %10s %10s %9s %14s %*s\n",
		"PV", "TOTAL(GB)", "FREE(GB)", "MIRROR", "STATE", poollen, "POOL");
	if (aflag == 0)
		phdr++;
	print("%-T %B %Z %T %14s %*s\n", 
		parsess(pv), 
		pvs->length, 
		pvs->freeext, 
		pvs->mirror,
		state,
		poollen, pvs->pool);
	if (aflag) {
		print("%*s%s%d %,ZGB\n",
			Pahdr, "Total Exts", delim, pvs->totalext, pvs->totalext);
		print("%*s%s%d %,ZGB\n",
			Pahdr, "Free Exts",  delim, pvs->freeext,  pvs->freeext);
		print("%*s%s%d %,ZGB\n",
			Pahdr, "Used Exts",  delim, pvs->usedext,  pvs->usedext);
		print("%*s%s%d %,ZGB\n",
			Pahdr, "Dirty Exts", delim, pvs->dirtyext, pvs->dirtyext);
		print("%*s%s%d %,ZGB\n",
			Pahdr, "Meta Exts",  delim, pvs->metaext,  pvs->metaext);
		print("%*s%s%s", Pahdr, "Created", delim, ctime(pvs->ctime));
	}
	freepvs(pvs);
}

int
dirpvindexcmp(Dir *aa, Dir *bb)
{
	int a, b;

	a = getpv(pool, aa->name, Pri);
	if (a < 0)
		return -1;
	b = getpv(pool, bb->name, Pri);
	if (b < 0)
		return 1;
	if (a == b)
		return 0;
	return a < b ? -1 : 1;
}
	
void 
printallpvs(void)
{
	int i, j, n, n2;
	Dir *dp, *dp2;
	char *b;

	n = numfiles("/n/xlate/pool", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirnamecmp);
	for (i = 0; i < n; i++) {
		b = mustsmprint("/n/xlate/pool/%s/pv", dp[i].name);
		n2 = numfiles(b, &dp2);
		if (n2 < 0)
			errfatal("%r");
		free(b);
		pool = dp[i].name;
		qsort(dp2, n2, sizeof *dp2, (int (*)(void *, void *))dirpvindexcmp);
		for (j = 0; j < n2; j++)
			printpvs(dp2[j].name);
		free(dp2);	
	}
	free(dp);
}

void
main (int argc, char **argv) 
{
	ARGBEGIN {
	case 'a':
		aflag++;
		break;
	default:
		usage();
	} ARGEND
	fmtinstall('T', Tfmt);
	fmtinstall('B', Bfmt);
	fmtinstall('Z', Zfmt);
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	poolmaxlen();
	if (argc > 0)
		while (argc-- > 0)
			printpvs(*argv++);
	else
		printallpvs();
	exits(nil);
}
