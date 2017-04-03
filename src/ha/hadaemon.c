#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ip.h>
#include <authsrv.h>
#include <libsec.h>
#include <ctype.h>
#include <libcutil.h>
#include "dat.h"
#include "fns.h"
#include "haconfig.h"

/*
 * ha server.
 * primary runs as a server that allows the secondary
 * to log in and control it.
 
 * this version uses 256-bit sha2 to authenticate the password
 * in the nvram.
 */

enum {
	EL,
	TCP,
};

char *address[] = {
	[EL]	"el!*!"HAport,
	[TCP]	"tcp!*!"HAport,
};

int	hello(void);
char	*goactivecmd(int, char**);
char	*getcmd(int, char**);
char	*putcmd(int, char**);
char	*goinactivecmd(int, char**);
char	*statuscmd(int, char**);
char	*putstatuscmd(int, char**);
char	*sendaddrscmd(int, char**);
char	*recvaddrscmd(int, char**);
char	*listcmd(int, char**);
char	*quitcmd(int, char**);
char	*logincmd(int, char**);
void	halisten(int);
void	handle(int, Conn*);
void	ha(int, Conn*);
char*	remoteaddr(char*);
int	mkpwhash(uchar *, int, char *);
char*	getpfile(char *);
void	putpfile(char*, char*);
int	active(void);
void	setstate(char *);
char *	getstate(void);
void    cppeers(void);
void	clrpeers(void);
char	*byecmd(int, char**);

Cmd cmdtab[] =
{
	"echo", 	0, 	1,	echocmd,
	"version",	0,	1,	versioncmd,
	"failover",	1,	1,	goinactivecmd,
	"login",	0,	0,	logincmd,
	"goact", 	1, 	1,	goactivecmd,
	"goinact",	1,  	1,	goinactivecmd,
	"restorepri",	1,	1,	goactivecmd,
	"status",	1,	1,	statuscmd,
	"putstatus",	1,	1,	putstatuscmd,
	"list",		1,	1,	listcmd,
	"get",		1,	1,	getcmd,
	"put",		1, 	1,	putcmd,
	"sendaddrs",	1,	1,	sendaddrscmd,
	"recvaddrs",	1,	1,	recvaddrscmd,
	"help",		1,	1,	helpcmd,
	"quit", 	0, 	0,	quitcmd,
	"bye",		0,	0,	byecmd,
	"eladdr",	0,	1,	eladdrcmd,
	nil, 0, 0, nil,
};

char *myrole = "primary";
int	debug;
Rendez	ckstate;
QLock	stlk;
int	cmdpid;			/* pid of the command being run */
User useg;
User	*u;			/* points into useg in main */
QLock	prlock;			/* lock the protect state */
char *rootdir;
int	dopeercp;

void
notifyf(void *, char *msg)
{
	if(strncmp(msg, HAactive, strlen(HAactive)) == 0){
		csyslog("commanded active from user\n");
		setstate(HAactive);
		qlock(&stlk);
		rwakeup(&ckstate);
		qunlock(&stlk);
		noted(NCONT);
	}
	if(strncmp(msg, HAinactive, strlen(HAinactive)) == 0){
		csyslog("commanded inactive from user\n");
		setstate(HAinactive);
		killapp(cmdpid);
		noted(NCONT);
	}
	noted(NDFLT);
}

/* the group shall ignore what one doth care */
void
notifyfg(void *, char *msg)
{
	if(strcmp(msg, "alarm") == 0){
		u->loggedin = 0;
		clrpeers();
		csyslog("lost contact with secondary\n");
		noted(NCONT);
	}
	if(strstr(msg, HAactive))
		noted(NCONT);
	noted(NDFLT);
}

void
sleeper(void)
{
	notify(notifyf);
	while(1)
		sleep(60000);
}

