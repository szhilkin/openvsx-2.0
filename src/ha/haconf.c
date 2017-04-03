#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <ctype.h>
#include <libcutil.h>
#include <bio.h>

char *conf = "/n/kfs/conf/ha";
int role;
int state;
char *ip;

int dflag;

void
nowrite(Req *rq)
{
	respond(rq, "write not supported");
}

void
noread(Req *rq)
{
	respond(rq, "read not supported");
}

enum
{
	HAmyrole,
	HAmystate,
	HAremote,
	HAnum,		/* number of top types */

	Unset	= 0,
	Pri,
	Sec,	

	Active	= 1,
	Inactive,
	Nline = 8192,
};

char *roles[] = {
	[Unset]	"unset",
	[Pri]	"primary",
	[Sec]	"secondary",
};

char *states[] = {
	[Unset]		"unset",
	[Active]	"active",
	[Inactive]	"inactive",
};

static void
hasyslog(char *fmt, ...)
{
	char buf[Nline], *p, *e;
	va_list arg;
	static int fd = -1;

	p = buf;
	e = p + sizeof buf;
	va_start(arg, fmt);
	vseprint(p, e, fmt, arg);
	va_end(arg);
	if (fd == -1)
		fd = open("/dev/syslog", OWRITE);
	write(fd, buf, strlen(buf));
}

static int
legalel(char *eladdr)
{
        int i, n;

        n = strlen(eladdr);
        if (n != 16) {
error:		werrstr("%s has incorrect form [5100nnnnnnnnnnnn]", eladdr);
        	return -1;
	}
	if (strncmp(eladdr, "5100", 4) != 0)
		goto error;
	for (i=4; i<n; i++)
		if (!isxdigit(eladdr[i]))
			goto error;
	return 0;	
}

int
rmall(char *dir)
{
	char *b;
	int i, n, ret;
	Dir *dp;

	n = numfiles(dir, &dp);
	if (n < 0)
		return -1;
	for (i = 0, ret = 0; 
		 i < n && ret >= 0 && (b = smprint("%s/%s", dir, dp[i].name)); 
		 i++, free(b)) {
		if (dp[i].mode & DMDIR)
			ret = rmall(b);
		else
			ret = remove(b);
	}
	free(dp);
	return remove(dir);
}

int
hacmd(char *cmd, char *buf, int buflen, char *name)
{
	char *b;
	int fd, n;

	if (name)
		b = smprint("/srv/ha.%s.cmd", name);
	else
		b = smprint("/srv/ha.cmd");
	if (b == nil) {
		werrstr("smprint failed %r");
		return -1;
	}
	fd = open(b, ORDWR);
	free(b);
	if (fd < 0) {
		werrstr("could not open srv file %r");
		return -1;
	}
	if (write(fd, cmd, strlen(cmd)) != strlen(cmd)) {
		werrstr("hacmd: error writing %s: %r", cmd);
		close(fd);
		return -1;
	}
	if (buf) {
		n = read(fd, buf, buflen - 1);
		if (n < 0) {
			werrstr("hacmd: error executing %s: %r", cmd);
			close(fd);
			return -1;
		}
		buf[n] = '\0';
	}
	close(fd);
	return 0;
}

void
rdrole(Req *rq)
{
	readstr(rq, roles[role]);
	respond(rq, nil);
}

int
getstate(void)
{
	char buf[100];

	if (readfile(buf, sizeof buf, "%s/state", conf) < 0)
		return Unset;
	else
		return strncmp(buf, states[Active], strlen(states[Active])) == 0 ? Active : Inactive;
}

int
myeladdr(char *addr)
{
	char buf[100];

	if (readfile(buf, sizeof buf, "/net/el/addr") < 0)
		return -1;
	return strcmp(addr, buf) ? 0 : -1;
}

int
istarg(char *addr)
{
	char *line, *args[6];
	int n;
	Biobuf *bp;

	bp = Bopen("/n/nsd/targets", OREAD);
	if(bp == nil) {
		werrstr("could not open targets file: %r");
		return -1;
	}
	for (; (line = Brdstr(bp, '\n', 1)); free(line)) {
		n = gettokens(line, args, 6, ":");
		if (n < 5)
			continue;
		if (strcmp(addr, args[0]) == 0)
			break;
	}
	Bterm(bp);
	if (line == nil) {
		werrstr("SAN address %s not found", addr);
		return -1;
	}
	if ((atoi(args[2]) & 1) == 0) {
		free(line);
		werrstr("VSX %s not available", addr);
		return -1;
	}
	n = strncmp("VSX", args[4], 3);
	free(line);
	if (n)
		werrstr("SAN address %s is not a VSX", addr);
	return n;
}

int
roleusage(Cmdbuf *cb)
{
	if (cb->nf != 2) {
u:		werrstr("usage: %s { new | retain } | %s SANaddress", roles[Pri], roles[Sec]);
		return -1;
	}
	if (strcmp(roles[Pri], cb->f[0]) == 0) {
		if (strcmp("new", cb->f[1]) && strcmp("retain", cb->f[1]))
			goto u;
		return Pri;
	} else if (strcmp(roles[Sec], cb->f[0]) == 0)
		return Sec;
	else
		goto u;
}

