#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <pool.h>
#include "dat.h"
#include "fns.h"


/*
 * File system for xlate.
 * This code started life as ramfs.c.
 */


ulong	path;		/* incremented for each new file */
Fid	*fids;
Lock	inodelk;
int	mfd[2];
char	*user;
uchar	mdata[IOHDRSZ+Maxfdata];
uchar	rdata[Maxfdata];	/* buffer for data in reply */
uchar 	statbuf[STATMAX];
Fcall 	thdr;
Fcall	rhdr;
int	messagesize = sizeof mdata;
QLock	fslck;
QLock	fidlck;
static	Inode	*allocstart;
Inode	*freeinodes;

static	Fid*	newfid(int);
static	uint	inodestat(Inode*, uchar*, uint);
static	void	io(void);
static	void	*erealloc(void*, ulong);
static	void	*emalloc(ulong);
static	char	*estrdup(char*);
static	void	usage(void);
static	int	perm(Fid*, Inode*, int);
static	void	startrrfs(void);
static	void	getinode(Inode *);
static	void	putinode(Inode *);
static	void	freeinode(Inode *);
static	ulong	namehash(char *);

static	char	*rflush(Fid*), *rversion(Fid*), *rauth(Fid*),
		*rattach(Fid*), *rwalk(Fid*),
		*ropen(Fid*), *rcreate(Fid*),
		*rread(Fid*), *rwrite(Fid*), *rclunk(Fid*),
		*rremove(Fid*), *rstat(Fid*), *rwstat(Fid*);

int needfid[] = {
	[Tversion]	0,
	[Tflush] 	0,
	[Tauth] 	0,
	[Tattach] 	0,
	[Twalk] 	1,
	[Topen] 	1,
	[Tcreate] 	1,
	[Tread] 	1,
	[Twrite] 	1,
	[Tclunk] 	1,
	[Tremove] 	1,
	[Tstat] 	1,
	[Twstat] 	1,
};

char 	*(*fcalls[])(Fid*) = {
	[Tversion]	rversion,
	[Tflush]	rflush,
	[Tauth]		rauth,
	[Tattach]	rattach,
	[Twalk]		rwalk,
	[Topen]		ropen,
	[Tcreate]	rcreate,
	[Tread]		rread,
	[Twrite]	rwrite,
	[Tclunk]	rclunk,
	[Tremove]	rremove,
	[Tstat]		rstat,
	[Twstat]	rwstat,
};

static	char	Eperm[] =	"permission denied";
static	char	Enotdir[] =	"not a directory";
static	char	Enoauth[] =	"xlatefs: authentication not required";
static	char	Enotexist[] =	"file does not exist";
static	char	Einuse[] =	"file in use";
static	char	Eexist[] =	"file exists";
static	char	Eisdir[] =	"file is a directory";
static	char	Enotowner[] =	"not owner";
static	char	Eisopen[] = 	"file already open for I/O";
static	char	Excl[] = 	"exclusive use file already open";
static	char	Ename[] = 	"illegal name";
static	char	Eversion[] =	"unknown 9P version";
static	char	Enotempty[] =	"directory not empty";
static	char	Ephase[] =	"phase error";

int debug;
static int alarmingsecs;

static void
shutitdown(void)
{
	remove("/srv/remote");
	system("kill remotefs rrfs | rc");
	tagdebug = 0;
	shutdown = 1;
}

static
void
notifyf(void *, char *s)
{
//	fprint(2, "xlate notifyf: %s\n", s);
	if (strncmp(s, "interrupt", 9) == 0)
		noted(NCONT);
	if (strncmp(s, "shutdown", 8) == 0) {
		shutitdown();
		noted(NCONT);
	}
	if (strcmp(s, "alarm") == 0) {
		noted(NCONT);
	}
	if (strcmp(s, shadowclose) == 0) {
		noted(NCONT);
	}
	noted(NDFLT);
}

static 
void
fsnotifyf(void *, char *s)
{
//	fprint(2, "xlate fsnotifyf: %s\n", s);
	if(strcmp(s, "interrupt") == 0)
		noted(NCONT);
	else if (strcmp(s, "alarm") == 0) {
		if (alarmflag)
			noted(NCONT);
		print("hang alarm fired, closing mount\n");
		close(mfd[0]);
		close(mfd[1]);
		noted(NCONT);
	} else if (strstr(s, "restrict"))
		noted(NCONT);
	else if (strncmp(s, "shutdown", 8) == 0) {
		shutitdown();
		noted(NCONT);
	}
	noted(NDFLT);
}

