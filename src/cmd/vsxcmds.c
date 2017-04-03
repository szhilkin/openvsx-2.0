#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include "vsxcmds.h"

enum {
	RPool,
	RIndex,
	Maxargs = 128,
	Maxbuf	= 8192,
	Serialsz	= 21,
	Classz	= 16
};

int askres;
int bshelf;
char *delim = " : ";

/* Return true if a name identifies a pool */
int
ispool(char *name) 
{
	int fd;
	char *b;

	b = mustsmprint("/n/xlate/pool/%s", name);
	fd = open(b, OREAD);
	free(b);
	if (fd >= 0) {
		close(fd);
		return 1;
	}
	return 0;
}

/* Return true if name is an lv */
int
islv(char *name) 
{
	int fd;
	char *b;

	b = mustsmprint("/n/xlate/lv/%s", name);
	fd = open (b, OREAD);
	free(b);
	if (fd > 0) {
		close(fd);
		return 1;
	}
	return 0;

}

/* Returns the pv(int) in status */
int
getpv(char *pool, char *index, int state)
{
	char buf[Maxbuf], *args[Maxargs];

	if (readfile(buf, Maxbuf, "/n/xlate/pool/%s/pv/%s/status", pool, index) < 0)
		return -1;

	if (tokenize(buf, args, Maxargs) != 10) {
		werrstr("corrupted status file");
		return -1;
	}
	if (state == Pri)
		return parsess(args[5]);
	else if (state == Mir)
		return parsess(args[6]);
	else {
		werrstr("do not know state value");
		return -1;
	}
}

/* Return Pvs struct based on pool and index */
Pvs *
getpvstatusbyindex(char *pool, char *index)
{
	Pvs *np;
	char buf[Maxbuf], *args[Maxargs];

	if (pool == nil || index == nil)
		return nil;
	if (readfile(buf, Maxbuf, "/n/xlate/pool/%s/pv/%s/status", pool, index) < 0)
		return nil;

	if (tokenize(buf, args, Maxargs) != 10) {
		werrstr("/n/xlate/pool/%s/pv/%s/status: wrong status file format", pool, index);
		return nil;
	}

	np = mustmalloc(sizeof *np);
	np->pool	= strdup(pool);
	np->state	= strdup(args[0]);
	np->freeext	= atoi(args[1]);
	np->usedext	= atoi(args[2]);
	np->metaext	= atoi(args[3]);
	np->totalext	= atoi(args[4]);
	np->primary	= parsess(args[5]);
	np->mirror	= parsess(args[6]);
	np->length	= atoll(args[7]);
	np->flags	= atoi(args[8]);
	np->dirtyext	= atoi(args[9]);

	if (readfile(buf, Maxbuf, "/n/xlate/pool/%s/pv/%s/ctime", pool, index) < 0)
		np->ctime = 0;
	else
		np->ctime = atol(buf);
	return np;
}

/* Returns the name of the pool or index that owns this physical volume,
     or nil if no pool owns this pv. The caller must free the returned string */
char *
searchpvs(char *pv, int state, int ret)
{
	int i, j, n, n2, ss;
	Dir *dp, *dp2;
	char *b, *r;

	ss = parsess(pv);
	if (ss < 0) {
		werrstr("%s is invalid PV name", pv);
		return nil;
	}
	n = numfiles("/n/xlate/pool", &dp);
	if (n < 0)
		errfatal("%r");
	for (r = nil, i = 0; i < n && r == nil; i++) {
		b = mustsmprint("/n/xlate/pool/%s/pv", dp[i].name);
		n2 = numfiles(b, &dp2);
		free(b);
		if (n2 < 0)
			errfatal("%r");
		for (j = 0; j < n2; j++) {
			if (getpv(dp[i].name, dp2[j].name, state) == ss) {
				if (ret == RPool)
					r = strdup(dp[i].name);
				else
					r = strdup(dp2[j].name);
				break;
			}
		}
		free(dp2);	
	}
	free(dp);
	return r;
}

char *
getpvpool(char *pv)
{
	return searchpvs(pv, Pri, RPool);
}

char *
getpvindex(char *pv)
{
	return searchpvs(pv, Pri, RIndex);
}

char *
getmirpool(char *aoe)
{
	return searchpvs(aoe, Mir, RPool);
}
char *
getmirindex(char *aoe)
{
	return searchpvs(aoe, Mir, RIndex);
}

