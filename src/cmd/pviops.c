// Copyright © 2011 Coraid, Inc.
// All rights reserved.
// Display IOPS of PVs

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

static char *secs = "4";

char *Iosizestr[] = { "512", "1024", "2048", "4096", "8192", ">8192" };

typedef struct Pviop Pviop;
struct Pviop {
	int targ;
	int matched;
	Iops rf;
	Iops wf;
};


static void
usage(void) 
{
	fprint(2, "usage: %s [-s seconds] [PV ...]\n", argv0);
	exits("usage");
}

static int
writeiops(char *pool, char *pv) 
{
	char buf[1024];

	snprint(buf, 1024, "/n/xlate/pool/%s/pv/%s/iops", pool, pv);
	return writefile(buf, "%s", secs);
}

static void
printiops(Pviop *s) 
{	
	char *fmt1 = "%-17s %9s %9lld %9lld\n";
	char *fmt2 = "%27s %9lld %9lld\n";
	char targ[16];
	static int phdr;
	int i;
	Iops *r, *w;

	r = &s->rf;
	w = &s->wf;
	snprint(targ, 16, "%d.%d", s->targ >> 8, s->targ & 0xff);
	if (phdr == 0) {
		print("%-17s %9s %9s %9s\n","PV", "IOPS", "READ", "WRITE");
		phdr++;
	}
	if (s->matched) {
		for (i = 0; i < 6; i++) 
			if (i == 0) 
				print(fmt1, targ, Iosizestr[i], r->io[i], w->io[i]);
			else
				print(fmt2, Iosizestr[i],  r->io[i], w->io[i]);
	} else
		print("Error: Unknown target: %s\n",targ);
}	

static int 
iopscmp(void *a, void *b) 
{
	Pviop *sa, *sb;

	sa = a;
	sb = b;
	return sa->targ - sb->targ;
}

/* Return the number of pviop entries */
static int
fillalliop(char *path, char *pool, Pviop **s) 
{
	Pviop *p;
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
	
	/* i is the offset in dir struct, j is offset in my Pviop */
	/* If I have a problem j won't be incremented */
	for (i = j = 0; i < n; i++) {
		if (writeiops(pool, d[i].name) <= 0)
			errfatal("%r");
		if (readfile(buf, 1024, "%s/%s/iops",path,d[i].name) < 0) 
			errfatal("%r");
		if ((p[j].targ = getpv(pool, d[i].name, Pri)) == -1) 
			errfatal("%r");
		/* This is fine, the Ios fields will all be zero */
		if (getfields (buf, args, 16, 1, "\t\r\n ") == 2) {
			parseiops(&p[j].rf, args[0]);
			parseiops(&p[j].wf, args[1]);
		}
		p[j++].matched = 1;
	}
	free(d);
	close(fd);
	return n - (i - j);
}

/* Return pviop entries matched */
static int
filliop(int todo, char *pool, Pviop *s) {
	Pviop *p;
	int fd, n, i, matched, targ;
	Dir *d;
	char buf[1024], *args[16], *b;
	
	matched = 0;
	b = mustsmprint("/n/xlate/pool/%s/pv", pool);
	if ((fd = open(b, OREAD)) < 0) {
		free(b);
		timeoutfatal();
		return 0;
	}
	free(b);
	n = dirreadall(fd, &d);
	if (n < 0) {
		free(d);
		timeoutfatal();
		close(fd);
		return 0;
	}
	for (i = 0; i < n && matched != todo; i++) {
		targ = getpv(pool, d[i].name, Pri);
		for (p = s;  p->targ != -1 && matched != todo; p++) {
			if (targ != p->targ)
				continue;
			if (writeiops(pool, d[i].name) <= 0)
				errfatal("%r");
			p->matched = ++matched;
			if (readfile(buf, 1024, "/n/xlate/pool/%s/pv/%s/iops",pool,d[i].name) < 0) 
				errfatal("%r");
			/* This is fine, the fields will all be zero */
			if (getfields (buf, args, 16, 1, "\t\r\n ") == 2) {
				parseiops(&p->rf, args[0]);
				parseiops(&p->wf, args[1]);
			}
			break;
		}

	}
	free(d);
	close(fd);

	return matched;
}
	
static void 
getalliops(void)
{
	int i, j, n, n2;
	Dir *dp;
	char *b;
	Pviop *s;
	n = numfiles("/n/xlate/pool", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, (int (*)(void *, void *))dirnamecmp);
	for (i = 0; i < n; i++) {
		b = mustsmprint("/n/xlate/pool/%s/pv", dp[i].name);
		n2 = fillalliop(b, dp[i].name, &s);
		free(b);
		qsort(s, n2, sizeof *s, (int (*)(void *, void *))iopscmp);
		for (j = 0; j < n2; j++)
			printiops(&s[j]);
		free(s);
	}
	free(dp);
}

static void 
assigntarg(Pviop *s, char **a) 
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
getiops(int argc, char **argv)
{
	Dir *dp;
	Pviop *s;
	int i,  todo, n;

	todo = argc;
	s = mustmalloc(sizeof(*s) * (argc + 1));
	assigntarg(s, argv);
	n = numfiles("/n/xlate/pool", &dp);
	if (n < 0)
		errfatal("%r");
	for (i = 0; i < n; i++) {
		todo -= filliop(todo, dp[i].name, s);
		if (todo == 0)
			break;
	}
	for (i = 0; i < argc; i++)
		printiops(&s[i]);
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
		getiops(argc, argv);
	else
		getalliops();
	exits(nil);
}