int
setprimaryrole(char *opt)
{
	char buf[255], *b, *c;
	char *confs[] = {"xlate", "rr", "remote"};
	int i, j, isxl, n;
	Dir *dp;

	if (hacmd("godown", buf, sizeof buf, nil) < 0)
		return -1;
	print("Waiting for VSX core service to shut down ...\n");
	for (isxl = 1, i = 0; isxl && i < 100; i++, sleep(1000)) {
		n = numfiles("/proc", &dp);
		if (n < 0)
			break;
		isxl = 0;
		for (j = 0; j < n; j++) {
			if (readfile(buf, sizeof buf, "/proc/%s/status", dp[j].name) < 0)
				continue;
			if (strncmp("xlate", buf, 5) == 0) {
				isxl = 1;
				break;
			}
		}
		free(dp);
	}
	if (isxl) {
		werrstr("core not shutting down");
		return -1;
	}
	for (i = 0; i < nelem(confs) && 
		(b = smprint("%s/%s/%s", conf, ip, confs[i])) &&
		(c = smprint("/n/kfs/conf/%s", confs[i])); 
		i++, free(b), free(c)) {
		if (strcmp(opt, "new") == 0)
			remove(c);
		else {
			unmount(b, c);
			copy(b, c);
		}
	}
	b = smprint("%s/%s", conf, ip);
	rmall(b);
	free(b);
	snprint(buf, sizeof buf, "%s/peerea", conf);
	writefile(buf, "\n");
	snprint(buf, sizeof buf, "%s/role", conf);
	return writefile(buf, "%s", roles[Pri]);
}

int
setsecondaryrole(char *ip)
{
	char buf[255];

	snprint(buf, sizeof buf, "%s/role", conf);
	if (writefile(buf, "%s %s", roles[Sec], ip) < 0)
		return -1;
	snprint(buf, sizeof buf, "%s/peerea", conf);
	writefile(buf, "\n");
	snprint(buf, sizeof buf, "%s/state", conf);
	return writefile(buf, "%s", states[Inactive]);
}

int
chgsecondaryaddr(char *ip)
{
	char msg[100];

	snprint(msg, sizeof msg, "addrchg %s", ip);
	if (hacmd(msg, msg, sizeof msg, nil) < 0)
		return -1;
	return msg[0] != '+' ? -1 : 0;
}

void
haresponderror(Req *rq)
{
	char e[ERRMAX], *p;

	rerrstr(e, ERRMAX);
	p = strstr(e, "-ERR ");
	if (p) {
		p = strchr(p, ' ');
		werrstr(++p);
	}
	responderror(rq);
}

void
wrrole(Req *rq)
{
	int n, newrole;
	Cmdbuf *cb;
	char myrole[100];

	if (readfile(myrole, sizeof myrole, "%s/role", conf) < 0)
		strcpy(myrole, roles[Unset]);
 	rq->ofcall.count = rq->ifcall.count;
	cb = parsecmd(rq->ifcall.data, rq->ifcall.count);

	newrole = roleusage(cb);
	switch(newrole) {
	case Pri:
		if (role == Pri) {
			n = -1;
			werrstr("%s role already set", roles[Pri]);
			break;
		}
		if ((n = setprimaryrole(cb->f[1])) < 0)
			break;
reboot:		hasyslog("HA %s Node. Role set to %s for next boot, rebooting\n",
			roles[role], roles[newrole]);
		writefile("/n/sys/ctl", "reboot");
		break;
	case Sec:
		if ((n = legalel(cb->f[1])) < 0)
			break;
		if ((n = myeladdr(cb->f[1])) < 0) {
			werrstr("cannot set secondary address to itself\n");
			break;
		}
		if ((n = istarg(cb->f[1])))
			break;
		if (role == Pri) {
			if ((n = setsecondaryrole(cb->f[1])) < 0)
				break;
			goto reboot;
		} else
			n = chgsecondaryaddr(cb->f[1]);
		break;
	default:
		n = -1;
		break;
	}
	if (n < 0)
		haresponderror(rq);
	else
		respond(rq, nil);
	free(cb);
}

void
rdstate(Req *rq)
{
	readstr(rq, states[getstate()]);
	respond(rq, nil);
}

int
ckstate(Cmdbuf *cb)
{
	int ret;

	if (cb->nf != 1) {
		werrstr("usage: %s | %s", states[Active], states[Inactive]);
		return -1;
	}
	if (strcmp(cb->f[0], states[Active]) == 0)
		ret = Active;
	else if (strcmp(cb->f[0], states[Inactive]) == 0)
		ret = Inactive;
	else {
		werrstr("%s is not a valid parameter, { %s | %s }", cb->f[0], states[Active], states[Inactive]);
		return -1;
	}
	if (ret == getstate()) {
		werrstr("HA state is already %s", states[ret]);
		return -1;
	}
	return ret;
}