int 
vsxwritefile(char *file, char *fmt, va_list arg)
{
	int fd, n;
	
	fd = open(file, OWRITE);
	if (fd < 0) {
		timeoutfatal();
		return -1;
	}
	n = vfprint(fd, fmt, arg);
	if (n < 0)
		timeoutfatal();
	close(fd);
	return n;	
}

int
lvctlwrite(char *lv, char *fmt, ...) 
{
	char *b;
	int n;
	va_list arg;
	
	b = mustsmprint("/n/xlate/lv/%s/ctl", lv);
	va_start(arg, fmt);
	n = vsxwritefile(b, fmt, arg);
	va_end(arg);
	free(b);
	return n;
}

int
ctlwrite(char *fmt, ...) 
{
	int n;
	va_list arg;

	va_start(arg, fmt);
	n = vsxwritefile("/n/xlate/ctl", fmt, arg);
	va_end(arg);
	return n;
}

int
poolctlwrite(char *pool, char *fmt, ...)
{
	char *b;
	int n;
	va_list arg;

	b = mustsmprint("/n/xlate/pool/%s/ctl", pool);
	va_start(arg, fmt);
	n = vsxwritefile(b, fmt, arg);
	va_end(arg);
	free(b);
	return n;
}

int
lunctlwrite(int lunoff, char *fmt, ...)
{
	char *b;
	int n;
	va_list arg;

	b = mustsmprint("/n/xlate/lun/%d/ctl", lunoff);
	va_start(arg, fmt);
	n = vsxwritefile(b, fmt, arg);
	va_end(arg);
	free(b);
	return n;
}

int
writefile(char *file, char *fmt, ...)
{
	int n;
	va_list arg;

	va_start(arg, fmt);
	n = vsxwritefile(file, fmt, arg);
	va_end(arg);
	return n;
}

int
readfile(char *buf, int len, char *fmt, ...)
{
	char b[255];
	int fd, n;
	va_list arg;

	va_start(arg, fmt);
	n = vsnprint(b, sizeof b, fmt, arg);
	va_end(arg);
	if (n < 0) {
		werrstr("vsnprint failed %r");
		return -1;
	}
	fd = open(b, OREAD);
	if (fd < 0) {
		timeoutfatal();
		return -1;
	}
	memset(buf, 0, len);
	n = read(fd, buf, len);
	if (n < 0)
		timeoutfatal();
	close(fd);
	return n;
}

int
numfiles(char *dir, Dir **d)
{
	int n, fd;

	if ((fd = open(dir, OREAD)) < 0) {
		timeoutfatal();
		return -1;
	}
	n = dirreadall(fd, d);
	if (n < 0)
		timeoutfatal();
	close(fd);
	return n;
}

char *
mustsmprint(char *fmt, ...)
{
	char *cp;
	va_list arg;

	va_start(arg, fmt);
	cp = vsmprint(fmt, arg);
	va_end(arg);
	if (cp == nil)
		errfatal("smprint failed %r");
	return cp;
}

void *
mustmalloc(ulong size)
{
	void *p;

	if ((p = mallocz(size, 1)) == nil)
		errfatal("malloc failed %r");
	return p;
}

void
parseiostats(Ios *ios, char *str)
{
	int n;
	char *a[16];
	n = getfields(str, a, nelem(a), 1, ":/");
	if (n == 4) {
		ios->ns = atoi(a[0]);
		if (ios->ns == 0) 
			return;
	
		ios->bytes = strtoull(a[1], 0, 0);
		ios->lavg = strtoull(a[2], 0, 0);
		ios->lmax = strtoull(a[3], 0, 0);
	} else 
		fprint(2, "error: iostats has %d fields\n",n);
}

void
parseiops(Iops *iops, char *str)
{
	int n;
	char *a[16];
	n = getfields(str, a, nelem(a), 1, ":/");
	if (n == 7) {
		iops->ns = atoi(a[0]);
		if (iops->ns == 0) 
			return;
		iops->io[0] = strtoull(a[1], 0, 0);
		iops->io[1] = strtoull(a[2], 0, 0);
		iops->io[2] = strtoull(a[3], 0, 0);
		iops->io[3] = strtoull(a[4], 0, 0);
		iops->io[4] = strtoull(a[5], 0, 0);
		iops->io[5] = strtoull(a[6], 0, 0);
	} else 
		fprint(2, "error: iops has %d fields\n",n);
}

