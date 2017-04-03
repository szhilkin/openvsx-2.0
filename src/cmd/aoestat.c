// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// List available AoE targets

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <vsxcmds.h>

enum {
	Maxargs  = 8,
	Maxpaths = 32,
	Maxbuf	 = 8*1024
};

typedef struct Path Path;
struct Path {
	char *ea;
	ulong port;
	ulong recent;
	ulong retried;
	int retries;
};

typedef struct Targ Targ;
struct Targ {
	int npath;
	int target;
	vlong length;
	Path *paths;
	ulong ports;
	char *config;
	char *serial;
	char *serial2;
	char *error;
};

typedef struct Fd2e Fd2e;
struct Fd2e {
	int fd;
	char *ether;
};

Fd2e *fde;
int nfde;
int aflag;
int sflag;
int phdr;
Targ **targs;
int ntargs;

void freetarg(Targ *t);

void
usage(void) 
{
	fprint(2, "usage: %s [-a -s] [target ...]\n", argv0);
	exits("usage");
}
	
void
getfdether(void)
{
	char buf[Maxbuf], *args[Maxpaths], *line[Maxargs];
	int i;

	if (readfile(buf, Maxbuf, "/n/xlate/targ/ports") < 0)
		errfatal("%r");
	nfde = gettokens(buf, args, Maxpaths, "\n");
	fde = mustmalloc(nfde * sizeof *fde);
	for (i = 0; i < nfde; i++) {
		if (tokenize(args[i], line, Maxargs) != 2)
			errfatal("incorrect port assignments");
		fde[i].fd = atoi(line[0]);
		fde[i].ether = strdup(line[1]);
	}
}

char *
lookupe(int fd)
{
	int i;

	for (i = 0; i < nfde; i++)
		if (fde[i].fd == fd)
			return fde[i].ether;
	return nil;
}

char *
getetherstr(ulong port)
{
	char buf[300], *cp, *ep;
	int i, comma;

	memset(buf, 0, 300);
	cp = buf;
	ep = buf + sizeof buf;
	comma = 0;
	for (i = 0; i < 32; i++)
		if (port & (1 << i)) {
			if (comma)
				cp = seprint(cp, ep, ", ");
			cp = seprint(cp, ep, "%s", lookupe(i));
			comma++;
			}
	return strdup(buf);
}

Targ *
gettargstatus(char *targ)
{
	char buf[Maxbuf], *paths[Maxpaths], *args[Maxargs], con[Maxbuf];
	int i;
	Targ *np;

	if (readfile(con, Maxbuf, "/n/xlate/targ/%s/config", targ) < 0) {
		werrstr("%s is not an AoE target", targ);
		return nil;
	}

	if (readfile(buf, Maxbuf, "/n/xlate/targ/%s/target", targ) < 0)
		return nil;
	
	if (tokenize(buf, args, Maxargs) != 2) {
		werrstr("%s has wrong status file format", targ);
		return nil;
	}
	
	np = mustmalloc(sizeof *np);
	np->config = strdup(con);
	np->target = parsess(args[0]);
	np->length = atoll(args[1]);

	if (readfile(buf, Maxbuf, "/n/xlate/targ/%s/paths", targ) < 0) {
		free(np->config);
		free(np);
		return nil;
	}
	np->npath = gettokens(buf, paths, Maxpaths, "\n");
	np->paths = mustmalloc(np->npath * sizeof *np->paths);
	for (i = 0; i < np->npath; i++) {
		if (tokenize(paths[i], args, Maxargs) != 5) {
			werrstr("%s has wrong port status file format", targ);
			while (i > 0)
				free(np->paths[--i].ea);
			free(np->config);
			free(np->paths);
			free(np);
			return nil;
		}
		np->paths[i].ea = strdup(args[0]);
		np->paths[i].port = strtoul(args[1], nil, 0);
		np->paths[i].recent = strtoul(args[2], nil, 0);
		np->paths[i].retried = strtoul(args[3], nil, 0);
		np->paths[i].retries = strtoul(args[4], nil, 0);
	}
	np->ports = 0;
	for (i = 0; i < np->npath; i++)
		np->ports |= np->paths[i].port;
	if (readfile(buf, Maxbuf, "/n/xlate/targ/%s/serial", targ) < 0) {
		freetarg(np);
		return nil;
	}
	i = getfields(buf, args, Maxargs, 1, "\n");
	if (i > 0)
		np->serial = strdup(args[0]);
	if (i > 1)
		np->serial2 = strdup(args[1]);
	return np;
}

