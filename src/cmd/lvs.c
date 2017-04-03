// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// List logical volumes

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

enum {
	Maxargs = 512,
	Maxbuf	= 8*1024,
};

int phdr;
int aflag;

typedef struct Lvext Lvext;

struct Lvext
{
	int targ;
	uint exts;
};

Lvext lvexts[128];

void
usage(void) 
{
	fprint(2, "usage: %s [-a] [LV ...]\n", argv0);
	exits("usage");
}

void
printmask(char *lv)
{
	char buf[Maxbuf], *args[Maxargs];
	int n, i, j, len;

	print("%*s%s", Pahdr, "Masks", delim);

	if (readfile(buf, Maxbuf, "/n/xlate/lv/%s/mask", lv) < 0) {
		print("\n");
		return;
	}
	if ((n = tokenize(buf, args, Maxargs)) < 0) {
		print("\n");
		return;
	}
	
	for (i = 0; i < n; i += j) {
		len = 0;
		for (j = 0; i+j < n; j++) {
			len += strlen(args[i+j]) + 1;
			if (len < Plen)
				print("%s ", args[i+j]);
			else
				break;
		}
		if (i+j != n)
			print("\n%*s", Pleft, " ");
	}
	print("\n");
}	

void
printres(char *lv)
{
	char buf[Maxbuf], *args[Maxargs];
	int n, i, j, len;

	print("%*s%s", Pahdr, "Reservations", delim);

	if (readfile(buf, Maxbuf, "/n/xlate/lv/%s/reserve", lv) < 0) {
		print("\n");
		return;
	}
	if ((n = tokenize(buf, args, Maxargs)) < 0) {
		print("\n");
		return;
	}
	
	for (i = 0; i < n; i += j) {
		len = 0;
		for (j = 0; i+j < n; j++) {
			len += strlen(args[i+j]) + 1;
			if (len < Plen)
				print("%s ", args[i+j]);
			else
				break;
		}
		if (i+j != n)
			print("\n%*s", Pleft, " ");
	}
	print("\n");
}

int
lvextcmp(Lvext *a, Lvext *b) {
	return a->targ - b->targ;
}

void
printlvexts(Lvs *lvs)
{
	char buf[Maxbuf], *args[Maxargs], t[16], **p;
	int n, i;

	if (readfile(buf, Maxbuf, "/n/xlate/lv/%s/extents", lvs->name) < 0) {
		errfatal("/n/xlate/lv/%s/extents: %r", lvs->name);
	}
	
	n = tokenize(buf, args, Maxargs);
	if ((n & 1) || n > 256) {
		errfatal("bad extents file");
	}
	p = args;
	n /= 2;
	for (i = 0; i < n; i++) {
		lvexts[i].targ = getpv(lvs->pool, *p++, Pri);
		lvexts[i].exts = atoi(*p++);
	}
	qsort(lvexts, n, sizeof(Lvext), (int (*)(void *, void *))lvextcmp);
	for (i = 0; i < n; i++) {
		snprint(t, sizeof(t), "%T Exts", lvexts[i].targ);
		print("%*s%s%d %,ZGB\n", 
			Pahdr, t, delim, lvexts[i].exts, lvexts[i].exts);
	}
}

void
printlvs(char *lv)
{
	Lvs *lvs;
	char *shadow;

	lvs = getlvstatus(lv);
	if (lvs == nil) {
		fprint(2, "error: %r\n");
		return;
	}
	if (lvs->issnap) {
		free(lvs);
		return;
	}
	if (phdr == 0)
		print("%-*s %10s %4s %9s %*s %6s %6s %9s\n", lvlen,
		      "LV", "SIZE(GB)", "MODE", "LUN", poollen, "POOL", "PROV",
		      "SHADOW", "STATE");
	if (aflag == 0)
		phdr++;

	if (lvs->rmtname)
		shadow = lvs->mode & LVSEND ? "send" : "recv";
	else
		shadow = "";

	print("%-*s %B %4s %T %*s %6s %6s %9s\n",
		lvlen, lv,
		lvs->length,
		lvs->mode & Write ? "r/w" : "r/o",
		lvs->lun,
		poollen, lvs->pool, 
		lvs->mode & LVTHIN ? "thin" : "thick",
		shadow, lvs->state);
	if (aflag) {
		print("%*s%s%d %,ZGB\n", 
			Pahdr, "Total Exts", delim, lvs->totalext, lvs->totalext);
		print("%*s%s%d %,ZGB\n", 
			Pahdr, "Dirty Exts", delim, lvs->dirtyext, lvs->dirtyext);
		print("%*s%s%d %,ZGB\n", 
			Pahdr, "Thin Exts", delim, lvs->thinext, lvs->thinext);
		printlvexts(lvs);
		print("%*s%s%s", Pahdr, "Created", delim, ctime(lvs->ctime));
		print("%*s%s%s\n", Pahdr, "Serial Number", delim, lvs->serial);
		printmask(lv);
		printres(lv);
		print("%*s%s", Pahdr, "Shadow", delim);
		if (lvs->rmtname) {
			if (lvs->mode & LVSEND) {
				print("send to %s @ %s\n",
				      lvs->rmtlv, lvs->rmtname);
			} else {
				print("recv from %s @ %s\n",
				      lvs->rmtlv, lvs->rmtname);
			}
		} else
			print("\n");;
	}
	free(lvs);
}


void 
printalllvs(void)
{
	int i, n;
	Dir *dp;

	n = numfiles("/n/xlate/lv", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirlvcmp);
	for (i = 0; i < n; i++)
		printlvs(dp[i].name);
	free(dp);
}

void
main(int argc, char **argv) 
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
	lvmaxlen();
	poolmaxlen();
	if (argc > 0)
		while (argc-- > 0)
			printlvs(*argv++);
	else
		printalllvs();
	exits(nil);
}
