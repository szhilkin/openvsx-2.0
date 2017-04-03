#include <u.h>
#include <libc.h>
#include <fcall.h>

#pragma varargck argpos procsetname 1

#define Eservice	"VSX core service unavailable"
#define Einact		"VSX core hastate inactive"
#define Etimeo		"Command timeout"

enum {
	Ndata= 8192,
	Nmsg= Ndata + IOHDRSZ,
	Ncmdtimeo= 35*1000,
};

typedef struct Jmp Jmp;
struct Jmp {
	jmp_buf env;
	int set;
};

void protmon(void);
void tagmon(void);
void serve(void);
void opensrv(void);
void notifyf(void *, char *);
int active(void);
int setupmnt(int);
int attach(int, uint, int);
void clfid(uint);
char *addfid(uint);
int addtag(ushort);
int cltag(ushort);
void dolbolt(void);
void procsetname(char *, ...);
long awrite(int, void *, long, ulong);

int sfd[2];
int protfd = -1;
char *protect = "xlate";
uchar tmsg[Nmsg];
uchar rmsg[Nmsg];
int alarmingsecs = 5;
Jmp *jmp;
int debug;
ulong lbolt;

enum {
	Ntags= 64,
	Nfids= 16*1024,
};

struct {
	QLock;
	uint f[Nfids];
	uint n;
} fids;

struct {
	QLock;
	struct t {
		ushort stale;
		ushort t;
		ulong e;
	} t[Ntags];
} tags;