void **private;
char **procname;

void **
procdata(void)
{
	return private;
}

void
procsetname(char *name)
{
	*procname = strdup(name);
}

char *
procgetname(void)
{
	return *procname;
}

void
heapcap(ulong maxmem)
{
	if (mainmem == nil) {
		print("can't cap malloc pool memory, mainmem nil\n");
		return;
	}
	mainmem->maxsize = maxmem;
}

static void
upgraded(void)
{
	releaselast = getenv("releaselast");
	if (releaselast && releaselast[0] == '\0') {
		free(releaselast);
		releaselast = nil;
	}
}

void
main(int argc, char *argv[])
{
	char *defmnt, *service;
	User useg;
	int p[2];
	int fd;
	int stdio = 0;
	int Xflag = 0;
	char *name;

	heapcap(2*1024*1024*1024);	// limit ourselves to 2GB heap.
	notify(notifyf);
	memset(&useg, 0, sizeof useg);
	u = &useg;
	procname = &name;

	quotefmtinstall();
	fmtinstall('H', encodefmt);
	fmtinstall('T', Tfmt);
	fmtinstall('I', iostatfmt);
	fmtinstall('O', iopsfmt);
	service = "xlate";
	defmnt = "/n/xlate";
	conffile = "/n/kfs/conf/xlate";
	nworkers = NWRKRS;
	ARGBEGIN{
	case 'a':
		alarmingsecs = atoi(EARGF(usage()));
		break;
	case 'i':
		defmnt = 0;
		stdio = 1;
		mfd[0] = 0;
		mfd[1] = 1;
		break;
	case 'm':
		defmnt = EARGF(usage());
		break;
	case 's':
		defmnt = 0;
		break;
	case 'D':
		debug = 1;
		break;
	case 'R':
		restricted = 1;
		break;
	case 'S':
		defmnt = 0;
		service = EARGF(usage());
		break;
	case 'c':	/* configuration file */
		conffile = EARGF(usage());
		break;
	case 'X':	/* no exit, for ha */
		Xflag++;
		break;
	case 'w':
		nworkers = atoi(EARGF(usage()));
		break;
	default:
		usage();
	}ARGEND

	if(pipe(p) < 0)
		sysfatal("pipe failed: %r");
	if(!stdio){
		mfd[0] = p[0];
		mfd[1] = p[0];
		if (defmnt == 0){
			char buf[64];
			snprint(buf, sizeof buf, "#s/%s", service);
			fd = create(buf, OWRITE|ORCLOSE, 0666);
			if (fd < 0)
				sysfatal("create failed: %r");
			sprint(buf, "%d", p[1]);
			if (write(fd, buf, strlen(buf)) < 0)
				sysfatal("writing service file: %r");
		}
	}

	user = getuser();
	root.parent = &root;
	root.perm = DMDIR | 0777;
	root.qid.type = QTDIR;
	root.qid.path = 0LL;
	root.qid.vers = 0;
	root.user = user;
	root.group = user;
	root.muid = user;
	root.atime = time(0);
	root.mtime = root.atime;
	root.name = estrdup(".");
	root.next = nil;
	root.chash = mallocz(Nchash * sizeof (Inode *), 1);
	if (root.chash == nil)
		panic("root chash allocation failure");
	getinode(&root);

	upgraded();

	if (access("/n/kfs/conf/togb", 0) == 0)
		togb = 1;
	if (debug) {
		fmtinstall('F', fcallfmt);
		fmtinstall('M', dirmodefmt);
	}
	enableredir = 1;
	switch (xlrfork(RFFDG|RFPROC|RFNAMEG, "io")) {
	case -1:
		sysfatal("fork: %r");
	case 0:
		close(p[1]);
		io();
		break;
	default:
		close(p[0]);	/* don't deadlock if child fails */
		if(defmnt && mount(p[1], -1, defmnt, MREPL|MCREATE, "") < 0)
			sysfatal("mount failed: %r");
	}
	if (Xflag)
		for (;;)
			sleep(1000*1000);
	xlexits(nil);
}