void
usage(void)
{
	fprint(2, "usage: %s cmd\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char err[ERRMAX];
	char buf[8192];
	int i, saidit, pfd[2], n;

	/*
	 * isolate myself for group note handling, and since we don't
	 * post a service, return immediately.
	 */
	if (rfork(RFNOTEG|RFPROC))
		exits(0);
	notify(notifyfg);
	u = &useg;
	u->raddr = strdup("none");
	ckstate.l = &stlk;
	fmtinstall('E', eipfmt);
	fmtinstall('I', eipfmt);
	fmtinstall('V', eipfmt);
	fmtinstall('H', encodefmt);
	ARGBEGIN{
	case 'd':
		rootdir = EARGF(usage());
		break;
	case 'D':
		debug++;
		break;
	default:
		usage();
	} ARGEND
	csyslog("started\n");
	if (argc < 1)
		usage();
	if (rootdir == nil)
		usage();
	if (access(*argv, AEXEC) != 0)
		sysfatal("%s not found", *argv);
	if (rfork(RFPROC|RFMEM))
		exits(nil);
	if (rfork(RFPROC|RFNOWAIT|RFMEM) == 0)
		halisten(EL);
	if (rfork(RFPROC|RFNOWAIT|RFMEM) == 0)
		halisten(TCP);
	if (rfork(RFPROC|RFNOWAIT|RFMEM) == 0)
		cmd();
	if(rfork(RFPROC|RFNOWAIT|RFMEM) == 0)
		sleeper();
	saidit = 0;
	*buf = 0;
	for (i = 0; i < argc; i++) {
		strcat(buf, argv[i]);
		strcat(buf, i < argc-1 ? " " : "");
	}
loop:
	if (!active()) {
		setprompt();
		if (saidit++ == 0)
			csyslog("set inactive; VSX CORE NOT RUNNING\n");
		qlock(&stlk);
		rsleep(&ckstate);
		qunlock(&stlk);
		goto loop;
	}
	if (pipe(pfd) < 0)
		error("pipe error");
	saidit = 0;
	switch (cmdpid = rfork(RFPROC|RFFDG|RFNAMEG|RFNOTEG)) {
	case -1:
		error("bad command");
		break;
	default:
		close(pfd[1]);
	again:
		/*
		 * If all descendants inherit the pipe, then reading when no one
		 * writes is basically a wait since we don't return until all are
		 * gone and we get an EOF.
		 */
		n = read(pfd[0], pfd+1, sizeof (int));
		if (n < 0) {
			rerrstr(err, sizeof err);
			if (strstr(err, "interrupt"))
				goto again;
		}
		cmdpid = 0;
		close(pfd[0]);
		break;
	case 0:
		csyslog("STARTING VSX CORE\n");
		exec(*argv, argv);
		sysfatal("%s: %s: %r", argv0, *argv);
	}
	goto loop;
}

int
rerr(char *e)
{
	char buf[64];

	rerrstr(buf, sizeof buf);
	if (strstr(buf, e))
		return 1;
	return 0;
}

void
halisten(int type)	/* serve ha */
{
	int ctl;
	char dir[40];
	Conn *c;

ann:	ctl = announce(address[type], dir);
print("announce %s\n", address[type]);
	if (ctl < 0)
		sysfatal("announce %s: %r", address[type]);
	for (;;) {
		c = mallocz(sizeof *c, 1);
		c->fd = listen(dir, c->dir);
		if (c->fd < 0) {
			free(c);
			if (rerr("announce")) {	// this is a weird one.
				close(ctl);
				goto ann;
			}
			if (!rerr("interrupt"))
				csyslog("listen error: %r\n");
			continue;
		}
		// XXX check to see if the address is on our network.  if not reject the connection
		if (rfork(RFPROC|RFMEM) == 0)
			handle(type, c);
	}
}

void
handle(int type, Conn *c)
{
	int fd;

	fd = accept(c->fd, c->dir);
	if (fd < 0) {
		fprint(2, "accept %s: can't open %s/data: %r", address[type], c->dir);
		free(c);
		exits("handle");
	}
	u->user = getenv("user");
	ha(fd, c);
	close(c->fd);
	free(c);
	exits(nil);
}

void
ha(int fd, Conn *co)	/* serve control to standby system */
{
	NetConnInfo *n;
	char cmdbuf[Nline];
	char *res;

	Binit(&u->in, fd, OREAD);
	Binit(&u->out, fd, OWRITE);
	
	n = getnetconninfo(co->dir, co->fd);
	if (n) {
		u->raddr = strdup(n->raddr);
		u->laddr = strdup(n->laddr);
		csyslog("connection from (%s)\n", n->raddr);
	}

	qlock(&prlock);
	if (u->loggedin) {
		qunlock(&prlock);
		csyslog("rejecting connection from %s; already protected\n",
			n ? n->raddr : "unknown");
		Bprint(&u->out, "-ERR already connected\r\n");
		freenetconninfo(n);
		Bterm(&u->in);
		Bterm(&u->out);
		close(fd);
		return;
	}
	qunlock(&prlock);
	freenetconninfo(n);
	if (hello() == -1) {
		Bflush(&u->out);
		Bterm(&u->in);
		Bterm(&u->out);
		close(fd);
		return;
	}
	
	while (getcrnl(u, cmdbuf, sizeof cmdbuf) > 0) {
		if (*cmdbuf == 0)
			continue;
		res = cmd_exec(1, cmdbuf);
		if(res == nil)
			break;
		Bprint(&u->out, "%s", res);
		Bflush(&u->out);
		if (dopeercp) {
			cppeers();
		}
	}
	qlock(&prlock);
	u->loggedin = 0;
	Bterm(&u->in);
	Bterm(&u->out);
	alarm(0);
	qunlock(&prlock);
	close(fd);
}


int
hello(void)
{
	uchar chal[32];
	
	srand(time(0));
	prng(chal, sizeof chal);
	u->chal = smprint("%.*H", sizeof chal, chal);
	sendok("HA server ready sha2_256 <%s>", u->chal);
	return 0;
}

char *
byecmd(int, char**)
{
	qlock(&prlock);
	clrpeers();
	csyslog("no longer protected by %s\n", u->raddr);
	free(u->raddr);
	u->raddr = strdup("none");
	qunlock(&prlock);
	return nil;
}

char *
quitcmd(int, char**)
{
	/* dummy.  see cmd loop */
	csyslog("%s logged off\n", u->raddr);
	return nil;
}

/*
 * login using the sha2/256 digest of challange string and password
 */
 
static char *
logincmdx(int, char **arg)
{
	char *resp;
	Nvrsafe *nv;
	uchar out[SHA2_256dlen], in[SHA2_256dlen];
	
	if (u->loggedin)
		return "-ERR already logged in\r\n";
	nv = malloc(sizeof *nv);
	readnvram(nv, 0);
	free(nv);
	resp = arg[2];
	if(strcmp(arg[1], u->user) != 0)
		return "-ERR bad user\r\n";
	if(mkpwhash(out, sizeof out, u->chal) < 0)
		return "-ERR can't get password\r\n";
	dec16(in, sizeof in, resp, strlen(resp));
	if(memcmp(out, in, SHA2_256dlen) != 0)
		return "-ERR wrong\r\n";
	u->loggedin++;
	u->rmtactive = -1;
	csyslog("login from %s\n", u->raddr);
	alarm(15000);
	return "+OK\r\n";
}

char *
logincmd(int i, char **arg)
{
	char *ret;

	qlock(&prlock);
	ret = logincmdx(i, arg);
	qunlock(&prlock);
	return ret;
}

char *
putstatuscmd(int, char **arg)
{
	u->rmtactive = !strcmp(arg[1], "active");
	alarm(15000);
	return "+OK\r\n";
}
	
char *
getcmd(int, char**arg)
{
	Biobuf *bp, b;
	char *p;
	int fd;

	if (arg[1] == nil)
		return "-ERR no file specified\r\n";
	fd = haopenconfig(arg[1]);
	bp = &b;
	memset(bp, 0, sizeof b);
	if (Binit(bp, fd, OREAD) < 0) {
		close(fd);
		return "-ERR %r\r\n";
	}
	while(p = Brdline(bp, '\n')) {
		p[Blinelen(bp)-1] = 0;
		sendcrnl(&u->out, "%s", p);
	}
	Bterm(bp);
	close(fd);
	return "+OK\r\n";
}
	
void
setstate(char *s)
{
	putpfile(stateenv, s);
	putpfile(statefile, s);
}

char *
getstate(void)
{
	char *p, *op;

	p = getpfile(stateenv);
	if (p == nil) {
		p = getpfile(statefile);
		if (p == nil)
			sysfatal("can't determine state");
	}
	op = p;
	for (; *p; p++) {
		if (isalpha(*p))
			continue;
		*p = 0;
		break;
	}
	return op;
}

static char cfgbuf[512*1024];
	
char *
putcmd(int, char **arg)
{
	ulong mtime;
	char *file;
	char line[Nline];
	char *p, *ep;

	file = arg[1];
	mtime = atoi(arg[2]);
	sendcrnl(&u->out, "enter data; end with '.'");
	p = cfgbuf;
	ep = p + sizeof cfgbuf;
	while (getcrnlto(u, line, sizeof line) != -1) {
		if (line[0] == '.' && line[1] == 0)
			break;
		p = seprint(p, ep, "%s\n", line);
	}
	/* Force this to be written out, we need it sync'ed with peer */
	if (hawriteconfig(file, mtime, cfgbuf, p - cfgbuf, 1) < 0)
		return "-ERR save failure: %r\r\n";
	if (active())
		killapp(cmdpid);
	return "+OK\r\n";
}

char *
sendaddrscmd(int, char**)
{
	sendaddrs(&u->out);
	return "+OK\r\n";
}

char *
recvaddrscmd(int, char**)
{
	sendcrnl(&u->out, "enter data; end with '.'");
	if (receiveaddrs(u, active()))
		return "+OK\r\n";
	return "-ERR address receive error %r\r\n";
}

int
active(void)
{
	char *p;
	int n;

	p = getstate();
	n = strcmp(p, HAactive);
	free(p);
	return n == 0;
}
			
char *
goinactivecmd(int argc, char **argv)
{
	if(!active())
		return "-ERR already inactive\r\n";
	csyslog("commanded inactive by %s\n", argc > 1 ? argv[1] : "secondary");
	setstate(HAinactive);
	killapp(cmdpid);
	return "+OK\r\n";
}
	
char *
goactivecmd(int argc, char **argv)
{
	if(active())
		return "-ERR already active\r\n";
	setstate(HAactive);
	csyslog("commanded active by %s\n", argc > 1 ? argv[1] : "secondary");
	qlock(&stlk);
	rwakeup(&ckstate);
	qunlock(&stlk);
	dopeercp = 1;
	return "+OK\r\n";
}
	
char *
listcmd(int, char **arg)	/* ask about a file */
{
	Dir *d;
	char *p, *e;
	int fd;
	static char buf[Nline];

	if (arg[1] == nil) {
		snprint(buf, Nline, "-ERR missing name\r\n");
		return buf;
	}

	p = buf;
	e = p + sizeof buf;
	fd = haopenconfig(arg[1]);
	if (fd < 0)
		goto none;
	d = dirfstat(fd);
	if (d == nil) {
		close(fd);
none:		p = seprint(p, e, "0 0 %s\r\n", arg[1]);
		seprint(p, e, "+OK 0 0 %s\r\n", arg[1]);
		goto e;
	}
	/* duplicate data returned before _+OK is compatibility for 1.0 */
	p = seprint(p, e, "%uld %lld %s\r\n", d->mtime, d->length, arg[1]);
	seprint(p, e, "+OK %uld %lld %s\r\n", d->mtime, d->length, arg[1]);
	free(d);
	close(fd);
e:	alarm(15000);
	return buf;
}

int
mkpwhash(uchar *out, int lim, char *hash)
{
	uchar buf[256];
	Nvrsafe *nv;
	DigestState *s;
	int n;
	
	if (lim < SHA2_256dlen)
		return -1;
	nv = malloc(sizeof *nv);
	if (readnvram(nv, 0) != 0) {
		free(nv);
		return -1;
	}
//**/	print("%s\n", nv->config);
//**/	print("%s\n", u->raddr);
//**/	print("%s\n", u->laddr);
//**/	print("hash=%s\n", hash);
	n = dec16(buf, sizeof buf, hash, strlen(hash));
	s = sha2_256((uchar *)nv->config, strlen(nv->config), nil, nil);
	s = sha2_256((uchar *)u->raddr, strlen(u->raddr), nil, s);
	s = sha2_256((uchar *)u->laddr, strlen(u->laddr), nil, s);
	sha2_256(buf, n, out, s);
	free(nv);
	return SHA2_256dlen;
}

char *
getpfile(char *fn)	/* open, read, strdup and close */
{
	int fd, n;
	char *p, buf[512];
	
	fd = open(fn, OREAD);
	if (fd == -1)
		return nil;
	n = read(fd, buf, sizeof buf);
	close(fd);
	if (n < 0) 
		return nil;
	if (p = strchr(buf, '\n'))
		*p = 0;
	buf[n] = 0;
	return strdup(buf);
}

void
putpfile(char *fn, char *s)	/* store string 's' in file 'fn' */
{
	int fd;
	
	fd = open(fn, OWRITE|OTRUNC);
	if (fd < 0)
		return;
	fprint(fd, "%s\n", s);
	close(fd);
}

void
cppeers(void)
{
	char buf[Nline];
	int r, w;

	r = readfile(buf, sizeof buf, "/n/kfs/conf/ha/peerea");
	if (r > 0) {
		w = writefile("/n/xlate/targ/peerea", "%s", buf);
		if (w == r)
			dopeercp = 0;
	}
}

void
clrpeers(void)
{
	writefile(peereafile, "\n");
	writefile("/n/xlate/targ/peerea", "\n");
}

