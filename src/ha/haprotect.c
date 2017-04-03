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
 * hadaemon serves, haprotect protects.
 * this should be run on the secondary to keep track of the primary.
 * this can be run multiple times to keep track of multiple primaries.
 */

enum {
	Elogin = 0,
	EprimAct,
	EprimInact,
	Elostconn,
	Ecmdact,
	Ecmdinact,
	Esquat,
	
	Eprint,
	
	/* states */
	SinactNoConn = 0,
	SinactConn,
	SinactEstab,
	SactConn,
	SactNoConn,
	SactEstab,
	Squat,
};

char 	*cmdprim(char *);
int	checkconfig(void);
int	checkstatus(void);
void	client(void);
void	console(void);
void	debugtest(char **);
void	doact(void);
void	doinact(void);
int	expanded(char *, int, char *);
void	expargs(char **);
void	getconf(char *, char *, ulong);
char	*goactivecmd(int, char **);
char	*godown(int, char**);
char	*goinactivecmd(int, char **);
void	inactprim(void);
ulong	lclstat(char *, ulong*);
int	login(char *);
int	mkpwhash(uchar *, int, char *, NetConnInfo*);
void	replace(char *, char *, ulong);
ulong	rmtstat(char *, ulong *);
void	run(int, char **);
char	*showstatecmd(int, char **);
int	statemach(int);
char	*statuscmd(int, char **);
void	syncconf(void);
char	*getinitstate(void);
char	*getpfile(char *fn);
void	putpfile(char *, char *);
void	putconf(char *, char *, ulong);
void	getconf(char *, char *, ulong);
char	*addrchgcmd(int, char **);

Cmd	cmdtab[] = {
	"echo", 	0,	1,	echocmd,	/* just for test */
	"version",	0,	1,	versioncmd,
	"failover",	0,	1,	goactivecmd,
	"goact", 	0,	1,	goactivecmd,
	"godown",	0,	1,	godown,
	"goinact",	0,	1,	goinactivecmd,
	"restorepri", 	0,	1,	goinactivecmd,
	"status",	0,	1,	statuscmd,
	"state",	0,	1,	showstatecmd,
	"help",		0,	1,	helpcmd,
	"addrchg",	0,	1,	addrchgcmd,
	"eladdr",	0,	1,	eladdrcmd,
	nil, 0, 0, nil,
};

char *myrole = "secondary";
int	active;			/* true if active */
int	debug;
User useg;
User	*u;
int	sleeptime = 5000;	/* default to 5 seconds */
char	*remoteconf[Maxconf];
int	nconf = 0;
char	*rootdir;
Rendez	ckstate;
QLock	stlk;
int	cmdpid;
int 	spectest;
int	clientnote;
QLock	srvlk;			/* lock access to server */
QLock	addrlk;			/* lock access to address */
int	clientpid;
int	addrchg;
char	cdir[40];
int	seenpri;

void
catch(void *, char *msg)
{
	print("catch: msg=%s\n", msg);
	if (strcmp(msg, "alarm") == 0)
		noted(NCONT);
	noted(NDFLT);
}