void
usage(void)
{
	fprint(2, "usage: %s [ -p /srv/prot ] [ -a alarmsecs ]\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	Jmp j;

	ARGBEGIN {
	case 'p':
		protect = EARGF(usage());
		break;
	case 'a':
		alarmingsecs = atoi(EARGF(usage()));
		break;
	case 'd':
		debug = 1;
		break;
	} ARGEND

	opensrv();
	if (rfork(RFPROC|RFNOTEG|RFNOWAIT))
		exits(0);
	jmp = &j;
	memset(jmp, 0, sizeof *jmp);
	notify(notifyf);
	if (rfork(RFPROC|RFMEM))
		dolbolt();
	if (rfork(RFPROC|RFMEM))
		protmon();
	if (rfork(RFPROC|RFMEM))
		tagmon();
	serve();
}

void
etag(ushort t)
{
	Fcall f;
	uchar msg[Nmsg];
	int n;

	memset(&f, 0, sizeof f);
	f.type = Rerror;
	f.ename = Etimeo;
	f.tag = t;
	n = convS2M(&f, msg, sizeof msg);
	if (n <= 0) {
		print("etag convS2M error\n");
		return;
	}
	awrite(sfd[0], msg, n, 5);
}

void
tagmon(void)
{
	ulong e;
	int i;

	procsetname("tagmon");
loop:
	sleep(1*1000);
	qlock(&tags);
	for (i=0; i<nelem(tags.t); i++) {
		if (tags.t[i].e == 0)
			continue;
		e = lbolt;
		if (e - tags.t[i].e >= Ncmdtimeo/1000) {
			if (debug)
				print("erroring tag %d\n", tags.t[i].t);
			etag(tags.t[i].t);
			tags.t[i].e = 0;
			tags.t[i].stale++;
		}
	}
	qunlock(&tags);
	goto loop;
}

void
flushtags(void)
{
	int i;

	qlock(&tags);
	for (i=0; i<nelem(tags.t); i++) {
		if (tags.t[i].e == 0)
			continue;
		if (debug)
			print("flushing tag %d\n", tags.t[i].t);
		etag(tags.t[i].t);
	}
	memset(tags.t, 0, sizeof tags.t);
	qunlock(&tags);
}

void
protmon(void)
{
	char *p;
	char srvf[64], ebuf[64];
	int fd, n;
	Fcall f;
	uchar msg[Nmsg];

	procsetname("protmon");

	if (p = strrchr(protect, '/'))
		p++;
	else
		p = protect;
	snprint(srvf, sizeof srvf, "#s/%s", p);
loop:
	protfd = -1;
	flushtags();
	postnote(PNGROUP, getpid(), "service");
	while ((fd = open(srvf, ORDWR)) < 0)
		sleep(1*1000);
	if (setupmnt(fd) < 0) {
		close(fd);
		goto loop;
	}
	if (setjmp(jmp->env)) {
		jmp->set = 0;
		close(protfd);
		goto loop;
	}
	jmp->set = 1;
	protfd = fd;
	for (;;) {
		n = read9pmsg(fd, msg, sizeof msg);
		if (debug)
			print("message rec'd: %d\n", n);
		if (n < 0) {
			rerrstr(ebuf, sizeof ebuf);
			if (strstr(ebuf, "interrupt"))
				continue;
			if (debug)
				print("monitor < 0: %s\n", ebuf);
			longjmp(jmp->env, 1);
		}
		if (n == 0)
			continue;
		memcpy(rmsg, msg, n);
		if (convM2S(msg, n, &f) > 0)
			if (f.type == Rattach)
				continue;
		if (cltag(f.tag) < 0)	// we already killed it
			continue;
		awrite(sfd[0], rmsg, n, 5);
	}
//	exits(0);
}

void
notifyf(void *a, char *s)
{
	if (debug)
		print("notifyf: %s %d\n", s, jmp->set);
	if (strncmp(s, "alarm", 5) == 0 || strncmp(s, "service", 7) == 0) {
		if (jmp->set)
			notejmp(a, jmp->env, 1);
	}
	noted(NCONT);
}

void
serve(void)
{
	char buf[64], *err;
	int fd;
	int n;
	Fcall thdr, rhdr;
	uchar msg[Nmsg];

	procsetname("serve");
	fd = sfd[0];
	for (;;) {
		err = nil;
		memset(&rhdr, 0, sizeof rhdr);
		memset(&thdr, 0, sizeof thdr);
		n = read9pmsg(fd, msg, sizeof msg);
		if (n < 0) {
			rerrstr(buf, sizeof buf);
			if (buf[0] == 0 || strstr(buf, "hungup")) {
				print("hasrv: exiting\n");
				exits(0);
			} else if (strstr(buf, "interrupt"))
				continue;
			sysfatal("mount read: %r");
		}
		if (n == 0)
			continue;
		memcpy(tmsg, msg, n);
		if (debug)
			print("convM2s\n");
		if (convM2S(msg, n, &thdr) <= 0) {
			print("convM2s failure: %r\n");
			continue;
		}
		if (debug)
			print("%d\n", thdr.type);
		switch (thdr.type) {
		case Tattach:
			if (err = addfid(thdr.fid))
				break;
			rhdr.qid.type = QTDIR;
			rhdr.qid.path = 0LL;
			rhdr.qid.vers = 0;
			break;
		case Tauth:
			err = "No auth required";
			break;
		case Tversion:
			if (strncmp(thdr.version, "9P2000", 6))
				err = "9p version failure";
			else
				rhdr.version = "9P2000";
			rhdr.msize = sizeof msg;
			break;
		case Tclunk:
			clfid(thdr.fid);	// note local, pass it through.
		default:
			if (!active()) {
				err = Einact;
				break;
			}
			if (addtag(thdr.tag) < 0) {
				err = Eservice;
				break;
			}
			if (setjmp(jmp->env)) {
				if (debug)
					print("setjmp\n");
e:				jmp->set = 0;
				alarm(0);
				continue;
			}
			jmp->set = 1;
			if (debug)
				print("write protfd: %d\n", protfd);
			if (awrite(protfd, tmsg, n, alarmingsecs) <= 0)
				goto e;
			jmp->set = 0;
			if (debug)
				print("message sent\n");
			continue;
		}
		if (err) {
			rhdr.type = Rerror;
			rhdr.ename = err;
		} else {
			rhdr.type = thdr.type + 1;
			rhdr.fid = thdr.fid;
		}
		rhdr.tag = thdr.tag;
		n = convS2M(&rhdr, msg, sizeof msg);
		if (n == 0)
			sysfatal("convS2M error: %r");
		if (debug)
			print("self response %d\n", n);
		
		if (awrite(sfd[0], msg, n, 5) != n)
			sysfatal("mount write: %r");
		if (debug)
			print("self response complete\n");
	}
}

void
opensrv(void)
{
	char buf[64];
	int fd;

	if (pipe(sfd) < 0)
		sysfatal("pipe failed: %r");
	snprint(buf, sizeof buf, "#s/hasrv.%s", protect);
	fd = create(buf, OWRITE|ORCLOSE, 0666);
	if (fd < 0)
		sysfatal("srv create failed: %r");
	if (fprint(fd, "%d", sfd[1]) < 0)
		sysfatal("writing srv fail: %r");
}

int
active(void)
{
	char *p;
	int n;

	p = getenv("hastate");
	if (p == nil)
		return 0;
	n = strncmp(p, "active", 6);
	free(p);
	return n == 0;
}

int
setupmnt(int fd)
{
	int i;

	qlock(&fids);
	for (i=0; i<fids.n; i++) {
		if (attach(fd, fids.f[i], 0) < 0) {
			qunlock(&fids);
			return -1;
		}
	}
	qunlock(&fids);
	return 0;
}

int
attach(int fd, uint fid, int fromsrv)
{
	Fcall f;
	uchar msg[Nmsg];
	int n;

	memset(&f, 0, sizeof f);
	f.type = Tattach;
	f.fid = fid;
	n = convS2M(&f, msg, sizeof msg);
	if (n <= 0) {
		print("attach convS2M failure: %r\n");
		return -1;
	}
	alarm(5*1000);
	if (write(fd, msg, n) <= 0) {
		print("attach write failure: %r\n");
		goto e;
	}
	if (fromsrv) {
		alarm(0);
		return 0;
	}
	if ((n = read(fd, msg, sizeof msg)) <= 0) {
		print("attach read failure: %r\n");
e:		alarm(0);
		return -1;
	}
	alarm(0);
	if (convM2S(msg, n, &f) <= 0) {
		print("attach convM2S failure: %r\n");
		return -1;
	}
	if (f.type == Rerror) {
		print("attach rerror: %s\n", f.ename);
		return -1;
	}
	return 0;
}

char *
addfid(uint fid)
{
	int i;

	qlock(&fids);
	for (i=0; i<fids.n; i++)
		if (fids.f[i] == fid) {
			qunlock(&fids);
			return nil;
		}
	if (fids.n >= nelem(fids.f)) {
		qunlock(&fids);
		return "out of afids";
	}
	fids.f[fids.n++] = fid;
	if (protfd >= 0)
		attach(protfd, fid, 1);
	qunlock(&fids);
	return nil;
}

void
clfid(uint fid)
{
	int i;

	qlock(&fids);
	for (i=0; i<fids.n; i++)
		if (fids.f[i] == fid)
			fids.f[i] = fids.f[--fids.n];
	qunlock(&fids);
}

int
addtag(ushort tag)
{
	int i, x, nms;
	enum { Nms = 1*1000, };

	nms = 0;
loop:	if (protfd < 0)
		return -1;
	qlock(&tags);
	x = -1;
	for (i=0; i<nelem(tags.t); i++) {
		if (tags.t[i].stale) {
			if (tags.t[i].t == tag) {
				x = i;
				break;
			}
			continue;
		}
		if (x == -1)
		if (tags.t[i].e == 0)
			x = i;
	}
	if (x == -1) {
		qunlock(&tags);
		if (nms >= Ncmdtimeo)
			return -1;
		nms += Nms;
		sleep(Nms);
		goto loop;
	}
	tags.t[x].e = lbolt;
	tags.t[x].t = tag;
	qunlock(&tags);
	return x;
}

int
cltag(ushort tag)
{
	int i;
	
	qlock(&tags);
	for (i=0; i<nelem(tags.t); i++)
		if (tags.t[i].t == tag) {
			if (tags.t[i].stale) {
				tags.t[i].stale--;
				break;
			}
			tags.t[i].e = 0;
			qunlock(&tags);
			return i;
		}
	qunlock(&tags);
	return -1;
}

void
dolbolt(void)
{
	procsetname("lbolt");
	for (;;) {
		lbolt++;
		sleep(1000);
	}
}

/*
 * from listen.c
 * based on libthread's threadsetname, but drags in less library code.
 * actually just sets the arguments displayed.
 */
void
procsetname(char *fmt, ...)
{
	int fd;
	char *cmdname;
	char buf[128];
	va_list arg;

	va_start(arg, fmt);
	cmdname = vsmprint(fmt, arg);
	va_end(arg);
	if (cmdname == nil)
		return;
	snprint(buf, sizeof buf, "#p/%d/args", getpid());
	if((fd = open(buf, OWRITE)) >= 0){
		write(fd, cmdname, strlen(cmdname)+1);
		close(fd);
	}
	free(cmdname);
}

long
awrite(int fd, void *p, long len, ulong secs)
{
	long n;

	alarm(secs*1000);
	n = write(fd, p, len);
	alarm(0);

	return n;
}
