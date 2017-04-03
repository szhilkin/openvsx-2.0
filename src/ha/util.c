#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <ip.h>
#include <libcutil.h>
#include "dat.h"
#include "fns.h"
#include "alib.h"


enum	{
	Nmyea = 32,		/* one for each port we're using */
	Nsanports = Nmyea,
};

static char *cmd_execv(int, int , char **);

void
exitsall(char *s)
{
	char buf[64];
	int fd;
	
	snprint(buf, sizeof buf, "/proc/%d/notepg", getpid());
	fd = open(buf, OWRITE);
	write(fd, s, strlen(s));
	close(fd);
	exits(s);
}

void
killapp(int cmdpid)	/* shutdown command being watched */
{
	if (cmdpid == 0)
		return;
	postnote(PNGROUP, cmdpid, "shutdown");
}

void
error(char *s)
{

	fprint(2, "%s: %s: %r\n", argv0, s);
	exitsall(s);
}

char *
basename(char *s)
{
	char *cp;
	
	cp = strrchr(s, '/');
	if (cp == nil)
		return s;
	return cp+1;
}

int
srvfd(char *s, int mode, int sfd)
{
	int fd;
	char buf[32];

	fd = create(s, ORCLOSE|OWRITE, mode);
	if(fd < 0){
		remove(s);
		fd = create(s, ORCLOSE|OWRITE, mode);
		if (fd < 0) {
			csyslog("srvfd: create failed: %s: %r\n", s);
			exits("srvfd");
		}
	}
	sprint(buf, "%d", sfd);
	if(write(fd, buf, strlen(buf)) != strlen(buf)) {
		csyslog("srv write\n");
		exits("srvfd");
	}
	return sfd;
}

void
setprompt(void)
{
	int fd;

	fd = create("#ec/cliprompt", OWRITE, 0666);
	if (fd < 0) {
		csyslog("warning: cannot set prompt to inactive\n");
		return;
	}
	fprint(fd, "VSX shelf inactive> ");
	close(fd);
	remove("#ec/shelf");
}

void
halog(char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	csyslog(fmt, arg);
	va_end(arg);
}

void
csyslog(char *fmt, ...)
{
	char buf[Nline], *p, *e;
	va_list arg;
	static int fd = -1;

	p = buf;
	e = p + sizeof buf;
	p = seprint(p, e, "HA %s Node ", strcmp(myrole, "secondary") ? "Primary" : "Secondary");
	va_start(arg, fmt);
	vseprint(p, e, fmt, arg);
	va_end(arg);
	if(debug)
		fprint(2, "%s\n", buf);
	if (fd == -1)
		fd = open("/dev/syslog", OWRITE);
	write(fd, buf, strlen(buf));
}

int
getcrnlto(User *u, char *buf, int n)
{
	int res;

	if(debug)
		fprint(2, "Setting read alarm: %d\n", timeout);
	alarm(timeout);
	res = getcrnl(u, buf, n);
	alarm(0);
	return res;
}

/*
 *  get a line that ends in crnl or cr, turn terminating crnl into a nl
 *
 *  return 0 on EOF
 */
int
getcrnl(User *u, char *buf, int n)
{
	int c;
	char *ep;
	char *bp;
	Biobuf *fp = &u->in;

	Bflush(&u->out);
	bp = buf;
	ep = bp + n - 1;
	while(bp != ep){
		c = Bgetc(fp);
		switch(c){
		case Beof:
			if(debug)
				fprint(2, "Got alarm/Bgetc failure: %r\n");
			*bp = 0;
			if(bp==buf)
				return -1;
			else
				return bp-buf;
		case '\r':
			c = Bgetc(fp);
			if(c == '\n'){
				*bp = 0;
				if (debug)
					fprint(2, "<- %s\n", buf);
				return bp-buf;
			}
			Bungetc(fp);
			c = '\r';
			break;
		case '\n':
			*bp = 0;
			if (debug)
				fprint(2, "<- %s\n", buf);
			return bp-buf;
		}
		*bp++ = c;
	}
	*bp = 0;
	if (debug)
		fprint(2, "<- %s\n", buf);
	return bp-buf;
}

char *
killws(char *p)
{
	char *rp;

	while (*p && isspace(*p))
		p++;
	rp = p;
	p += strlen(p) - 1;
	while (p > rp && isspace(*p))
		*p-- = 0;
	return rp;
}

void
sendcrnl(Biobuf *out, char *fmt, ...)
{
	char buf[Nline];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf+sizeof(buf), fmt, arg);
	va_end(arg);
	if(debug)
		fprint(2, "-> %s\n", buf);
	Bprint(out, "%s\r\n", buf);
}

int
sendok(char *fmt, ...)
{
	char buf[Nline];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf+sizeof(buf), fmt, arg);
	va_end(arg);
	if(*buf){
		if(debug)
			fprint(2, "-> +OK %s\n", buf);
		Bprint(&u->out, "+OK %s\r\n", buf);
	} else {
		if(debug)
			fprint(2, "-> +OK\n");
		Bprint(&u->out, "+OK\r\n");
	}
	return 0;
}

int
senderr(Biobuf *out, char *fmt, ...)
{
	char buf[Nline];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf+sizeof(buf), fmt, arg);
	va_end(arg);
	if(debug)
		fprint(2, "-> -ERR %s\n", buf);
	Bprint(out, "-ERR %s\r\n", buf);
	return -1;
}