void
usage(void)
{
	fprint(2, "usage: %s [-s stime] -d dir -r conf [-p primary] cmd args ... \n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *initstate;

	u = &useg;
	u->raddr = nil;
	notify(catch);
	fmtinstall('E', eipfmt);
	fmtinstall('I', eipfmt);
	fmtinstall('V', eipfmt);
	fmtinstall('H', encodefmt);
	ARGBEGIN {
	case 's':
		sleeptime = atoi(EARGF(usage()));
		break;
	case 'r':
		if (nconf >= Maxconf) {
			fprint(2, "Cannot protect more than %d configuration files\n", Maxconf);
			usage();
		}
		remoteconf[nconf++] = EARGF(usage());
		break;
	case 'd':
		rootdir = EARGF(usage());
		break;
	case 'p':
		u->raddr = EARGF(usage());
		break;
	case 'X':
		debug++;
		break;
	case 'Z':
		spectest++;
		break;
	default:
		usage();
	} ARGEND
	if (argc < 1)
		usage();
	if (nconf == 0 || rootdir == nil)
		usage();
	debugtest(argv);
	ckstate.l = &stlk;
	csyslog("%s started\n", argv0);
	if(u->raddr == nil)
	if(checkrole(&u->raddr)) {
		print("haprotect called as primary\n");
		exits(0);
	}
	initstate = getinitstate();
	if (initstate) {
		if (strcmp(initstate, "active") == 0)
			statemach(Ecmdact);
		free(initstate);
	}
	if ((clientpid = rfork(RFPROC|RFMEM)) == 0)
		client();
	if (rfork(RFPROC|RFMEM) == 0)
		cmd();
	if (rfork(RFPROC|RFMEM) == 0)
		run(argc, argv);
	exits(nil);
}

void
clientcatch(void *, char *s)
{
	if(debug)
		fprint(2, "client catch: %s\n", s);
	if (strcmp(s, "wakeup") == 0 || strcmp(s, "alarm") == 0)
		noted(NCONT);
	noted(NDFLT);
}

char *
remvers(void)
{
	char line[Nline], *p;

	qlock(&srvlk);
	sendcrnl(&u->out, "version");
	if (getcrnlto(u, line, sizeof line) == -1) {
		csyslog("remvers: put init failure\n");
e:		qunlock(&srvlk);
		return nil;
	}
	if (line[0] != '+') {
		csyslog("remvers: version command failed: %s\n", line);
		goto e;
	}
	p = strchr(line, ' ');
	if (p == nil) {
		csyslog("remvers: wrong version format: %s\n", line);
		goto e;
	}
	qunlock(&srvlk);
	return killws(strdup(p+1));
}

int
mkdir(char *p)
{
	int fd;

	fd = create(p, OREAD, 0777|DMDIR);
	if (fd < 0) {
		csyslog("cannot create file %s: %r\n", p);
		return - 1;
	}
	close(fd);
	return 0;
}

int
setraddr(char *addr)	/* called with addrlk */
{
	char buf[100], *s;
	int i;

	snprint(buf, sizeof buf, "%s/role", rootdir);
	if (writefile(buf, "%s %s", myrole, addr) < 0)
		return -1;
	s = smprint("%s/%s", rootdir, addr);
	rmall(s);
	mkdir(s);
	free(s);	
	if (active) {
		for (i = 0; i < nconf; i++) {
			s = smprint("%s/%s/%s", rootdir, addr, basename(remoteconf[i]));
			copy(remoteconf[i], s);
			free(s);
		}
	}
	csyslog("changing Primary address from %s to %s\n", u->raddr, addr);
	free(u->raddr);
	u->raddr = strdup(addr);
	addrchg = 1;
	seenpri = 0;
	return 0;
}

/* Upgrade for VSX-1.5 to VSX-2.0 without user intervention */
/* If the remote version is not 1.5 and ip address, convert */
int
elconv(void)		/* called with addrlk */
{
	char line[Nline], *p;

	if (strncmp(u->raddr, "5100", 4) == 0)
		return 1;
	qlock(&srvlk);
	sendcrnl(&u->out, "eladdr");
	if (getcrnlto(u, line, sizeof line) == -1) {
		csyslog("elconv: put init failure\n");
e:		qunlock(&srvlk);
		return -1;
	}
	if (line[0] != '+') {
		csyslog("elconv: eladdr command failed: %s\n", line);
		goto e;
	}
	p = strchr(line, ' ');
	if (p == nil) {
		csyslog("elconv: eladdr format failed: %s\n", line);
		goto e;
	}
	p = killws(p);
	if (setraddr(p) < 0) {
		csyslog("elconv: setraddr failed to save %s\n", p);
		goto e;
	}
	qunlock(&srvlk);
	return 0;
}

static int
putaddrsx(void)
{
	char line[Nline];

	sendcrnl(&u->out, "recvaddrs");
	if (getcrnlto(u, line, sizeof line) == -1) {
		csyslog("putaddrs: put init failure\n");
		return -1;
	}
	if (line[0] == '-') {
		csyslog("putaddrs: recvaddrs command failed: %s\n", line);
		return -1;
	}
	sendaddrs(&u->out);
	if (getcrnlto(u, line, sizeof line) == -1) {
		csyslog("putaddrs: put failed: %r\n");
		return -1;
	}
	if (line[0] != '+') {
		csyslog("putaddrs: error on recvaddrs: %s\n", line);
		return -1;
	}
	return 0;
}

int
putaddrs(void)
{
	int ret;

	qlock(&srvlk);
	ret = putaddrsx();
	qunlock(&srvlk);
	return ret;
}	

static int
getaddrsx(void)
{
	char line[Nline];

	sendcrnl(&u->out, "sendaddrs");
	if (receiveaddrs(u, active)) {
		if (getcrnlto(u, line, sizeof line) == -1) {
			csyslog("getaddrs: put init failure\n");
			return -1;
		}
		if (line[0] == '-') {
			csyslog("receiveaddrs failed: %s\n", line);
			return -1;
		}
	}
	return 0;
}

int
getaddrs(void)
{
	int ret;

	qlock(&srvlk);
	ret = getaddrsx();
	qunlock(&srvlk);
	return ret;
}	

void
logoff(void)
{
	qlock(&srvlk);
	sendcrnl(&u->out, "quit");
	qunlock(&srvlk);
}

int
isclosed(void)
{
	char buf[255];

	if (readfile(buf, sizeof buf, "%s/status", cdir) < 0)
		return -1;
	return strncmp(buf, "Closed", 6) == 0;
}

/* wait up to 60 seconds to see if the el connection closes */
/* hung xlate takes 70 seconds to timeout, we have 80 */
int
waitclose(void)
{
	int i;

	for (i = 0; i < 60; i++, sleep(1000))
		if (isclosed())
			return 0;
	return -1;
}

static int grace = 60;

void
bootgrace(void)
{
	for (; grace; grace--)
		sleep(1000);
	exits(0);
}

/*
 * notice: if you can't connect to the primary, login returns a -1.  if it can
 * connect but can't login, for example the passwords are different, login returns
 * -2.  we shouldn't protect just because the password is different.
 */

void
client(void)
{
	char *p, *v;
	int fd, cantconnmsg, cantloginmsg, n;
	char buf[Nline];

	if (rfork(RFPROC|RFMEM))
		bootgrace();
	notify(clientcatch);
	for (;;) {
		u->loggedin = cantconnmsg = cantloginmsg = 0;
		for (;;) {
			qlock(&addrlk);
			if (addrchg) {
				addrchg = 0;
				cantconnmsg = cantloginmsg = 0;
			}
			if(debug)
				print("Attempting to log in to: %s\n", u->raddr);
			alarm(5*1000);
			fd = login(u->raddr);
			alarm(0);
			switch (fd) {
			case -1:
				if(!grace && cantconnmsg > 0)
					statemach(Elostconn);
				if (cantconnmsg++ == 0)
					csyslog("cannot connect to %s: %r\n", u->raddr);
				qunlock(&addrlk);
				continue;
			case -2:
				if (!grace && cantloginmsg > 0)
					statemach(Elostconn);
				if (cantloginmsg++ == 0)
					csyslog("Login to primary %s failed; PRIMARY NOT PROTECTED\n", u->raddr);
				qunlock(&addrlk);
				continue;
			}
			u->loggedin = 1;
			statemach(Elogin);
			break;
		}
		if(debug)
			print("Logged into %s\n", u->raddr);
		csyslog("logged into %s\n", u->raddr);
		syncconf();

		/* not backwards compatible with 1.5 */
		v = remvers();
		if (v && strncmp(v, "VSX-1.", 6)) {
			if (elconv() == 0)
				goto out;
			if (getaddrs() < 0 || putaddrs() < 0) {
				csyslog("detects unresponsive primary node\n");
				if (seenpri) {
					if (active)
						cmdprim("goinact");
					else
						statemach(Ecmdact);
				} else
					csyslog("Login to primary %s incomplete: PRIMARY NOT PROTECTED\n", u->raddr);
				/* relogin after remote stops hanging */
				logoff();
				Bflush(&u->out);
				waitclose();
				goto out;
			}
		}

		seenpri++;
		qunlock(&addrlk);
		for (;;) {
			qlock(&addrlk);
			if(debug)
				print("Checking status\n");
			if (addrchg) {
				addrchg = 0;
				if (debug)
					print("Secondary address change\n");
				break;
			}
			n = checkstatus();
			if(debug)
				print("Got %d from checkstatus\n", n);
			switch (n) {
			case 0:
				statemach(EprimAct);
				break;
			case -1:
				goto out;
			case -2:
				statemach(EprimInact);
				break;
			}
			qlock(&srvlk);
			p = getstate();
			sendcrnl(&u->out, "putstatus %s", p);
			free(p);
			if(getcrnlto(u, buf, sizeof buf) <= 0) {
				qunlock(&srvlk);
				break;
			}
			qunlock(&srvlk);
			if(debug)
				print("Checking config\n");
			if (checkconfig() < 0)
				break;
			if(debug)
				print("Sleeping %d\n", sleeptime);
			qunlock(&addrlk);
			sleep(sleeptime);
		}
	out:
		free(v);
		Bterm(&u->in);
		Bterm(&u->out);
		close(fd);
		qunlock(&addrlk);
	}
}

int
statemach(int event)
{
	static state;
	static Lock statelk;

	lock(&statelk);
	if (debug)
		print("statemach: state=%d event=%d ==> ", state, event);
	if (event == Eprint)
		csyslog("statemach: state=%d event=%d\n", state, event);
	if (event == Esquat)
		state = Squat;
	switch (state) {
	case Squat:
		break;
	case SinactNoConn:
		switch (event) {
		case Elogin:
			state = SinactConn;
			break;
		case Ecmdact:
			state = SactNoConn;
			doact();
			break;
		case Elostconn:
			state = SactNoConn;
			doact();
			break;
		}
		break;
	case SinactConn:
		switch (event) {
		case EprimInact:
			csyslog("commanded active by primary\n");
			state = SactEstab;
			doact();
			break;
		case EprimAct:
			state = SinactEstab;
			break;
		case Ecmdact:
			state = SactConn;
			doact();
			cmdprim("goinact");
			break;
		case Elostconn:
			state = SactNoConn;
			doact();
			break;
		}
		break;
	case SinactEstab:
		switch (event) {
		case EprimInact:
			csyslog("commanded active by primary\n");
			state = SactEstab;
			doact();
			break;
		case Ecmdact:
			state = SactEstab;
			doact();
			cmdprim("goinact");
			break;
		case Elostconn:
			state = SactNoConn;
			doact();
			break;
		}
		break;
	case SactNoConn:
		switch (event) {
		case Elogin:
			state = SactConn;
			break;
		case Ecmdinact:
			csyslog("attempt to go inactive with no primary connection\n");
			break;
		}
		break;
	case SactConn:
		switch (event) {
		case EprimInact:
			state = SactEstab;
			break;
		case EprimAct:
			state = SactEstab;
			cmdprim("goinact");
			break;
		case Ecmdinact:
			state = SinactConn;
			doinact();
			break;
		case Elostconn:
			state = SactNoConn;
			break;
		}
		break;
	case SactEstab:
		switch (event) {
		case EprimAct:
			/*
			 * We'll probably want to change this case somewhere down
			 * the line so that the restore to primary can be initiated
			 * on the primary.  But for now, we want what the secondary
			 * says to go.
			 */
			cmdprim("goinact");
			break;
		case Ecmdinact:
			state = SinactEstab;
			doinact();
			break;
		case Elostconn:
			state = SactNoConn;
			break;
		}
		break;
	}
	if (debug)
		print("state=%d\n", state);
	unlock(&statelk);
	return state;
}
	
int
login(char *s)		/* dial and login to a primary: -1 means protect, -2 means something else */
{
	char buf[32], line[Nline], *p, *q;
	uchar hash[32];
	int fd, cfd, eout;
	NetConnInfo *ni;

	snprint(buf, sizeof buf, "%s!%s!"HAport, strncmp(u->raddr, "5100", 4) ? "tcp" : "el", s);
	eout = -1;
	if(u->loggedin){
		werrstr("Trying to log in to already logged in connection\n");
		return -1;
	}
	memset(cdir, 0, 40);
	fd = dial(buf, nil, cdir, &cfd);
	if (fd == -1)
		return -1;

	ni = getnetconninfo(cdir, cfd);
	if (ni == nil) {
		werrstr("getnetconninfo: %r");
		close(cfd);
		close(fd);
		return -1;
	}

	Binit(&u->in, fd, OREAD);
	Binit(&u->out, fd, OWRITE);
	qlock(&srvlk);
	if (getcrnlto(u, line, sizeof line) == -1) {
		goto e;
	}
	if (strncmp(line, "+OK HA", 6) != 0) {
		werrstr("%s\n", line);
		goto e;
	}
	if ((p = strchr(line, '<')) && (q = strchr(p+1, '>'))) {
		*q = 0;
		if (mkpwhash(hash, sizeof hash, p+1, ni) == -1) {
			eout = -2;
			goto e;
		}
		sendcrnl(&u->out, "login %s %.*H", getuser(), sizeof hash, hash);
		if (getcrnlto(u, line, sizeof line) == -1) {
			goto e;
		}
		if (line[0] != '+') {
			werrstr("bad line: %s\n", line);
			eout = -2;
			goto e;
		}
		qunlock(&srvlk);
		close(cfd);
		freenetconninfo(ni);
		return fd;
	} 
	werrstr("missing hash: line=%s\n", line);
e:	qunlock(&srvlk);
	if(ni)
		freenetconninfo(ni);
	close(fd);
	close(cfd);
	return eout;
}

int
mkpwhash(uchar *out, int lim, char *chal, NetConnInfo *ni)
{
	uchar buf[256];
	Nvrsafe *nv;
	DigestState *s;
	int n;
	
	if (lim < SHA2_256dlen) {
		werrstr("mkpwhash: lim too small");
		return -1;
	}
	nv = malloc(sizeof *nv);
	if (readnvram(nv, 0) != 0) {
		free(nv);
		return -1;
	}
	n = dec16(buf, sizeof buf, chal, strlen(chal));
	s = sha2_256((uchar *)nv->config, strlen(nv->config), nil, nil);
	s = sha2_256((uchar *)ni->laddr, strlen(ni->laddr), nil, s);
	s = sha2_256((uchar *)ni->raddr, strlen(ni->raddr), nil, s);
	sha2_256(buf, n, out, s);
	free(nv);
	return SHA2_256dlen;
}

int
checkstatus(void)	/* do status check */
{
	char *p;
	char line[Nline];
	int status;

	qlock(&srvlk);
	sendcrnl(&u->out, "status");
	do {
		if (getcrnlto(u, line, sizeof line) <= 0) {
			qunlock(&srvlk);
			return -1;
		}
	} while(line[0] != '+' && line[0] != '-');
	if (line[0] == '-') {
		qunlock(&srvlk);
		return -2;
	}
	if((p = strchr(line, ' ')) && (p = strchr(p+1, ' ')) && strncmp(p+1, "active", 6) == 0)
		status = 0;	/* okay */
	else
		status = -2;
	u->rmtactive = status == 0;
	qunlock(&srvlk);
	return status;
}

void
inactprim(void)		/* tell the primary not to bother */
{
	char line[Nline];
	
	qlock(&srvlk);
	sendcrnl(&u->out, "goinact");
	getcrnlto(u, line, sizeof line);
	qunlock(&srvlk);
}

int
checkconfig(void)
{
	ulong mtime, length;
	ulong lmtime, llength;
	char *s;
	int i;

	if (nconf == 0)
		return 0;	/* nothing to check */
	for (i = 0; i < nconf; ++i) {
		mtime = rmtstat(remoteconf[i], &length);
		if (debug)
			fprint(2, "rmtstat returns %ld %ld\n", mtime, length);

		if (mtime == -1)
			return mtime;

		s = smprint("%s/%s/%s", rootdir, u->raddr ? u->raddr : "unknown", basename(remoteconf[i]));
		lmtime = lclstat(s, &llength);
		if (debug)
			fprint(2, "lclstat returns %ld %ld\n", lmtime, llength);
//		if (lmtime == 0) {
//			free(s);
//			return 0;
//		}
#ifdef notdef
		if (lmtime < mtime) // || llength != length)
			getconf(remoteconf[i], s, mtime);
		else if (lmtime > mtime)
			putconf(remoteconf[i], s, lmtime);
		else if (llength != length) {
			// ok, so mtime is in sync, but length is off.  weird.  and probably bad.
			if (active)	// i win
				putconf(remoteconf[i], s, lmtime);
			else		// he wins
				getconf(remoteconf[i], s, mtime);
		}
#endif
		if (lmtime != mtime) {
			if (active)
				putconf(remoteconf[i], s, lmtime);
			else
				getconf(remoteconf[i], s, mtime);
		}
		free(s);
	}
	return 0;
}

ulong
lclstat(char *s, ulong *lengthp)
{
	Dir *d;
	ulong mtime;
	int fd;

	fd = haopenconfig(s);
	if (fd < 0) {
//??		csyslog("failure opening %s for lclstat: %r\n", s);
		return 0;
	}
	mtime = 0;
	d = dirfstat(fd);
	if (d != nil) {
		*lengthp = d->length;
		mtime = d->mtime;
		free(d);
	}
	close(fd);
	return mtime;
}

ulong
rmtstat(char *fn, ulong *lengthp)	/* get mtime and length */
{
	char line[Nline], *toks[5];
	ulong mtime;

	qlock(&srvlk);
	*lengthp = 0;
	sendcrnl(&u->out, "list %s", fn);
	do {
		if(getcrnlto(u, line, sizeof line) <= 0) {
			qunlock(&srvlk);
			return -1;
		}
	} while(line[0] != '+' && line[0] != '-');
	if(line[0] != '+') {
		qunlock(&srvlk);
		return -1;
	}
	if(tokenize(line, toks, 5) != 4) {
		qunlock(&srvlk);
		return -1;
	}
	mtime = 0;
	if(isdigit(*toks[1])) {
		mtime = strtoul(toks[1], nil, 10);
		*lengthp = strtoul(toks[2], nil, 10);
	}
	qunlock(&srvlk);

	return mtime;
}

static char cfgbuf[512*1024];

void
getconf(char *rmt, char *lcl, ulong mtime)
{
	char line[Nline];
	char *p, *ep;

	/* An inactive node is never the winner */
	if (active)
		return;
	qlock(&srvlk);
	p = cfgbuf;
	ep = p + sizeof cfgbuf;
	sendcrnl(&u->out, "get %s", rmt);
	for (;;) {
		if (getcrnlto(u, line, sizeof line) == -1)
			break;
		if (line[0] == '-') {
			csyslog("getconf failed: %s\n", line);
			break;
		}
		if (line[0] == '+')
			break;
		p = seprint(p, ep, "%s\n", line);
	}
	/* Force this to be written out, we need it sync'ed with peer */
	if (hawriteconfig(lcl, mtime, cfgbuf, p - cfgbuf, 1) < 0)
		csyslog("failed to write fetched config file\n");
	qunlock(&srvlk);
}

void
putconf(char *rmt, char *lcl, ulong mtime)
{
	char line[Nline];
	char *l;
	Biobuf *bp, b;
	int fd;

	/* An inactive node is never the winner */
	if (!active)
		return;
	qlock(&srvlk);
	fd = haopenconfig(lcl);
	if (fd < 0) {
		qunlock(&srvlk);
		csyslog("putconf: cannot open %s: %r\n", lcl);
		return;
	}
	bp = &b;
	memset(bp, 0, sizeof b);
	if (Binit(bp, fd, OREAD) < 0) {
		qunlock(&srvlk);
		close(fd);
		csyslog("putconf: cannot Binit %s: %r\n", lcl);
		return;
	}
	sendcrnl(&u->out, "put %s %uld", rmt, mtime);
	if (getcrnlto(u, line, sizeof line) == -1) {
		csyslog("putconf: put init failure\n");
		goto e;
	}
	if (line[0] == '-') {
		csyslog("putconf: put command failed: %s\n", line);
		goto e;
	}
	while (l = Brdstr(bp, '\n', 1)) {
		sendcrnl(&u->out, "%s", l);
		free(l);
	}
	sendcrnl(&u->out, ".");
	if (getcrnlto(u, line, sizeof line) == -1) {
		csyslog("putconf; put failed: %r\n");
		goto e;
	}
	if (line[0] != '+') {
		csyslog("putconf: error on put: %s\n", line);
		goto e;
	}
e:	qunlock(&srvlk);
	Bterm(bp);
	close(fd);
}

/* ensure file exists so bind will work */
void
exist(char *p)
{
	int fd;

	fd = open(p, OREAD);
	if (fd < 0) {
		fd = create(p, OREAD, 0666);
		if (fd < 0) {
			csyslog("cannot create file %s: %r\n", p);
			return;
		}
	}
	close(fd);
}

void
doact(void)
{
	char *p;
	int i;

	active = 1;
	putpfile(stateenv, "active");
	putpfile(statefile, "active");
	csyslog("going active\n");
	for (i = 0; i < nconf; ++i) {
		p = smprint("%s/%s/%s", rootdir, u->raddr ? u->raddr : "unknown", basename(remoteconf[i]));
		exist(remoteconf[i]);
		bind(p, remoteconf[i], MREPL);
		free(p);
	}
	qlock(&stlk);
	rwakeup(&ckstate);
	qunlock(&stlk);
}

char *
addrchgcmd(int argc, char **argv)
{
	int n;

	if (argc != 2)
		return "-ERR usage: addrchg newaddress\r\n";
	qlock(&addrlk);
	n = setraddr(argv[1]);
	qunlock(&addrlk);
	if (n < 0) 
		return "-ERR set address failed\r\n"; 
	qlock(&srvlk);
	sendcrnl(&u->out, "bye");
	qunlock(&srvlk);
	postnote(PNPROC, clientpid, "alarm");
	return "+OK\r\n";
}

char *
goactivecmd(int argc, char **argv)
{
	csyslog("commanded active by %s\n", argc > 1 ? argv[1] : "primary");
	statemach(Ecmdact);
	return "+OK\r\n";
}

void
doinact(void)
{
	int i;

	active = 0;
	putpfile(stateenv, "inactive");
	putpfile(statefile, "inactive");
	csyslog("going inactive\n");
	killapp(cmdpid);
	for (i = 0; i < nconf; ++i)
		unmount(nil, remoteconf[i]);
	cmdprim("goact");
}

char *
cmdprim(char *cmd)	/* issues simple commands */
{
	char line[Nline];
	
	qlock(&srvlk);
	sendcrnl(&u->out, "%s", cmd);
	do {
		if (getcrnlto(u, line, sizeof line) == -1) {
			qunlock(&srvlk);
			return "lost connection to primary";
		}
	} while (line[0] != '+' && line[0] != '-');
	qunlock(&srvlk);
	return nil;
}

char *
godown(int, char**)
{
	active = 0;
	statemach(Esquat);
	killapp(cmdpid);
	return "+OK\r\n";
}
char *
goinactivecmd(int argc, char **argv)
{
	if(!u->loggedin)
		return "-ERR cannot set inactive without primary connected\r\n";
	csyslog("commanded inactive by %s\n", argc > 1 ? argv[1] : "primary");
	statemach(Ecmdinact);
	return "+OK\r\n";
}

char *
getstate(void)
{
		return strdup(active ? "active" : "inactive");
}
	
char *
getinitstate(void)
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

char *
showstatecmd(int, char **)	/* debug the state machine */
{
	statemach(Eprint);
	return "+OK\r\n";
}

void
run(int , char **argv)
{
	int n;
	int pfd[2];
	char err[64];
	int saidit;

	expargs(argv);
	saidit = 0;
	for (;;) {
		if (active == 0) {
			setprompt();
			if (saidit++ == 0)
				csyslog("set inactive; VSX CORE NOT RUNNING\n");
			qlock(&stlk);
			rsleep(&ckstate);
			qunlock(&stlk);
			continue;
		}
		saidit = 0;
		if (pipe(pfd) < 0)
			error("pipe error");
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
			error(*argv);
		}
	}
}