int
setstate(int s)
{
	char buf[Nline], *b, cmd[100];

	print("Attempting to set self %s\n", states[s]);
	snprint(cmd, sizeof cmd, "%s user", s == Active ? "goact" : "goinact");
	if (hacmd(cmd, buf, sizeof buf, nil) < 0)
		return -1;
	if (strstr(buf, "-ERR")) {
		werrstr("state changed failed %s", buf);
		return -1;
	}
	print("State changed to %s\n", states[s]);
	b = smprint("%s/state", conf);
	writefile(b, "%s", states[s]);
	free(b);
	writefile("#ec/hastate", "%s", states[s]);
	state = s;
	return 0;
}

void
wrstate(Req *rq)
{
	int n;
	Cmdbuf *cb;

 	rq->ofcall.count = rq->ifcall.count;
	cb = parsecmd(rq->ifcall.data, rq->ifcall.count);

	n = ckstate(cb);
	if (n > Unset && setstate(n) == 0)
		respond(rq, nil);
	else
		haresponderror(rq);
	free(cb);
}

void
rdremote(Req *rq)
{
	char buf[Nline], *args[7], *b, *p;

	if (hacmd("status", buf, sizeof buf, nil) < 0) {
		haresponderror(rq);
		return;
	}
	if ((p = strstr(buf, "OK")) == nil) {
		respond(rq, "unknown status");
		return;
	}
	if (tokenize(p, args, 7) != 6) {
		respond(rq, "unrecognized file format");
		return;
	}
	b = smprint("%s %s %s", args[3], args[4], args[5]);
	readstr(rq, b);
	free(b);
	respond(rq, nil);
}

static struct
{
	char *name;
	ulong perm;
	void  (*read)(Req *);
	void  (*write)(Req *);
	File *file;
} toptbl[HAnum] = {
	[HAmyrole]	{ "role",	0666,	rdrole,		wrrole	},
	[HAmystate]	{ "state",	0666,	rdstate,	wrstate	},
	[HAremote]	{ "remote",	0444,	rdremote,	nowrite	},
};

typedef ulong Qidunique;

/*
 * intro(5): The path is an integer unique among all files in the
 * hierarchy.
 */
typedef struct
{
	Qidunique	unique;
	ulong		top;
} Qidpath;

static Qidpath *
getqidpath(Qid *q)
{
	return (Qidpath *)&q->path;
}

static void
setqidpath(Qid *q, uchar top)
{
	static Qidunique unique = 0;
	Qidpath *p;

	q->path = 0;
	p = getqidpath(q);
	p->unique = ++unique;
	p->top = top;
}

void
fswrite(Req *r)
{
	Qidpath *p;

	p = getqidpath(&r->fid->qid);

	if (p->top >= HAnum)
		respond(r, "write not supported");
	else {
		hasyslog(LOGCOM "ha/%s %.*s\n", toptbl[p->top].name,
			 endspace(r->ifcall.data, r->ifcall.count),
			 r->ifcall.data);
		toptbl[p->top].write(r);
		if (r->error)
			hasyslog(LOGCOM "ha/%s error %r\n",
				 toptbl[p->top].name);
	}
}

void
fsread(Req *r)
{
	Qidpath *p;

	p = getqidpath(&r->fid->qid);

	if (p->top >= HAnum)
		respond(r, "read not supported");
	else
		toptbl[p->top].read(r);
}

static Srv fs = {
	.read = fsread,
	.write = fswrite,
};

void
loadparams(void)
{
	char buf[100], *p;

	if (readfile(buf, sizeof buf, "%s/role", conf) < 0)
		role = Unset;
	else {
		if (strncmp(buf, roles[Pri], strlen(roles[Pri])) == 0)
			role = Pri;
		else {
			role = Sec;
			p = strchr(buf, ' ') + 1;
			ip = strdup(p);
		}
	}
}

static char *srvname = "haconf";

int
createhaconf(void)
{
	unsigned n;
	char *srvpath;

	fs.tree = alloctree(nil, nil, DMDIR|0555, nil);

	for (n = 0; n < HAnum; ++n)
		if ((toptbl[n].file
		     = createfile(fs.tree->root, toptbl[n].name,
				  nil, toptbl[n].perm, nil)) == nil)
			return -1;
		else {
			setqidpath(&toptbl[n].file->qid, n);
			closefile(toptbl[n].file);
		}

	srvpath = smprint("/srv/%s", srvname);
	remove(srvpath);
	free(srvpath);

	postmountsrv(&fs, srvname, nil, MREPL);
	return 0;
}

static void
usage(void)
{
	fprint(2, "usage: %s [-c conf] [-d] [-s srvname]\n", argv0);
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
		srvname = ARGF();
		break;
	} ARGEND;

	quotefmtinstall();

	if (createhaconf() < 0)
		sysfatal("%r");

	loadparams();
}