int
Tfmt(Fmt *f)
{
	char buf[10];
	int t;
	
	t = va_arg(f->args, int);
	if (t < 0) {
		if (f->flags & FmtComma)
			return fmtprint(f, "%s", "");
		else
			return fmtprint(f, "%9s", "");
	}
	if (f->flags & FmtSharp)
		return fmtprint(f, "%5d.%-3d", t >> 8, t & 0xff);
	snprint(buf, 10, "%d.%d", t >> 8, t & 0xff);
	if (f->flags & FmtLeft)
		return fmtprint(f, "%-9s", buf);
	else if (f->flags & FmtComma)
		return fmtprint(f, "%s", buf);
	else
		return fmtprint(f, "%9s", buf);
}

int
Zfmt(Fmt *f)
{
	uint z;
	
	z = va_arg(f->args, uint);
	if (f->flags & FmtComma)
		return fmtprint(f, "%.3f",   z * (float)Ext2B / (float)Byte2G);
	else
		return fmtprint(f, "%10.3f", z * (float)Ext2B / (float)Byte2G);
}

int
Bfmt(Fmt *f)
{
	uvlong b;
	
	b = va_arg(f->args, uvlong);
	if (f->flags & FmtComma)
		return fmtprint(f, "%.3f",   b / (float)Byte2G);
	else
		return fmtprint(f, "%10.3f", b / (float)Byte2G);
}

int
parsess(char *s)
{
	int u, v;
	
	while (*s == ' ' || *s == '\t')
		s++;
	if (s == nil)
		return -1;
	v = 0;
	while ('0' <= *s && *s <= '9')
		v = v * 10 + *s++ - '0';
	u = v;
	if (*s != '.')
		return -1;
	s++;
	v = 0;
	while ('0' <= *s && *s <= '9')
		v = v * 10 + *s++ - '0';
	return u << 8 | v & 0xff;
}

int
dirintcmp(Dir *aa, Dir *bb)
{
	int a, b;

	a = atoi(aa->name);
	b = atoi(bb->name);
	if (a == b)
		return 0;
	return a < b ? -1 : 1;
}

int
dirnamecmp(Dir *a, Dir *b)
{
	return strcmp(a->name, b->name);
}

int
dirnamesscmp(Dir *a, Dir *b)
{
	int aa, bb;

	aa = parsess(a->name);
	bb = parsess(b->name);
	
	if (aa == bb)
		return 0;
	return aa < bb ? -1 : 1;
}

void
move(char *s1, char *s2, char *e2)
{
	while (s2 != e2)
		*s1++ = *s2++;
	*s1 = 0;
}

int
dirlvcmp(Dir *da, Dir *db)
{
	char ba[100], bb[100];
	char *ca, *cb;
	int a, b, n;

	ca = strchr(da->name, '.');
	if (ca == nil) {
		memccpy(ba, da->name, '\0', 100);
		a = 0;
	} else {
		move(ba, da->name, ca);
		a = atoi(ca+1);
	}

	cb = strchr(db->name, '.');
	if (cb == nil) {
		memccpy(bb, db->name, '\0', 100);
		b = 0;
	} else {
		move(bb, db->name, cb);
		b = atoi(cb+1);
	}

	n = strcmp(ba, bb);
	if (n == 0) {
		if (a == b)
			return 0;
		return a < b ? -1 : 1;
	} else
		return n;
}

Pool *
getpoolstatus(char *pool)
{
	char buf[Maxbuf], *args[Maxargs];
	Pool *np;

	if (ispool(pool) == 0) {
		werrstr("%s is not a pool", pool);
		return nil;
	}

	if (readfile(buf, Maxbuf, "/n/xlate/pool/%s/status", pool) < 0)
		return nil;

	if (tokenize(buf, args, Maxargs) != 4) {
		werrstr("%s has wrong status file format", pool);
		return nil;
	}
	np = mustmalloc(sizeof *np);
	np->freeext = strtoul(args[0], nil, 0);
	np->totalext = strtoul(args[1], nil, 0);
	np->unique = strtoul(args[2], nil, 0);
	np->mode = strdup(args[3]);
	return np;
}

void
freepool(Pool *p)
{
	if (p == nil)
		return;
	free(p->mode);
	free(p);
}

Pvs *
getpvstatus(char *pv)
{
	char *pool, *index;
	Pvs *np;

	pool = getpvpool(pv);
	if (pool == nil) {
		werrstr("%s is not a PV", pv);
		return nil;
	}
	index = getpvindex(pv);
	np = getpvstatusbyindex(pool, index);
	free(pool);
	free(index);
	return np;
}