static 
char*
rversion(Fid*)
{
	Fid *f;

	qlock(&fidlck);
	for(f = fids; f; f = f->next)
		if(f->busy)
			rclunk(f);
	qunlock(&fidlck);
	if(thdr.msize > sizeof mdata)
		rhdr.msize = sizeof mdata;
	else
		rhdr.msize = thdr.msize;
	messagesize = rhdr.msize;
	if(strncmp(thdr.version, "9P2000", 6) != 0)
		return Eversion;
	rhdr.version = "9P2000";
	return 0;
}

static 
char*
rauth(Fid*)
{
	return "xlatefs: no authentication required";
}

static 
char*
rflush(Fid *f)
{
	USED(f);
	return 0;
}

static 
char*
rattach(Fid *f)
{
	/* no authentication! */
	f->busy = 1;
	f->rclose = 0;
	f->inode = &root;
	getinode(&root);
	rhdr.qid = f->inode->qid;
	if (thdr.uname[0])
		f->user = estrdup(thdr.uname);
	else
		f->user = "none";
	if (strcmp(user, "none") == 0)
		user = f->user;
	return 0;
}

static 
char*
clone(Fid *f, Fid **nf)		/* Called with fslck held */
{
	if(f->open)
		return Eisopen;
	if(f->inode->dead)
		return Enotexist;
	*nf = newfid(thdr.newfid);
	(*nf)->busy = 1;
	(*nf)->open = 0;
	(*nf)->rclose = 0;
	(*nf)->inode = f->inode;
	getinode(f->inode);
	(*nf)->user = f->user;
	return 0;
}

static 
char*
rwalk(Fid *f)
{
	Inode *ip, *fip, *parent;
	char *name;
	Fid *nf;
	ulong t;
	int i;

	u->err = nil;
	nf = nil;
	rhdr.nwqid = 0;
	qlock(&fslck);
	if (thdr.newfid != thdr.fid){
		u->err = clone(f, &nf);
		if (u->err) {
			qunlock(&fslck);
			return u->err;
		}
		f = nf;	/* walk the new fid */
	}
	fip = f->inode;
	if (thdr.nwname > 0){
		t = time(0);
		for (i = 0; i < thdr.nwname && i < MAXWELEM; i++){
			if ((fip->qid.type & QTDIR) == 0){
				u->err = Enotdir;
 				break;
			}
			if (fip->dead){
				u->err = Enotexist;
				break;
			}
			fip->atime = t;
			name = thdr.wname[i];
			if (strcmp(name, ".") == 0){
	Found:			rhdr.nwqid++;
				rhdr.wqid[i] = fip->qid;
				continue;
			}
			parent = fip->parent;
			if (!perm(f, parent, Pexec)){
				u->err = Eperm;
				break;
			}
			if (strcmp(name, "..") == 0){
				fip = parent;
				goto Found;
			}
			ip = fip->chash[namehash(name)];
			for (; ip; ip = ip->cnext) {
				if (strcmp(name, ip->name) == 0) {
					fip = ip;
					goto Found;
				}
			}
			break;
		}
		if (i==0 && u->err == nil)
			u->err = Enotexist;
	}
	if (nf != nil && (u->err != nil || rhdr.nwqid < thdr.nwname)){
		/* clunk the new fid, which is the one we walked */
		f->busy = 0;
		putinode(f->inode);
		f->inode = nil;
	}
	if (rhdr.nwqid > 0)
		u->err = nil;	/* didn't get everything in 9P2000 right! */
	if (rhdr.nwqid == thdr.nwname) {	/* update the fid after a successful walk */
		putinode(f->inode);
		getinode(fip);
		f->inode = fip;
	}
	qunlock(&fslck);
	return u->err;
}

