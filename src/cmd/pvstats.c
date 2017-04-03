// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Display I/O stats of PVs

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

static char *secs = "4";

typedef struct Pvstat Pvstat;
struct Pvstat {
	int targ;
	int matched;
	Ios rf;
	Ios wf;
};


static void
usage(void) 
{
	fprint(2, "usage: %s [-s seconds] [PV ...]\n", argv0);
	exits("usage");
}

static int
writestats(char *pool, char *pv) 
{
	char buf[1024];

	snprint(buf, 1024, "/n/xlate/pool/%s/pv/%s/iostats", pool, pv);
	return writefile(buf, "%s", secs);
}

static void
printstats(Pvstat *s) 
{	
	char *fmt = "%-17s %10s %6lldms %6lldms %10s %6lldms %6lldms\n";
	char rbuf[16], wbuf[16], targ[16];
	static int phdr;
	Ios *r, *w;

	r = &s->rf;
	w = &s->wf;

	if (phdr == 0) {
		print("%-17s %10s %8s %8s %10s %8s %8s\n", "PV", "READ", "AVG", "MAX",
			"WRITE", "AVG", "MAX");
		phdr++;
	}	
	snprint(targ, 16, "%d.%d", s->targ >> 8, s->targ & 0xff);
	if (s->matched == 0) 
		print("Error: unknown PV %s\n",targ);
	else 
		print(fmt, targ, mbstr(rbuf, 16, r->bytes), r->lavg, r->lmax,
			mbstr(wbuf, 16, w->bytes), w->lavg, w->lmax);
}

static int 
statcmp(void *a, void *b) 
{
	Pvstat *sa, *sb;

	sa = a;
	sb = b;
	return sa->targ - sb->targ;
}

/* Return the number of pvstat entries */
static int
fillallstat(char *path, char *pool, Pvstat **s) 
{
	Pvstat *p;
	int fd, n, i, j;
	Dir *d;
	char buf[1024], *args[16];
	
	if ((fd = open(path, OREAD)) < 0) {
		timeoutfatal();
		return 0;
	}
	n = dirreadall(fd, &d);
	if (n < 0) {
		timeoutfatal();
		return 0;
	}
	*s = mustmalloc(n * sizeof (*p));
	p = *s;
	
	/* i is the offset in dir struct, j is offset in my Pvstat */
	/* If I have a problem j won't be incremented */
	for (i = j = 0; i < n; i++) {
		if (writestats(pool, d[i].name) <= 0) 
			errfatal("%r");
		if (readfile(buf, 1024, "%s/%s/iostats",path,d[i].name) < 0) 
			continue;
		if ((p[j].targ = getpv(pool, d[i].name, Pri)) == -1) 
			continue;
		/* This is fine, the Ios fields will all be zero */
		if (getfields (buf, args, 16, 1, "\t\r\n ") == 2) {
			parseiostats(&p[j].rf, args[0]);
			parseiostats(&p[j].wf, args[1]);
		}
		p[j++].matched = 1;
	}
	free(d);
	close(fd);
	return n - (i - j);
}

/* Return pvstat entries matched */
static int
fillstat(int todo, char *pool, Pvstat *s) {
	Pvstat *p;
	int fd, n, i, matched, targ;
	Dir *d;
	char buf[1024], *args[16], *b;
	
	matched = 0;
	b = mustsmprint("/n/xlate/pool/%s/pv", pool);
	if ((fd = open(b, OREAD)) < 0) {
		free(b);
		return 0;
	}
	free(b);
	n = dirreadall(fd, &d);
	if (n < 0) {
		free(d);
		close(fd);
		return 0;
	}
	for (i = 0; i < n && matched != todo; i++) {
		targ = getpv(pool, d[i].name, Pri);
		for (p = s;  p->targ != -1 && matched != todo; p++) {
			if (targ != p->targ)
				continue;
			if (writestats(pool, d[i].name) <= 0) 
				errfatal("%r");
			p->matched = ++matched;
			if (readfile(buf, 1024, "/n/xlate/pool/%s/pv/%s/iostats",pool,d[i].name) < 0) 
				break;
			/* This is fine, the Ios fields will all be zero */
			if (getfields (buf, args, 16, 1, "\t\r\n ") == 2) {
				parseiostats(&p->rf, args[0]);
				parseiostats(&p->wf, args[1]);
			}
			break;
		}

	}
	free(d);
	close(fd);

	return matched;
}
	
static void 
getallstats(void)
{
	int i, j, n, n2;
	Dir *dp;
	char *b;
	Pvstat *s;
	n = numfiles("/n/xlate/pool", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirnamecmp);
	for (i = 0; i < n; i++) {
		b = mustsmprint("/n/xlate/pool/%s/pv", dp[i].name);
		n2 = fillallstat(b, dp[i].name, &s);
		free(b);
		qsort(s, n2, sizeof *s, (int (*)(void *, void *))statcmp);
		for (j = 0; j < n2; j++)
			printstats(&s[j]);
		free(s);
	}
	free(dp);
}

static void 
assigntarg(Pvstat *s, char **a) 
{
	while (*a) {
		s->targ = parsess(*a);
		if (s->targ == -1) 
			errfatal("Unknown target: %s\n",*a);
		a++;
		s++;
	}
	s->targ = -1;
}

static void
getstats(int argc, char **argv)
{
	Dir *dp;
	Pvstat *s;
	int i,  todo, n;

	todo = argc;
	s = mustmalloc(sizeof(*s) * (argc + 1));
	assigntarg(s, argv);
	n = numfiles("/n/xlate/pool", &dp);
	if (n < 0)
		errfatal("%r");
	for (i = 0; i < n; i++) {
		todo -= fillstat(todo, dp[i].name, s);
		if (todo == 0)
			break;
	}
	for (i = 0; i < argc; i++)
		printstats(&s[i]);
}

void
main (int argc, char **argv) 
{
	ARGBEGIN {
	case 's':
		secs = ARGF();
		break;
	default:
		usage();
	} ARGEND

	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	if (argc)
		getstats(argc, argv);
	else
		getallstats();
	exits(nil);
}
