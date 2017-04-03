#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <libsec.h>
#include <thread.h>
#include <9p.h>
#include <ip.h>
#include <bio.h>
#include <libcutil.h>
#include "shadow.h"
#include "haconfig.h"

int dflag;
#define dprint(...) if (dflag == 0) USED(dflag); else print(__VA_ARGS__)

enum
{
	Topsec = 1,
	Topname,
	Topnum,		/* number of top types */

	Stacksize = 16*1024,
};

typedef struct
{
	char *name;
	ulong perm;
	void (*read)(Req *r);
	void (*write)(Req *r);
	File *file;

} Faux;

char *conf = "/n/kfs/conf/remote";	/* configuration file */
char *srvname = "remote";
char *mtpt = "/n/remote";

int dflag;				/* debug flag */

static void confsave(void);

enum {
	Secreclen = sizeof
	"255.255.255.255 encrypt cafebabecafebabecafebabecafebabecafebabe",

	Namreclen = sizeof
	"12345678901234567890 255.255.255.255 255.255.255.255",
};

typedef struct Name {
	struct Name	*next;
	char		*name;
	char		*ipaddr0;
	char		*ipaddr1;
} Name;

static Name **namfindipaddr(char *ipaddr);

typedef struct Security {
	struct Security *next;
	char		*ipaddr;
	char		*enc;
	char		*certhash;
} Security;

static Security *seclist;

// Return &next. If ipaddr is not found next is 0, end of list.
//
static Security **
secfind(char *ipaddr)
{
	Security **n, *next;

	for (n = &seclist; (next = *n); n = &next->next)
		if (strcmp(next->ipaddr, ipaddr) == 0)
			break;
	return n;
}

static void
secfree(Security *s)
{
	if (s) {
		free(s->ipaddr);
		free(s->enc);
		free(s->certhash);
		free(s);
	}
}

static int
secset(char *ipaddr, char *enc, char *certhash)
{
	Security **n, *next, *s;

	if (*secfind(ipaddr)) {
		werrstr("%s already exists", ipaddr);
		return 0;
	}
	for (n = &seclist; (next = *n); n = &next->next)
		if (strcmp(next->ipaddr, ipaddr) > 0)
			break;

	if ((s = mallocz(sizeof *s, 1))
	    && (s->ipaddr = strdup(ipaddr))
	    && (s->enc = strdup(enc))
	    && (s->certhash = strdup(certhash))) {
		s->next = next;
		*n = s;
		return 1;
	}
	secfree(s);
	werrstr("%s malloc failure", ipaddr);
	return 0;
}

static int
secclr(char *ipaddr)
{
	Security **n, *s;
	Name **nn, *np;

	n = secfind(ipaddr);
	
	if (!(s = *n)) {
		werrstr("%s does not exist", ipaddr);
		return 0;
	}
	nn = namfindipaddr(ipaddr);

	if ((np = *nn)) {
		werrstr("remove failed, %s is used for remote %s",
			ipaddr, np->name);
		return 0;
	}
	*n = s->next;
	secfree(s);
	return 1;
}

static int
secwriteline(char *line)
{
	int n, l;
	char *f[4];
	uchar hash[SHA1dlen];

	n = getfields(line, f, nelem(f), 1, " \n");
	if (!(n == 3 || n == 1)) {
		werrstr("command has wrong number of arguments: %s", line);
		return 0;
	}
	if (legalip(f[0]) < 0) {
		werrstr("%s bad IP address", f[0]);
		return 0;
	}
	if (n == 1) {
		return secclr(f[0]);
	}
	if (strcmp(f[1], "null") && strcmp(f[1], "encrypt")) {
		werrstr("%s bad encrypt value %s", f[0], f[1]);
		return 0;
	}
	l = strlen(f[2]);
	if (l != 2 * sizeof hash) {
		werrstr("bad hash length %d instead of %d", l, 2 * sizeof hash);
		return 0;
	}
	if (dec16(hash, sizeof hash, f[2], l)
	    != sizeof hash) {
		werrstr("%s incorrect hash value %s", f[0], f[2]);
		return 0;
	}
	return secset(f[0], f[1], f[2]);
}

static void
secwrite(Req *r)
{
	char *reqs, *line, *nextreq;

 	r->ofcall.count = r->ifcall.count;

	if (!(reqs = malloc(r->ifcall.count + 1))) {
		respond(r, "malloc failure");
		return;
	}
	memcpy(reqs, r->ifcall.data, r->ifcall.count);
	reqs[r->ifcall.count] = '\0';
	nextreq = reqs;

	while ((line = strtok(nextreq, "\n"))) {
		nextreq += (strlen(line) + 1);
		if (secwriteline(line)) {
			confsave();
		} else {
			free(reqs);
			responderror(r);
			return;
		}
	}
	free(reqs);
	respond(r, nil);
}

static Name *namlist;

// Return &next. If ipaddr is not found next is 0, end of list.
//
static Name **
namfind(char *name)
{
	Name **n, *next;

	for (n = &namlist; (next = *n); n = &next->next)
		if (strcmp(next->name, name) == 0)
			break;
	return n;
}