static 
char *
ropen(Fid *f)
{
	Inode *ip;
	int mode;

	if (f->open)
		return Eisopen;
	ip = f->inode;
	qlock(&fslck);
	if (ip->dead) {
		qunlock(&fslck);
		return Enotexist;
	}
	mode = thdr.mode;
	if (ip->qid.type & QTDIR) {
		if(mode != OREAD) {
			qunlock(&fslck);
			return Eperm;
		}
		rhdr.qid = ip->qid;
		qunlock(&fslck);
		return 0;
	}
	if (mode & ORCLOSE){
		/* can't remove root; must be able to write parent */
		if (ip->qid.path == 0 || !perm(f, ip->parent, Pwrite)) {
			qunlock(&fslck);
			return Eperm;
		}
		f->rclose = 1;
	}
	mode &= OPERM;
	if (mode == OWRITE || mode == ORDWR)
		if (!perm(f, ip, Pwrite)) {
			qunlock(&fslck);
			return Eperm;
		}
	if (mode == OREAD || mode == ORDWR)
		if (!perm(f, ip, Pread)) {
			qunlock(&fslck);
			return Eperm;
		}
	if (mode == OEXEC)
		if (!perm(f, ip, Pexec)) {
			qunlock(&fslck);
			return Eperm;
		}
	rhdr.qid = ip->qid;
	rhdr.iounit = messagesize-IOHDRSZ;
	f->open = 1;
	ip->open++;
	qunlock(&fslck);
	return 0;
}

static 
char *
rcreate(Fid *)
{
	return "xlatefs: can't create";
}

static 
char*
rread(Fid *f)
{
	Inode *ip;
	uchar *buf;
	vlong off;
	int n, m, cnt;

	n = 0;
	rhdr.count = 0;
	rhdr.data = (char*)rdata;
	if (thdr.offset < 0)
		return "negative seek offset";
	off = thdr.offset;
	buf = rdata;
	cnt = thdr.count;
	if(cnt > messagesize)	/* shouldn't happen, anyway */
		cnt = messagesize;
	if(cnt < 0)
		return "negative read count";
	qlock(&fslck);
	if(f->inode->dead) {
		qunlock(&fslck);
		return Enotexist;
	}
	if(f->inode->qid.type & QTDIR){
		if (off == 0)
			if (f->dirread = f->inode->children)
				getinode(f->dirread);
		ip = f->dirread;
		if (ip == nil) {
			rhdr.count = 0;
			qunlock(&fslck);
			return 0;
		}
		if (ip->dead) {
			putinode(f->dirread);
			f->dirread = nil;
			qunlock(&fslck);
			return Ephase;
		}
		putinode(f->dirread);	/* ref held from previous call */
		for (; ip && n < cnt; ip = ip->next, f->dirread = ip) {
			m = inodestat(ip, buf + n, cnt - n);
			if (m == 0)
				break;
			n += m;
		}
		if (f->dirread)
			getinode(f->dirread);	/* keep pointer valid */
		rhdr.count = n;
		qunlock(&fslck);
		return 0;
	}
	qunlock(&fslck);
	ip = f->inode;
	if (ip->read == nil)
		return "no read function";
	u->err = nil;
	rhdr.count = (*ip->read)(ip->arg, rdata, cnt, off);
	assert((int)rhdr.count >= 0);
	rhdr.data = (char*)rdata;
	ip->atime = time(0);
	return u->err;	
}

static 
char*
rwrite(Fid *f)
{
	Inode *ip;
	vlong off;
	int cnt;

	ip = f->inode;
	rhdr.count = 0;
	if (thdr.offset < 0)
		return "negative seek offset";
	off = thdr.offset;
	cnt = thdr.count;
	if(cnt < 0)
		return "negative write count";
	qlock(&fslck);
	if(ip->dead) {
		qunlock(&fslck);
		return Enotexist;
	}
	if(ip->qid.type & QTDIR) {
		qunlock(&fslck);
		return Eisdir;
	}
	if (ip->write == nil) {
		qunlock(&fslck);
		return "no write function";
	}
	ip->atime = time(0);
	ip->qid.vers++;
	qunlock(&fslck);
	u->err = nil;
	rhdr.count = (*ip->write)(ip->arg, thdr.data, cnt, off);
	return u->err;	
}

static 
char *
rclunk(Fid *f)
{
	qlock(&fslck);
	if(f->open)
		f->inode->open--;
	putinode(f->inode);
	f->inode = nil;
	if (f->dirread) {
		putinode(f->dirread);
		f->dirread = nil;
	}
	qunlock(&fslck);
	f->busy = 0;
	f->open = 0;
	return nil;
}

static 
char *
rremove(Fid *)
{
	return "xlatefs: can't remove";
}

static 
char *
rstat(Fid *f)
{
	qlock(&fslck);
	if (f->inode->dead) {
		qunlock(&fslck);
		return Enotexist;
	}
	rhdr.nstat = inodestat(f->inode, statbuf, sizeof statbuf);
	rhdr.stat = statbuf;
	qunlock(&fslck);
	return 0;
}