void
freepvs(Pvs *p)
{
	if (p == nil)
		return;
	if (p->pool)
		free(p->pool);
	if (p->state)
		free(p->state);
	free(p);
}

Lvs *
getlvstatus(char *lv)
{
	char buf[Maxbuf], *args[Maxargs], *pool, serial[Serialsz];
	Lvs *np;

	if (islv(lv) == 0) {
		werrstr("%s is not an LV", lv);
		return nil;
	}

	if (readfile(buf, Maxbuf, "/n/xlate/lv/%s/pool", lv) < 0)
		return nil;
	pool = strdup(buf);

	if (readfile(buf, Maxbuf, "/n/xlate/lv/%s/status", lv) < 0) {
		free(pool);
		return nil;
	}

	if (tokenize(buf, args, Maxargs) != 18) {
		free(pool);
		werrstr("%s has wrong status file format", lv);
		return nil;
	}

	if (readfile(serial, Serialsz - 1, "/n/xlate/lv/%s/serial",lv) < 0) {
		free(pool);
		return nil;
	}
	
	np = mustmalloc(sizeof *np);
	np->pool	= pool;
	np->name	= strdup(lv);
	np->mode	= strtoul(args[0], &args[0], 0);
	np->totalext	= atoi(args[1]);
	np->thinext	= atoi(args[2]);
	np->dirtyext	= atoi(args[3]);
	np->uniqext	= atoi(args[4]);
	np->length	= atoll(args[7]);
	np->issnap	= strcmp(args[8], "normal");
	np->lun		= parsess(args[9]);
	np->actlen	= atoll(args[11]);
	np->vlan	= atoi(args[12]);
	np->snapext	= atoi(args[13]);
	strncpy(np->serial, serial, Serialsz -1);
	strncpy(np->class, args[8], Classz - 1);
	if (strcmp(args[14], "unset") != 0) {
		np->rmtname = strdup(args[14]);
		np->rmtlv = strdup(args[15]);
	}
	np->state	= strdup(args[16]);
	if (readfile(buf, Maxbuf, "/n/xlate/lv/%s/ctime", lv) < 0)
		np->ctime = 0;
	else
		np->ctime = atol(buf);
	return np;
}

void
freelvs(Lvs *l)
{
	if (l == nil)
		return;
	if (l->pool)
		free(l->pool);
	free(l->rmtname);
	free(l->rmtlv);
	free(l->name);
	free(l->state);
	free(l);
}

void
getshelf(void)
{
	char buf[100], *cp;

	if (readfile(buf, sizeof buf, "/n/xlate/targ/shelf") < 0)
		errfatal("%r");
	cp = buf;
	while (*cp == ' ')
		cp++;
	if (!isdigit(*cp))
		bshelf = -1;
	else
		bshelf = atoi(cp);
}

Lun *
getlunstatus(char *lun)
{
	char buf[Maxbuf], *args[Maxargs];
	int l;
	Lun *np;

	l = parsess(lun);
	if (l < 0) {
		werrstr("%s has invalid LUN format [shelf.slot]", lun);
		return nil;
	}
	getshelf();
	if (bshelf < 0) {
		werrstr("must set shelf");
		return nil;
	}
	if (readfile(buf, Maxbuf, "/n/xlate/lun/%d/status", l - (bshelf << 8)) < 0) {
		werrstr("%s is not a LUN", lun);
		return nil;
	}
	if (tokenize(buf, args, Maxargs) != 3) {
		werrstr("%s has wrong status file format", lun);
		return nil;
	}
	if (strcmp(lun, args[2])) {
		werrstr("%s is not a LUN", lun);
		return nil;
	}
	np = mustmalloc(sizeof *np);
	np->status = strdup(args[0]);
	np->lv	   = strdup(args[1]);
	np->lun    = l;
	np->loffset = l - (bshelf << 8);
	return np;
}

void
freelun(Lun *l)
{
	if (l == nil)
		return;
	if (l->status)
		free(l->status);
	if (l->lv)
		free(l->lv);
	free(l);
}

int
getposint(char *s)
{
	char *p;

	if (*s == '\0') {
		werrstr("no string");
		return -1;
	}
	for (p = s; *p; p++)
		if (!isdigit(*p)) {
			werrstr("%s is not a number", s);
			return -1;
		}
	return atoi(s);
}

int
serieserr(int c, char **v, int ifto)
{
	return serieserrs(c, v, ifto, 0);
}