// Return &next. If ipaddr is not found next is 0, end of list.
//
static Name **
namfindipaddr(char *ipaddr)
{
	Name **n, *next;

	for (n = &namlist; (next = *n); n = &next->next)
		if (strcmp(next->ipaddr0, ipaddr) == 0
		    || (next->ipaddr1 && strcmp(next->ipaddr1, ipaddr) == 0))
			break;
	return n;
}

static void
namfree(Name *n)
{
	if (n) {
		free(n->name);
		free(n->ipaddr0);
		free(n->ipaddr1);
		free(n);
	}
}

static int
namipaddrok(char *ipaddr)
{
	Name **n, *next;

	if (*(n = namfindipaddr(ipaddr))) {
		next = *n;
		werrstr("%s already used for %s", ipaddr, next->name);
		return 0;
	}
	if (!*secfind(ipaddr)) {
		werrstr("%s has no security set", ipaddr);
		return 0;
	}
	return 1;
}

static int
namset(char *name, char *ipaddr0, char *ipaddr1)
{
	Name **n, *next, *e;

	if (strcmp(name, "unset") == 0) {
		werrstr("%s not allowed", name);
		return 0;
	}
	if (*namfind(name)) {
		werrstr("%s already exists", name);
		return 0;
	}
	if (!namipaddrok(ipaddr0)) {
		return 0;
	}
	if (ipaddr1 && !namipaddrok(ipaddr1)) {
		return 0;
	}
	for (n = &namlist; (next = *n); n = &next->next)
		if (strcmp(next->name, name) > 0)
			break;

	if ((e = mallocz(sizeof *e, 1))
	    && (e->name = strdup(name))
	    && (e->ipaddr0 = strdup(ipaddr0))
	    && (!ipaddr1 || (e->ipaddr1 = strdup(ipaddr1)))) {
		e->next = next;
		*n = e;
		return 1;
	}
	namfree(e);
	werrstr("%s malloc failure", name);
	return 0;
}

static int
namclr(char *name)
{
	int fd;
	Name **n, *e;

	n = namfind(name);
	
	if (!(e = *n)) {
		werrstr("%s does not exist", name);
		return 0;
	}
	fd = open("/n/xlate/ctl", OWRITE);
	if (fd < 0)
		return 0;
	if (fprint(fd, "isnotremote %s", name) < 0)
		return 0;
	*n = e->next;
	namfree(e);
	return 1;
}

static int
namwriteline(char *line)
{
	int n;
	char *f[4];

	n = getfields(line, f, nelem(f), 1, " \n");
	if (n < 1 || n > 3) {
		werrstr("command has wrong number of arguments: %s", line);
		return 0;
	}
	if (strlen(f[0]) > 20) {
		werrstr("%s too long", f[0]);
		return 0;
	}
	if (n == 1) {
		return namclr(f[0]);
	}
	if (legalip(f[1]) < 0) {
		werrstr("%s bad IP address", f[1]);
		return 0;
	}
	if (n == 3) {
		if (legalip(f[2]) < 0) {
			werrstr("%s bad IP address", f[2]);
			return 0;
		}
		if (strcmp(f[1], f[2]) == 0) {
			werrstr("%s can not be both addresses", f[2]);
			return 0;
		}
	}
	else
		f[2] = nil;
	return namset(f[0], f[1], f[2]);
}

static void
namewrite(Req *r)
{
	char *reqs, *line, *nextreq;

 	r->ofcall.count = r->ifcall.count;

	if (!(reqs = malloc(r->ifcall.count + 1))) {
		respond(r, "malloc failure");
		return;
	}
	memcpy(reqs, r->ifcall.data, r->ifcall.count);
	reqs[r->ifcall.count] = '\0';
	nextreq = reqs;

	while ((line = strtok(nextreq, "\n"))) {
		nextreq += (strlen(line) + 1);
		if (namwriteline(line)) {
			confsave();
		} else {
			free(reqs);
			responderror(r);
			return;
		}
	}
	free(reqs);
	respond(r, nil);
}

typedef struct
{
	int reclen;
	int recoff;
	int recnum;
	char *start;
	char *end;
} Frec;

static int
frecinit(Req *r, Frec *f, int reclen)
{
	f->reclen = reclen;
	r->ofcall.count = 0;

	if (r->ifcall.offset % reclen) {
		respond(r, "offset not at record boundary");
		return 0;
	}
	f->recoff = r->ifcall.offset / reclen;
	f->recnum = r->ifcall.count / reclen;

	if (f->recnum < 1) {
		respond(r, "count smaller than a record");
		return 0;
	}
	f->start = r->ofcall.data;
	f->end = f->start + r->ifcall.count;
	return 1;
}

static int
frec(Req *r, Frec *f, char *s)
{
	*s++ = '\n'; // replace \0 with \n
	f->start = s;
	r->ofcall.count += f->reclen;
	if (--f->recnum <= 0)
		return 0;
	return 1;
}