static 
char *
rwstat(Fid *)
{
	return "xlatefs: can't wstat";
}

static 
uint
inodestat(Inode *ip, uchar *buf, uint nbuf)
{
	int n;
	Dir dir;

	memset(&dir, 0, sizeof dir);
	dir.name = ip->name;
	dir.qid = ip->qid;
	dir.mode = ip->perm;
	dir.length = ip->length;
	dir.uid = ip->user;
	dir.gid = ip->group;
	dir.muid = ip->muid;
	dir.atime = ip->atime;
	dir.mtime = ip->mtime;
	n = convD2M(&dir, buf, nbuf);
	if(n > 2)
		return n;
	return 0;
}

static 
Fid *
newfid(int fid)
{
	Fid *f, *ff;

	ff = 0;
	qlock(&fidlck);
	for (f = fids; f; f = f->next)
		if (f->fid == fid) {
			qunlock(&fidlck);
			return f;
		} else if (!ff && !f->busy)
			ff = f;
	if (ff) {
		ff->fid = fid;
		qunlock(&fidlck);
		return ff;
	}
	f = emalloc(sizeof *f);
	f->inode = f->dirread = nil;
	f->fid = fid;
	f->next = fids;
	fids = f;
	qunlock(&fidlck);
	return f;
}

static
void
startremotefs(void)
{
	switch (fork()) {
	case -1:
		xsyslog("Failure to start remotefs process: %r\n");
		break;
	case 0:
		execl("/bin/remotefs", "remotefs", nil);
		xsyslog("Failure to execute remotefs: %r\n");
		xlexits(nil);
	default:
		waitpid();
	}
}

static
void
startrrfs(void)
{
	switch (fork()) {
	case -1:
		xsyslog("Failure to start rrfs process: %r\n");
		break;
	case 0:
		execl("/bin/rrfs", "rrfs", "/n/kfs/conf/rr", nil);
		xsyslog("Failure to execute rrfs: %r\n");
		xlexits(nil);
	default:
		waitpid();
	}
}

static 
void
io(void)
{
	char buf[40];
	int n, pid;
	Fid *fid;
	int type;	/* for debugging */

	xsyslog("");	/* set xsyslog's fd before rfork(RFFDG) */
	xlmaintmode(1);
	startrrfs();
	startremotefs();
	resinit();
	xtntcopyinit();
	if (startctl() == 0 || shutdown) {
		shutitdown();
		xlexits(0);
	}
	readconfig();	/* from the file */
	setprompt();
	targinit();
	schedinit();
	pid = getpid();
	notify(fsnotifyf);

	for (;;) {
		/*
		 * reading from a pipe or a network device
		 * will give an error after a few eof reads.
		 * however, we cannot tell the difference
		 * between a zero-length read and an interrupt
		 * on the processes writing to us,
		 * so we wait for the error.
		 *  XXX is this still true?
		 */
		if (shutdown) {
			shutitdown(); // called again, potential race
			xlexits(0);
		}
		memset(&thdr, 0, sizeof thdr);
		memset(&rhdr, 0, sizeof rhdr);
		u->err = nil;
		n = read9pmsg(mfd[0], mdata, messagesize);
if (debug) print("xlate read9pmsg: %d\n", n);
		if (n < 0){
			rerrstr(buf, sizeof buf);
			if (buf[0] == '\0' || strstr(buf, "hungup")) {
				if (debug)
					fprint(2, "xlatefs: signing off\n");
/**/				print("xlatefs: xlexits\n");
				xlexits("");
			} else if (strstr(buf, "interrupt"))
				continue;
			if (shutdown)
				xlexits(nil);
			else
				sysfatal("mount read: %r");
		}
		if (n == 0)
			continue;
		if (convM2S(mdata, n, &thdr) == 0) {
			if (debug)
				print("xlate convM2S failure: %r\n");
			continue;
		}
		if (debug)
			fprint(2, "xlatefs %d:<-%F\n", pid, &thdr);
		type = rhdr.type; USED(type);
		if (thdr.type<0 || thdr.type >= nelem(fcalls) || !fcalls[thdr.type])
			u->err = "bad fcall type";
		else if (((fid = newfid(thdr.fid)) == nil || !fid->inode) 
		     && needfid[thdr.type])
			u->err = "fid not in use";
		else {
			alarm(alarmingsecs*1000);
			u->err = (*fcalls[thdr.type])(fid);
			alarm(0);
		}
		if (u->err) {
			rhdr.type = Rerror;
			rhdr.ename = u->err;
		} else {
			rhdr.type = thdr.type + 1;
			rhdr.fid = thdr.fid;
		}
		rhdr.tag = thdr.tag;
		if (debug)
			fprint(2, "xlatefs %d:->%F\n", pid, &rhdr);/**/
		n = convS2M(&rhdr, mdata, messagesize);
		if (n == 0)
			sysfatal("convS2M error on write: %r");
if (debug) print("xlate write\n");
		if (write(mfd[1], mdata, n) != n)
			sysfatal("mount write: %r");
if (debug) print("xlate write complete\n");
	}
}