/*
 * check that the primary has the newest config.
 * if it is inactive, and our config is newer, send it.
 */

void
syncconf(void)
{
	ulong mtime, length;
	ulong lmtime, llength;
	int st, i;
	char *s;
	
	st = checkstatus();
	if (st == -1 || st == 0)
		return;
	for (i = 0; i < nconf; ++i) {
		mtime = rmtstat(remoteconf[i], &length);
		if (mtime == -1)
			continue;
	
		s = smprint("%s/%s/%s", rootdir, u->raddr ? u->raddr : "unknown", basename(remoteconf[i]));
		lmtime = lclstat(s, &llength);
		if (lmtime && lmtime > mtime)
			putconf(remoteconf[i], s, lmtime);
		free(s);
	}
}

/*
 * expand the arguments.
 * the strings are scanned for expansion names.
 *	\c	path to the config directory.
 *	\r	root
 *	\p	primary target name or address
 */

void
expargs(char **argv)
{
	char buf[Nline];
	
	while (*argv) {
		if (expanded(buf, sizeof buf, *argv))
			*argv = strdup(buf);
		argv++;
	}
}

int
expanded(char *buf, int len, char *arg)
{
	int xp, n;
	
	xp = 0;
	len--;
	while (*arg) {
		if (*arg == '\\') {
			arg++;
			switch (*arg) {
			case 'c':
				xp++;
				n = snprint(buf, len+1, "%s/%s/%s", rootdir, u->raddr ? u->raddr : "unknown", remoteconf[0]);
				buf += n;
				break;
			case 'p':
				xp++;
				n = snprint(buf, len+1, "%s", u->raddr ? u->raddr : "unknown");
				buf += n;
				break;
			case 'r':
				xp++;
				n = snprint(buf, len+1, "%s", rootdir);
				buf += n;
				break;
			default:
				*buf++ = *arg;
				len--;
				break;
			}
			arg++;
			continue;
		}
		*buf++ = *arg++;
		len--;
	}
	*buf = 0;
	return xp;
}

void
debugtest(char **argv)
{
	if (spectest == 0)
		return;
	expargs(argv);
	while (*argv)
		print("%s\n", *argv++);
	exits("debugtest");
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
	if (fd == -1)
		return;
	fprint(fd, "%s\n", s);
	close(fd);
}