int
serieserrs(int c, char **v, int ifto, int s)
{
	int i, j, t;
	
	t = s;
	for (i = s; i < c; i++) {
		if (strcmp(v[i], "to") == 0) {
			t = i;
			if (ifto & Noto) {
				werrstr("\'to\' reserved");
				return -1;
			}
		}
	}
	if (t) {
		if (t != s + c - t - 1) {
			werrstr("\'to\' operator requires equal elements in each set\n%d \'to\' %d is invalid", t, s + c - t - 1);
			return -1;
		}
		if (ifto & SameOK)
			return t;

		for (i = s; i < t; i++)
			for (j = t + 1; j < c; j++)
				if (strcmp(v[i], v[j]) == 0) {
					werrstr("%s is in both series", v[i]);
					return -1;
				}
	}
	return t;
}

char *
stem(char *s)
{
	if (strncmp(s, "rm", 2) == 0 || strncmp(s, "un", 2) == 0)
		return s + 2;
	else if (strncmp(s, "clrpv", 5) == 0)
		return "PV";
	else if (strncmp(s, "lv", 2) == 0)
		return "LV";
	else if (strcmp(s, "clrsnapsched") == 0)
		return "snapshot schedule";
	else if (strcmp(s, "setsnaplimit") == 0)
		return "snapshot limit";
	else if (strstr(s, "clrreservation") != 0)
		return "LV";
	else if (strstr(s, "destroypool"))
		return "pool";
	else
		return "LUN";
}

void
askhdr(int c, char **v)
{
	if (askres == RespondAll)
		return;
	print("Request to %s %d %s%s: ", argv0, c, stem(argv0), c > 1 ? "s" : "");
	switch (c) {
	case 1:
		break;
	case 2:
		print("%s and ", v[0]);
		break;
	default:
		print("%s ... ", v[0]);
		break;
	}
	print("%s\n", v[c-1]);
}

char *
mbstr(char *p, int n, ulong bytes)
{
	enum { K= 1000, M= K*K, };
	ulong m, k;

	m = bytes / M;
	k = bytes % M / K;
	snprint(p, n, "%uld.%03uldMB", m, k);
	return p;
}

int
askresponse(void)
{
	char buf[100];
	int n;

	n = read(0, buf, sizeof buf);
	buf[n-1] = 0;
	if (cistrncmp(buf, "y", 1) == 0)
		return RespondOne;
	else if (cistrncmp(buf, "a", 1) == 0)
		return RespondAll;
	else
		sysfatal("action canceled");
	return -1;
}

void 
ask(char *s)
{
	if (askres == RespondAll)
		return;
	print("\'n\' to cancel, \'a\' for all, or \'y\' to %s %s [n]: ", argv0, s);
	askres = askresponse();
}

int lvlen;
int poollen;

void
lvmaxlen(void)
{
	int n, fd, i;
	Dir *dp;

	lvlen = 20;
	fd = open("/n/xlate/lv", OREAD);
	if (fd < 0)
		return;
	n = dirreadall(fd, &dp);
	close(fd);
	if (n < 0)
		return;
	lvlen = 2;
	for (i = 0; i < n; i++)
		lvlen = max(strlen(dp[i].name), lvlen);
	free(dp);
}

void
poolmaxlen(void)
{
	int n, fd, i;
	Dir *dp;

	poollen = 16;
	fd = open("/n/xlate/pool", OREAD);
	if (fd < 0)
		return;
	n = dirreadall(fd, &dp);
	close(fd);
	if (n < 0)
		return;
	poollen = 4;
	for (i = 0; i < n; i++)
		poollen = max(strlen(dp[i].name), poollen);
	free(dp);
}

void
errfatal(char *fmt, ...)
{
	char buf[1024];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf+sizeof(buf), fmt, arg);
	va_end(arg);
	fprint(2, "error: %s\n", buf);
	exits(buf);
}

/* formatted str to ll */
vlong
fstrtoll(char *str)
{
	double n;
	char *p;

	n = strtod(str, &p);
	if (n < 0 || p == str)
		goto error;
	switch (*p) {
	case 't':
	case 'T':
		n *= 1000;
	case 'g':
	case 'G':
		n *= 1000;
	case 'm':
	case 'M':
		n *= 1000;
	case 'K':
	case 'k':
		n *= 1000;
		p++;
		break;
	default:		// assume byte
		break;
	}
	if (*p != '\0') {
error:		fprint(2, "error: invalid size label %s [T, G, M, K, or none for bytes]\n", p);
		return -1;
	}
	return (vlong)n;
}