static 
int
perm(Fid *f, Inode *ip, int p)
{
	if ((p * Pother) & ip->perm)
		return 1;
	if (strcmp(f->user, ip->group) == 0 && ((p * Pgroup) & ip->perm))
		return 1;
	if (strcmp(f->user, ip->user) == 0 && ((p * Powner) & ip->perm))
		return 1;
	return 0;
}

static 
void *
emalloc(ulong n)
{
	void *p;

	p = mallocz(n, 1);
	if (!p)
		sysfatal("out of memory: %r");
	return p;
}

static 
void *
erealloc(void *p, ulong n)
{
	p = realloc(p, n);
	if (!p)
		sysfatal("out of memory: %r");
	return p;
}

static 
char *
estrdup(char *q)
{
	char *p;
	int n;

	n = strlen(q)+1;
	p = mallocz(n, 1);
	if(!p)
		sysfatal("out of memory: %r");
	memmove(p, q, n);
	return p;
}

static 
void
usage(void)
{
	fprint(2, "usage: %s [-Dipsu] [-m mountpoint] [-S srvname][-c conffile]\n", 
		argv0);
	xlexits("usage");
}

static
void
getinode(Inode *p)
{
	incref(p);
}

static
void
putinode(Inode *p)	/* Called with fslck held */
{
	if (decref(p) == 0)
		freeinode(p);
}

static
void
freeinode(Inode *p)	/* Called with fslck held */
{
	free(p->chash);
	free(p->name);
	p->chash = nil;
	p->name = nil;
	p->next = freeinodes;
	freeinodes = p;
}

static
Inode *
allocinode(void)	/* Called with fslck held */
{
	Inode *p, *x, *e;

	if (p = freeinodes) {
		freeinodes = p->next;
		goto e;
	}
	x = mallocz(sizeof *p * Ninode, 1);
	if (x == nil) {
		xsyslog("allocinode: cannot allocate any more memory!\n");
		return nil;
	}
	e = x + Ninode;
	p = x++;
	for (; x<e; x++) {
		x->next = freeinodes;
		freeinodes = x;
	}
e:	getinode(p);
	return p;
}

static
void
resetinode(Inode *p)	/* Called with fslck held */
{
	p->parent = nil;
	p->open = 0;
	p->qid = (Qid){0, 0, 0};
	p->perm = 0;
	p->name = 0;
	p->atime = p->mtime = 0;
	p->user = p->group = p->muid = 0;
	p->arg = 0;
	p->read = 0;
	p->write = 0;
	p->length = 0;
	p->dead = 0;
	p->chash = 0;
}

/* djb2 */
static
ulong
namehash(char *name)
{
	unsigned long hash;
	int c;

	hash = 5381;
	while (c = *name++)
		hash = ((hash << 5) + hash) + c;
	return hash % Nchash;
}

static 
Inode *
newinode(char *name, Inode *parent, int prm)	/* Called with fslck held */
{
	Inode *p;
	int i;

	if (parent == nil) {
		xsyslog("newinode: %s: nil parent\n", name);
		return nil;
	}
	p = allocinode();
	if (p == nil)
		return nil;
	resetinode(p);

	p->qid.path = ++path;
	p->qid.vers = 0;
	if(prm & DMDIR)
		p->qid.type |= QTDIR;
	p->name = estrdup(name);
	p->user = parent->user;
	p->group = parent->group;
	if (prm & DMDIR) {
		prm = (prm&~0777) | (parent->perm&prm&0777);
		p->chash = mallocz(Nchash * sizeof (Inode *), 1);
		if (p->chash == nil) {
			print("chash allocation failure for %s\n", name);
			putinode(p);
			return nil;
		}
	} else
		prm = (prm&(~0777|0111)) | (parent->perm&prm&0666);
	p->perm = prm;
	p->atime = time(0);
	p->mtime = p->atime;
	p->parent = parent;
	p->next = parent->children;
	parent->children = p;
	i = namehash(name);
	p->cnext = parent->chash[i];
	parent->chash[i] = p;
	return p;
}

