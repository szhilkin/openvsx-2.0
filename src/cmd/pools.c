// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// List pools

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

enum {
	Maxargs	= 12,
	Maxbuf	= 100
};

int phdr;
int aflag;
char *gpool;

void
usage(void) 
{
	fprint(2, "usage: %s [-a] [pool ...]\n", argv0);
	exits("usage");
}

int
dirpvindexcmp(Dir *aa, Dir *bb)
{
	int a, b;

	a = getpv(gpool, aa->name, Pri);
	if (a < 0)
		return -1;
	b = getpv(gpool, bb->name, Pri);
	if (b < 0)
		return 1;
	if (a == b)
		return 0;
	return a < b ? -1 : 1;
}

void
printpvs(Dir *dp, int n, char *pool)
{
	int lost[129];
	char b[11];
	int i, j, len, l;
	Pvs *pv;

	gpool = pool;
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirpvindexcmp);

	l = 0;
	print("%*s%s", Pahdr, "PVs", delim);
	for (i = 0; i < n; i += j) {
		len = 0;
		for (j = 0; i+j < n; j++) {
			pv = getpvstatusbyindex(pool, dp[i+j].name);
			if (pv->flags & PVFlost) {
				lost[l++] = pv->primary;
				free(pv);
			} else {
				len += snprint(b, 11, "%,T ", pv->primary);
				free(pv);
				if (len < Plen)
					print("%s", b);
				else
					break;
			}
		}
		if (i+j != n)
			print("\n%.*s", Pleft, " ");
	}
	print("\n");
	/* Conditionally list Lost PVs */
	if (l) {
		print("%*s%s", Pahdr, "Lost PVs", delim);
		for (i = 0; i < l; i += j) {
			len = 0;
			for (j = 0; i+j < l; j++) {
				len += snprint(b, 11, "%,T ", lost[i+j]);
				if (len < Plen)
					print("%s", b);
				else
					break;
			}
			if (i+j != l)
				print("\n%.*s", Pleft, " ");
		}
		print("\n");
	}
}

void
printlvs(char *pool)
{
	char buf[Maxbuf];
	int n, i, j, len;
	Dir *dp;

	print("%*s%s", Pahdr, "LVs", delim);

	n = numfiles("/n/xlate/lv", &dp);
	if (n < 0) {
		print("\n");
		return;
	}
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirlvcmp);
	for (i = 0; i < n; i += j) {
		len = 0;
		for (j = 0; i+j < n; j++) {
			if (readfile(buf, Maxbuf, "/n/xlate/lv/%s/pool", dp[i+j].name) < 0)
				continue;
			if (strcmp(buf, pool))
				continue;
			len += strlen(dp[i+j].name) + 1;
			if (len < Plen)
				print("%s ", dp[i+j].name);
			else
				break;
		}
		if (i+j != n)
			print("\n%*s", Pleft, " ");
	}
	print("\n");
	free(dp);
}

void
printpool(char *pool)
{
	char *b;
	int n;
	Dir *dp;
	Pool *p;

	p = getpoolstatus(pool);
	if (p == nil) {
		fprint(2, "error: %r\n");
		return;
	}

	b = mustsmprint("/n/xlate/pool/%s/pv", pool);
	n = numfiles(b, &dp);
	free(b);
	if (n < 0) {
		freepool(p);
		fprint(2, "error: no PVs in %s\n", pool);
		return;
	}

	if (phdr == 0)
		print("%-*s %5s %10s %10s %7s %7s\n", poollen,
			"POOL", "#PVs", "TOTAL(GB)", "FREE(GB)", "USED(%)", "MODE");
	if (aflag == 0)
		phdr++;
	print("%-*s %5d %Z %Z %7ulld %7s\n",
		poollen, pool, n, p->totalext, p->freeext,
		p->totalext ? (uvlong)(p->totalext - p->freeext) * 100 / p->totalext : 0, p->mode);

	if (aflag) {
		print("%*s%s%uld %,ZGB\n", 
			Pahdr, "Total Exts", delim, p->totalext, p->totalext);
		print("%*s%s%uld %,ZGB\n",
			Pahdr, "Free Exts",  delim, p->freeext, p->freeext);
		printpvs(dp, n, pool);
		printlvs(pool);
	}
	free(dp);
	freepool(p);
}

void 
printallpools(void)
{
	int i, n;
	Dir *dp;

	n = numfiles("/n/xlate/pool", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirnamecmp);
	for (i = 0; i < n; i++)
		printpool(dp[i].name);
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
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	fmtinstall('T', Tfmt);
	fmtinstall('Z', Zfmt);

	poolmaxlen();
	if (argc > 0)
		while (argc-- > 0)
			printpool(*argv++);
	else
		printallpools();
	exits(nil);
}