void 
timeoutfatal(void)
{
	char buf[ERRMAX];

	rerrstr(buf, sizeof buf);
	if (strstr(buf, "timeout"))
		errfatal("%r");
}

void
errskip(int argc, char **argv)
{
	fprint(2, "error: %r\n");
	if (argc > 0) {
		fprint(2, "skipping:");
		while (argc-- > 0)
			fprint(2, " %s", *argv++);
		fprint(2, "\n");
	}
	exits(smprint("%r"));
}

int
isinactive(void)
{
	int n;
	char buf[Maxbuf];

	n = readfile(buf, Maxbuf, "/n/ha/state");

	if (n < 0)
		return 1;

	buf[n] = '\0';

	if (strcmp(buf, "active") == 0)
		return 0;

	werrstr("VSX core hastate inactive");
	return 1;
}

void
mountremote(void)
{
	int fd;

	if (access("/n/remote/name", 0) < 0) {
		if ((fd = open("/srv/remote", ORDWR)) >= 0) {
			mount(fd, -1, "/n/remote", MREPL, "");
			close(fd);
		}
	}
}

char *
clsstr(int n)
{
	switch (n) {
	case LVSmanual:
		return "manual";
	case LVShour:
		return "hourly";
	case LVSday:
		return "daily";
	case LVSweek:
		return "weekly";
	case LVSmonth:
		return "monthly";
	case LVSyear:
		return "yearly";
	}
	errfatal("Error unknown class: %d\n", n);
	return ""; // Oh, compiler
}

static char *
daystr(int n)
{
	switch (n) {
	case 0:
		return "Sun";
	case 1:
		return "Mon";
	case 2:
		return "Tue";
	case 3:
		return "Wed";
	case 4:
		return "Thu";
	case 5:
		return "Fri";
	case 6:
		return "Sat";
	}
	errfatal("Error unknown day: %d\n", n);
	return ""; // Oh, compiler
}

static char *
monstr(int n)
{
	switch (n) {
	case 0:
		return "Jan";
	case 1:
		return "Feb";
	case 2:
		return "Mar";
	case 3:
		return "Apr";
	case 4:
		return "May";
	case 5:
		return "Jun";
	case 6:
		return "Jul";
	case 7:
		return "Aug";
	case 8:
		return "Sep";
	case 9:
		return "Oct";
	case 10:
		return "Nov";
	case 11:
		return "Dec";
	}
	errfatal("Error unknown month: %d\n", n);
	return ""; // Oh, compiler
}

int
getsnapsched(char *lv, int ntime, int *time)
{
	int k, i;
	char buf[8192], *args[256], *e;

	if (readfile(buf, 8192, "/n/xlate/lv/%s/snapsched", lv) < 0) {
		errfatal("%s: %r\n", lv);
	}
	k =  tokenize(buf, args, 256);
	if (k % 7) {
		errfatal("%s: unexpected elements in snapsched\n", lv);
	}
	if (k > ntime)
		errfatal("getsnapsched: buf too small\n");
	memset(time, 0, sizeof(int) * ntime);
	for (i = 0; i < k; i++) {
		time[i] = strtoll(args[i], &e, 10);
		if (*e)
			errfatal("getsnapsched: invalid parse value");
	}
	return k;
}	

/* Create a string describing the schedule */
/* In the time[] the offsets hold:
	0 Snapshot Class
	1 Retain Count
	2 Month 
	3 Day of Month
	4 Weekday
	5 Hour
	6 Minute
*/
char *
schedstr(int time[])
{
	char *r;

	r = nil;

	switch (time[0]) {
	case LVSmanual:
		r = smprint("");
		break;
	case LVShour:
		r = smprint("@xx:%.2d", time[6]);
		break;
	case LVSday:
		r = smprint("@%.2d:%.2d", time[5], time[6]);
		break;
	case LVSweek:
		r = smprint("%s@%.2d:%.2d", daystr(time[4]), time[5], time[6]);
		break;
	case LVSmonth:
		r = smprint("%d@%.2d:%.2d", time[3], time[5], time[6]);
		break;
	case LVSyear:
		r = smprint("%s\.%d@%.2d:%.2d", monstr(time[2]), time[3], time[5], time[6]);
		break;
	default:
		errfatal("schedstr: unknown class: %d", time[0]);
	}
	if (r == nil)
		errfatal("schedstr: %r");
	return r;
}