Inode *
newfile(Inode *parent, char *name, int perm, vlong length, Iop read, Iop write, void *arg)
{
	Inode *p;
	
	qlock(&fslck);
	p = newinode(name, parent, perm);
	if (p == nil) {
		qunlock(&fslck);
		return nil;
	}
	p->length = length;
	p->read = read;
	p->write = write;
	p->arg = arg;
	qunlock(&fslck);
	return p;
}

static
int
rdhash(void *ip, void *a, int count, vlong off)
{
	int i, min, max, sum, n;
	Inode *p, *xp;

	if (off)
		return 0;
	p = ip;
	sum = min = max = 0;
	for (i=0; i < Nchash; i++) {
		xp = p->chash[i];
		n = 0;
		for (; xp; xp=xp->cnext)
			n++;
		if (i == 0 || n < min)
			min = n;
		if (i == 0 || n > max)
			max = n;
		sum += n;
	}
	return snprint(a, count, "min: %d\nmax: %d\navg: %d\n",
		min, max, sum / Nchash);
}

Inode *
newdir(Inode *parent, char *name, int perm)
{
	Inode *p;

	qlock(&fslck);
	/* check for filename dups */
	p = parent->children;
	for (; p; p = p->next)
		if (strcmp(p->name, name) == 0) {
			qunlock(&fslck);
			return nil;
		}
	p = newinode(name, parent, DMDIR|perm);
	qunlock(&fslck);
//	xp = newfile(p, "hash", 0444, 0, rdhash, nowrite, nil);
//	xp->arg = p;
	return p;
}

/* remove child Inode from its parent tracking */
static
void
unchild(Inode *chld)	/* Called with fslck held */
{
	Inode *p, **pp;

	/* remove from parent child list */
	pp = &chld->parent->children;
	for (; p = *pp; pp = &p->next)
		if (p == chld)
			break;
	if (p) {
		*pp = p->next;
		p->next = nil;
	} else
		print("deldir: warning: %s not found in parent %s\n",
			chld->name, chld->parent->name);
	/* remove from parent child hash */
	pp = &chld->parent->chash[namehash(chld->name)];
	for (; p = *pp; pp = &p->cnext)
		if (p == chld)
			break;
	if (p) {
		*pp = p->cnext;
		p->cnext = nil;
	} else
		print("deldir: warning: %s not found in parent %s chash\n",
			chld->name, chld->parent->name);
}

void
deldir(Inode *parent)	/* rm -rf dir */
{
	Inode *p;

	if (parent == nil) {
		xsyslog("Warning: nil parent passed to deldir\n");
		return;
	}
	qlock(&fslck);
	while (p = parent->children) {
		parent->children = p->next;
		p->dead = 1;
		putinode(p);
	}
	parent->dead = 1;
	unchild(parent);
	putinode(parent);
	qunlock(&fslck);
}

void
panic(char *fmt, ...)	/* print message and break */
{
	Fmt f;
	char buf[64];
	va_list arg;
	
	fmtfdinit(&f, 1, buf, sizeof buf);
	fmtprint(&f, "panic: ");
	va_start(arg, fmt);
	fmtvprint(&f, fmt, arg);
	va_end(arg);
	fmtprint(&f, "\n");
	fmtfdflush(&f);
	abort();
}

int
xlrfork(int flags, char *namefmt, ...)
{
	int n;
	va_list arg;

	n = rfork(flags);
	if (n == 0) {
		va_start(arg, namefmt);
		u->name = vsmprint(namefmt, arg);
		va_end(arg);
		u->pid = getpid();
		u->tag = tagalloc();
		u->err = nil;
	}
	return n;
}

void
xlexits(char *e)
{
	free(u->name);
	tagfree(u->tag);
	exits(e);
}

/* Soli Deo Gloria */
/* Brantley Coile */