void
freetarg(Targ *t)
{
	int i;

	if (t == nil)
		return;
	for (i = 0; i < t->npath; i++)
		free(t->paths[i].ea);
	free(t->paths);
	free(t->config);
	free(t->serial);
	free(t->serial2);
	free(t->error);
	free(t);
}

void
printtarg(Targ *t)
{
	char *eth;
	int i;

	if (t->length == 0 && !aflag) {
		if (t->error)
			fprint(2, "error: %s\n", t->error);
		else
			fprint(2, "error: target %,T is currently not available\n", t->target);
		return;
	}
	eth = getetherstr(t->ports);
	print("%-T %B %-s\n", t->target, t->length, eth);
	free(eth);
	if (!aflag)
		return;
	for (i = 0; i < t->npath; i++) {
		eth = getetherstr(t->paths[i].port);
		print("\t%s %s\n", t->paths[i].ea, eth);
		free(eth);
		if (t->paths[i].retries) {
			eth = getetherstr(t->paths[i].port & t->paths[i].retried);
			print("\t    retries: %s %d\n", eth, t->paths[i].retries);
			free(eth);
		}
	}
	print("\tConfig string: %s\n", t->config ? t->config : "");
	if (t->serial)
		print("\tSerial number: %s\n", t->serial);
	if (t->serial2)
		print("\tSerial number conflict: %s\n", t->serial2);
}

int
targsscmp(Targ **aa, Targ **bb)
{
	Targ *a, *b;

	a = *aa;
	b = *bb;
	if (a->target == b->target)
		return 0;
	return a->target < b->target ? -1 : 1;
}

int
targlencmp(Targ **aa, Targ **bb)
{
	Targ *a, *b;

	a = *aa;
	b = *bb;
	if (a->length == b->length)
		return targsscmp(aa, bb);
	return a->length < b->length ? -1 : 1;
}

void
printheader(void) {
	if (phdr == 0) {
		print("%-9s %10s %-8s\n", "TARGET", "SIZE(GB)", "PORT(S)");
		phdr++;
	}
}

void
inserttargs(char *targ)
{
	char buf[ERRMAX];
	Targ *t;

	/* Print the header before getting the status because it may error */
	printheader();
	t = gettargstatus(targ);
	if (t == nil) {
		t = mustmalloc(sizeof *t);
		rerrstr(buf, ERRMAX);
		t->error = strdup(buf);
		t->target = parsess(targ);
		t->length = 0;
	}
	targs[ntargs++] = t;
}

void
main(int argc, char **argv) 
{
	int i, n;
	Dir *dp;

	ARGBEGIN {
	case 'a':
		aflag++;
		break;
	case 's':
		sflag++;
		break;
	default:
		usage();
	} ARGEND
	if (serieserr(argc, argv, Noto) < 0)
		errfatal("%r");
	fmtinstall('T', Tfmt);
	fmtinstall('B', Bfmt);

	getfdether();
	ntargs =  0;
	if (argc) {
		targs = mustmalloc(argc * sizeof *targs);
		while (argc--)
			inserttargs(*argv++);
		if (sflag)
			qsort(targs, ntargs, sizeof *targs, (int (*)(void *, void *))targlencmp);
	} else {
		n = numfiles("/n/xlate/targ", &dp);
		if (n < 0)
			errfatal("%r");
		targs = mustmalloc(n * sizeof *targs);
		for (i = 0; i < n; i++)
			if (isdigit(*dp[i].name))
				inserttargs(dp[i].name);
		qsort(targs, ntargs, sizeof *targs, (int (*)(void *, void *))(sflag ? targlencmp : targsscmp));
		free(dp);
	}
	for (i = 0; i < ntargs; i++) {
		printtarg(targs[i]);
		freetarg(targs[i]);
	}
	free(targs);
	if (phdr == 0)
		sysfatal("No SR/SRX LUNs found");
	exits(nil);
}