static void
nameread(Req *r)
{
	Frec f;
	int n;
	Name *l;

	if (!frecinit(r, &f, Namreclen))
		return;

	for (n = 0, l = namlist; l; ++n, l = l->next)
		if (n >= f.recoff
		    && !frec(r, &f,
			     seprint(f.start, f.end, "%20s %15s %15s",
				     l->name, l->ipaddr0,
				     l->ipaddr1 ? l->ipaddr1 : "")))
			break;
	respond(r, nil);
}

static void
secread(Req *r)
{
	Frec f;
	int n;
	Security *s;

	if (!frecinit(r, &f, Secreclen))
		return;

	for (n = 0, s = seclist; s; ++n, s = s->next)
		if (n >= f.recoff
		    && !frec(r, &f,
			     seprint(f.start, f.end, "%15s %7s %40s",
				     s->ipaddr, s->enc, s->certhash)))
			break;
	respond(r, nil);
}

static void
fsread(Req *r)
{
	Faux *f;

	f = r->fid->file->aux;
	if (f->read)
		f->read(r);
	else
		respond(r, "unreadable");
}

static void
fswrite(Req *r)
{
	Faux *f;

	f = r->fid->file->aux;
	if (f->write) {
		halog(LOGCOM "remote/%s %.*s\n", f->name,
		      endspace(r->ifcall.data, r->ifcall.count),
		      r->ifcall.data);
		f->write(r);
		if (r->error)
			halog(LOGCOM "remote/%s error %s\n", f->name, r->error);
	} else
		respond(r, "unwritable");
}

static Srv fs = {
	.read = fsread,
	.write = fswrite,
};

static Faux toptbl[Topnum] = {
	[Topname]	{ "name",     0666, nameread, namewrite },
	[Topsec]	{ "security", 0666, secread,  secwrite },
};

static int confloading;

static void
confload(void)
{
	char *s;
	Biobuf bb, *b;
	int fd;

	fd = haopenconfig(conf);
	if (fd < 0)
		return;
	b = &bb;

	if (Binit(b, fd, OREAD) < 0) {
		close(fd);
		halog("remotefs: Binit error: %r\n");
		return;
	}
	confloading = 1;

	for (; s = Brdstr(b, '\n', 1); free(s)) {
		switch (*s) {
		case 's':
			if (!secwriteline(s + 1))
				halog("error %r loading: %s", s + 1);
			break;
		case 'n':
			if (!namwriteline(s + 1))
				halog("error %r loading: %s", s + 1);
			break;
		}
	}
	Bterm(b);
	confloading = 0;
	close(fd);
	return;
}

static char cfgbuf[16*1024];

static void
confsave(void)
{
	char *p, *ep;
	Security *s;
	Name *n;
	
	if (confloading)
		return;

	p = cfgbuf;
	ep = p + sizeof cfgbuf;

	for (s = seclist; s; s = s->next)
		p = seprint(p, ep, "s %s %s %s\n",
			    s->ipaddr, s->enc, s->certhash);
	for (n = namlist; n; n = n->next)
		p = seprint(p, ep, "n %s %s %s\n", n->name, n->ipaddr0,
			    n->ipaddr1 ? n->ipaddr1 : "");
	if (p == ep)
		halog("remotefs: config save buffer too small error\n");

	hawriteconfig(conf, -1, cfgbuf, p - cfgbuf, 0);
}

void
halog(char *fmt, ...)
{
	static int fd = -1;
	va_list arg;

	if (fd < 0)
		fd = open("/dev/syslog", OWRITE);
	va_start(arg, fmt);
	if (vfprint(fd, fmt, arg) <= 0)
		vfprint(1, fmt, arg);	// at least try to be visible
	va_end(arg);
}

static void
chmodrw(char *path)
{
	Dir *d;

	d = dirstat(path);
	if (!d)
		sysfatal("%r");

	d->mode |= 0666;
	dirwstat(path, d);
	free(d);
}

int
createfs(void)
{
	unsigned n;
	char *srvpath;

	fs.tree = alloctree(nil, nil, DMDIR|0555, nil);

	for (n = 1; n < Topnum; ++n) {
		if ((toptbl[n].file = createfile(fs.tree->root, toptbl[n].name, 
		     nil, toptbl[n].perm, toptbl + n)) == nil)
			return -1;

		closefile(toptbl[n].file);
	}
	srvpath = smprint("/srv/%s", srvname);
	remove(srvpath);
	postmountsrv(&fs, srvname, mtpt, MREPL);
	chmodrw(srvpath);
	free(srvpath);
	return 0;
}
	
void
usage(void)
{
	fprint(2, "usage: %s [-c conf] [-s srvname] [-d] [-m mount]\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	ARGBEGIN {
	case 'c':
		conf = EARGF(usage());
		break;
	case 'd':
		dflag++;
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND;

	quotefmtinstall();

	confload();
	if (createfs() < 0)
		sysfatal("%r");
	exits(nil);
}