int
checkrole(char **prim)
{
	char *p;
	int fd, n;
	char buf[64];

	snprint(buf, sizeof buf, "%s/role", rootdir);
	fd = open(buf, OREAD);
	if(fd < 0){
		fprint(2, "Can't open role, assuming primary\n");
		return 1;
	}
	n = read(fd, buf, 64);
	close(fd);
	buf[n] = '\0';
	if(strncmp("primary", buf, 7) == 0)
		return 1;
	p = strchr(buf, ' ');
	if(p && *prim)
		*prim = killws(strdup(p+1));
	if(p && u->raddr == nil)
		u->raddr = killws(strdup(p+1));
	return 0;
}

void
cmd(void)
{
	char buf[Nline], *res;
	int p[2];
	int cmdfd, n;

	if (pipe(p) < 0) { 
		csyslog("cmd: pipes do not work: %r\n");
		exits("pipe");
	}
	sprint(buf, "#s/ha.cmd");
	srvfd(buf, 0666, p[0]);
	close(p[0]);
	cmdfd = p[1];
	u->fd = cmdfd;
	for (;;) {
		n = read(cmdfd, buf, sizeof buf - 1);
		if (n <= 0)
			sleep(10000);
		if (n > 0) {
			buf[n] = 0;
			res = cmd_exec(0, buf);
			if(res)
				fprint(cmdfd, "%s", res);
			else
				fprint(cmdfd, "+OK\r\n");
		}
	}
}

char *
cmd_exec(int src, char *arg)
{
	char line[256];
	char *argv[30];
	int argc;

	if(strlen(arg) >= nelem(line)-2) 
		return 0;
	strcpy(line, arg);
	argc = tokenize(line, argv, nelem(argv)-1);
	if (argc <= 0)
		return 0;
	argv[argc] = nil;
	return cmd_execv(src, argc, argv);
}

static char *
cmd_execv(int src, int argc, char **argv)
{
	Cmd *p;

	for(p = cmdtab; p->name; p++)
		if(strcmp(*argv, p->name) == 0 && (src && (!p->needauth || u->loggedin) || !src && p->hacmd))
			return (*p->f)(argc, argv);
	return "-ERR unknown command";
}

char *
eladdrcmd(int, char **)
{
	char addr[100];
	static char buf[Nline];

	if (readfile(addr, sizeof addr, "/net/el/addr") < 0)
		return "-ERR unknown\r\n";
	snprint(buf, sizeof buf, "+OK %s\r\n", addr);
	return buf;
}

char *
versioncmd(int, char **)
{
	char *myver;
	static char buf[Nline];

	myver = getenv("release");
	if(myver == nil)
		return "-ERR no release\r\n";
	snprint(buf, sizeof buf, "+OK %s\r\n", myver);
	free(myver);
	return buf;
}

char *
helpcmd(int, char**)
{
	Cmd *p;
	char *q, *e;
	static char buf[Nline];
	
	e = buf + Nline;
	for (p = cmdtab, q = buf; p->name; p++)
		q = seprint(q, e, " %s\r\n", p->name);
	seprint(q, e, "+OK\r\n");
	return buf;
}
	
char *
echocmd(int argc, char **argv)
{
	char *q, *e;
	int i;
	static char buf[Nline];

	e = buf + Nline;
	q = seprint(buf, e, "+OK ");
	for (i = 1; i < argc; i++)
		q = seprint(q, e, "%s%s", argv[i], i == argc-1 ? "\r\n" : " ");
	return buf;
}

char *
statuscmd(int , char **)
{
	char *p, *e, *st, *ras, *rst;
	static char buf[Nline];
	char addr[32];

	if(p = strchr(u->raddr, '!')) {
		++p;
		e = strchr(p, '!');
		strcpy(addr, p);
		addr[e-p] = '\0';
	}
	else
		strcpy(addr, u->raddr);
	st = getstate();
	p = buf;
	e = p + sizeof buf;
	p = seprint(p, e, "%s\r\n", st);	// compatibility with 1.0

	if (strcmp(addr, "none") == 0)
		ras = "N/A";
	else
		ras = u->loggedin ? "connected" : "disconnected";

	if (strcmp(addr, "none") == 0)
		rst = "N/A";
	else if (u->loggedin == 0 || u->rmtactive < 0)
		rst = "unknown";
	else
		rst = u->rmtactive ? "active" : "inactive";

	seprint(p, e, "+OK %s %s %s %s %s\r\n",
		myrole,
		st,
		addr, 
		ras,
		rst);
	free(st);
	return buf;
}

static int
haportadd(char *buf, int len, char *name)
{
	uchar ea[6];
	char *p, *ep;

	p = buf;
	ep = buf + len;
	if (myetheraddr(ea, name) == 0)
		p = seprint(p, ep, "%E\n", ea);
	return p - buf;
}

static void
enumports(char *p, int len)
{
	int i, n;
	char buf[32];

	for (i=2; i<Nsanports && len>0; i++) {
		snprint(buf, sizeof buf, "ether%d", i);
		n = haportadd(p, len, buf);
		p += n;
		len -= n;
	}
}

int
receiveaddrs(User *u, int active)
{
	char line[Nline], addrbuf[Nline];
	char *p, *ep;

	p = addrbuf;
	ep = p + sizeof addrbuf;
	while(p < ep) {
		if (getcrnlto(u, line, sizeof line) == -1)
			break;
		if (line[0] == '-') {
			csyslog("receiveaddrs failed: %s\n", line);
			return 0;
		}
		if (line[0] == '.' && line[1] == 0)
			break;
		p = seprint(p, ep, "%s\n", line);
	}
	*(--p) = 0; // remove last newline
	writefile(peereafile, "%s", addrbuf);
	if (active)
		writefile("/n/xlate/targ/peerea", "%s", addrbuf);
	return 1;
}

void
sendaddrs(Biobuf *out)
{
	char buf[Nline];

	enumports(buf, sizeof buf);
	sendcrnl(out, "%s", buf);
	sendcrnl(out, ".");
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
