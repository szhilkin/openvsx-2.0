#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <libsec.h>
#include <bio.h>
#include <pool.h>
#include <libcutil.h>
#include "dat.h"
#include "fns.h"
#include "shadow.h"

enum 
{
	CMmkpool = 0,		/* create a new pool */
	CMlabpool,		/* label a pool */
	CMrmpool,		/* ditch a pool */
	CMdestroypool,		/* destroy a pool and all contents */
	CMaddpv,		/* add a pv to the pool */
	CMlabpv,		/* add a label to a pv */
	CMrmpv,			/* take a pv out of a pool */
	CMclrpvmeta,		/* clear pv metadata */
	CMoospv,		/* out of service a pv */
	CMmigrate,		/* migrate off a pv */
	CMispv,			/* inservice a pv */
	CMlegacy,		/* add legacy pv/lv */
	CMpromote,		/* promote a mirror secondary to primary (swap 'em) */
	CMmode,			/* set pool allocation mode */
	
	CMmklv,			/* create a new logical volume */
	CMmklvthin,		/* create a new thin logical volume */
	CMmvlv,			/* rename a logical volume */
	CMrmlv,			/* remove a logical volume */
	CMclone,		/* create a read-write snapshot */
	CMmask,			/* add mac mask list to lv */
	CMresize,		/* resize lv */
	CMrollback,		/* revert to an earlier snapshot */
	CMresetserial,		/* Reset the serial of the lv */
	CMclrres,		/* Clear all reservations */
	CMthin,			/* Make a thick LV thin */
	CMthick,		/* Make a thin LV thick */
	CMprevsnap,		/* Set prevsnap value for LV */
	CMfixsnaplist,		/* Fix prevsnap values for LVs */
	
	CMmklun,		/* assign a logical volume to a lun */
	CMoffline,		/* hide lun from outside world */
	CMonline,		/* put it back */
	CMreadonly,		/* make lun readonly */
	CMrdwr,			/* make lun readwrite */
	CMrmlun,		/* disassociate volume from lun */
	
	CMdumplv,		/* dump the lvt for a volume: DEBUG */
	
	CMunmirror,		/* remove a mirror */
	CMbrkmirror,		/* break a mirror */
	CMmirror,		/* add a target to pv */
	CMxmirror,		/* add mirror target, no resync */
	CMunshadow,		/* remove shadowrecv or shadowsend */
	CMshadowrecv,		/* shadowrecv */
	CMshadowsend,		/* shadowsend */
	CMshadowdebug,		/* shadow debug */
	CMmaxshadows,		/* limit concurrent shadows */
	CMtagdebug,		/* tagio debug */
	CMdosnapcal,		/* force snapshot usage calculation */
	CMlvmddebug,		/* LV metadata debug flag */
	CMisnotremote,		/* check name not used for shadow remote */
	CMdebugpvpfree,		/* DEBUG: print out etotal and efree and check against reality */
	CMfakextntiowr,
	CMfakextntiord,
	
	CMmaintmode,		/* configure I/O timeouts */
	CMiotimeo,		/* undoc'd extension to simple maintenance mode */
	CMsnaplimit,		/* scheduled snapshot size limit */
	CMsnapsched,		/* Set a snapshot schedule */
	CMclrsnapsched,		/* Clear a snapshot schedule */
	CMsethold,		/* Set hold on a snapshot */
	CMclrhold,		/* Clear hold on a snapshot */
	CMhasmeta,		/* Check if metadata exists on target */
	CMupdatelegacy,		/* update old legacy PV format */
	CMrestorepool,		/* create pool with PVs containing LVs */
	CMflush,		/* force write of metadata to disk*/
	CMmemchk,		/* Verify heap integrity */
	CMmemflag,		/* Set mainmem memory flags */
	CMmemstir,		/* Stir up heap memory to flush out problems */
	CMenableredir,		/* Enable or disable redirect */
	CMlogstale,		/* Log stale target MACs */
	CMprobetarg,		/* Probe target */
};

static	Cmdtab ctltab[] = {
	{ CMmkpool, 	"mkpool", 	2 },	/* mkpool poolname  */
	{ CMrmpool, 	"rmpool",	2 },	/* rmpool poolname */
	{ CMdestroypool,	"destroypool",	2},	/* destroypool poolname */
	{ CMaddpv, 	"addpv",	2 },	/* addpv target */
	{ CMlabpv, 	"labpv",	2 },	/* labpv label */
	{ CMrmpv, 	"rmpv",		2 },	/* rmpv pvname */
	{ CMclrpvmeta, 	"clrpvmeta",	2 },	/* clrpvmeta pvname */
	{ CMoospv, 	"oospv",	2 },	/* oospv pvname */
	{ CMmigrate, 	"migrate",	2 },	/* migrate pvname */
	{ CMispv, 	"ispv",		2 },	/* ispv poolname */
	{ CMmklv,	"mklv",		3 },	/* mklv lvname size */
	{ CMmklvthin,	"mklvthin",	3 },	/* mklvthin lvname size */
	{ CMmvlv,	"mvlv",		3 },	/* mvlv lvname.old lvname.new */
	{ CMlegacy,	"addleg",	3 }, 	/* addleg lvname target  */
	{ CMpromote,	"promote",	2 },	/* promote target */
	{ CMmode,	"mode",		2 },	/* mode [striped | concat] */
	{ CMrmlv,	"rmlv",		2 },	/* rmlv lvname */
	{ CMclone,	"clone",	2 },	/* clone new*/
	{ CMmklun, 	"mklun", 	2 },	/* mklun lun */
	{ CMmask,	"mask",		0 },	/* mask [+-mac] ... */
	{ CMresize,	"resize",	2 },	/* resize size */
	{ CMrollback,	"rollback", 1 },	/* rollback snapshot */
	{ CMresetserial,	"resetserial", 1},	/* reset the serial number of the lv */
	{ CMclrres,	"clrreservation",	1 },	/* clrreservation */
	{ CMthin,	"thin",		1 },	/* thinlv */
	{ CMthick,	"thick",	1 },	/* thicklv */
	{ CMprevsnap,	"prevsnap",	2 },	/* prevsnap N */
	{ CMfixsnaplist,	"fixsnaplist",	1},	/* fixsnaplist  */
	{ CMoffline,	"offline",	1 },	/* offline */
	{ CMonline,	"online",	1 },	/* online */
	{ CMreadonly,	"readonly",	1 },	/* readonly */
	{ CMrdwr,	"readwrite",	1 },	/* readwrite */
	{ CMrmlun,	"rmlun", 	1 },	/* rmlun */
	{ CMdumplv,	"dumplv",	1 },	/* dumplv */
	{ CMunmirror,	"unmirror", 	2 },	/* unmirror pvname */
	{ CMbrkmirror,	"brkmirror", 	2 },	/* brkmirror pvname */
	{ CMmirror,	"mirror",	3 },	/* mirror pvname target */
	{ CMxmirror,	"cleanmirror",	3 },	/* cleanmirror pvname target (no silver) */
	{ CMunshadow,	"unshadow",	1 },	/* clear shadow */
	{ CMshadowrecv,	"shadowrecv",	4 },	/* shadowrecv */
	{ CMshadowsend,	"shadowsend",	3 },	/* shadowsend */
	{ CMshadowdebug,"shadowdebug",	2 },	/* shadow debug */
	{ CMmaxshadows,	"maxshadows",	2 },	/* limit shadow conns */
	{ CMtagdebug,	"tagdebug",	2 },	/* tagio debug */
	{ CMdosnapcal,	"dosnapcal",	2 },	/* dosnapcal N */
	{ CMlvmddebug,	"lvmddebug",	2 },	/* flvedebug N */
	{ CMfakextntiord, "fakextntiord", 2 },	/* fake xtntio read out */
	{ CMfakextntiowr, "fakextntiowr", 2 },	/* fake xtntio write out */
	{ CMisnotremote,"isnotremote",	2 },	/* name not used for remote */
	{ CMdebugpvpfree, "debugpvpfree", 1 },	/* check etotal and efree with reality */
	{ CMmaintmode,	"maintenancemode", 1},	/* readonly */
	{ CMiotimeo,	"iotimeo",	2},	/* set iotimeo value, bypass maint mode */
	{ CMsnaplimit,	"snaplimit", 2},	/* snaplimit size  */
	{ CMsnapsched,	"snapsched", 4},	/* snapsched class time retain */
	{ CMclrsnapsched,	"clrsnapsched", 3},	/* clrsnapsched class time */
	{ CMsethold,	"sethold", 1},	/* sethold */
	{ CMclrhold,	"clrhold", 1},	/* clrhold */
	{ CMhasmeta,	"hasmeta",	2 },	/* check if metadata presents at target */
	{ CMupdatelegacy, "updatelegacy", 1 },	/* updatelegacy */
	{ CMrestorepool, "restorepool", 0 },	/* restorepool targ ... */
	{ CMflush, "flush", 0 },		/* flush */
	{ CMmemchk, "memchk", 0 },		/* memchk */
	{ CMmemflag, "memflag", 2 },		/* memflag flag, see pool(2) */
	{ CMmemstir, "memstir", 2 },		/* stir heap memory */
	{ CMenableredir, "enableredir", 2 },	/* enableredir N */
	{ CMlogstale,	"logstale",	2 },	/* log stale target MACs */
	{ CMprobetarg, 	"probetarg",	4 },	/* probetarg t offset maxwait */
};

int tagdebug = 1;
int logstale = 1;

static	int	wrsleep(void*, void*, int, vlong);
static	void	saveproc(void);
static	void	pvsyslog(void);
static	void	poolpctsyslog(void);
static	void	lvsyslog(void);
static	void	legsyslog(void);
static	void	sanityproc(void);
static  void	isnotremote(char *);

static void
wassert(char *cmd)
{
	if (u->nerrlab != 0) {
		xsyslog(LOGCOM "wassert on %s\n", cmd);
		abort();
	}
}

int
startctl(void)	/* build file system */
{
	initxlock();
	if (xlrfork(RFPROC|RFMEM, "mirproc") == 0)
		mirproc();
	if (xlrfork(RFPROC|RFMEM, "shadowsendproc") == 0)
		shadowsendproc();
	if (xlrfork(RFPROC|RFMEM, "shadowrecvproc") == 0)
		shadowrecvproc();
	if (xlrfork(RFPROC|RFMEM, "saveproc") == 0)
		saveproc();
	if (xlrfork(RFPROC|RFMEM, "filtertimer") == 0)
		filtertimer();
	if (xlrfork(RFPROC|RFMEM, "pvsyslog") == 0)
		pvsyslog();
	if (xlrfork(RFPROC|RFMEM, "poolpctsyslog") == 0)
		poolpctsyslog();
	if (xlrfork(RFPROC|RFMEM, "sanityproc") == 0)
		sanityproc();
	if (xlrfork(RFPROC|RFMEM, "lvsyslog") == 0)
		lvsyslog();
	if (xlrfork(RFPROC|RFMEM, "legsyslog") == 0)
		legsyslog();
	newfile(&root, "ctl", 0222, 0, noread, wrctl, nil);
	newfile(&root, "status", 0444, 0, rdstatus, nowrite, nil);
	newfile(&root, "work", 0444, 0, rdwork, nowrite, nil);
	newfile(&root, "perf", 0444, 0, rdperf, nowrite, nil);
//	newfile(&root, "sleep", 0222, 0, noread, wrsleep, nil);
	pooldir = newdir(&root, "pool", 0777);
	lvdir = newdir(&root, "lv", 0777);
	lundir = newdir(&root, "lun", 0777);
	targdir = newdir(&root, "targ", 0777);

	return (pooldir != nil && lvdir != nil
		&& lundir != nil && targdir != nil);
}

void
newpool(PVPool *pvp, char *name)
{
	Inode *dir;

	dir = newdir(pooldir, name, 0777);
	if (dir == nil) {
		xsyslog("Pool %s: failed to create directory\n", name);
		return;
	}
	newfile(dir, "status", 	0444, 0, poolstatus, nowrite, pvp);
	newfile(dir, "ctl", 	0666, 0, pooldmp, poolctl, pvp);
	newfile(dir, "label", 	0666, 0, rdpoollab, wrpoollab, pvp);
	newfile(dir, "lvs",	0444, 0, rdpoollvs, nowrite, pvp);
	newfile(dir, "split", 	0444, 0, poolsplit, nowrite, pvp);
	pvp->subdir = newdir(dir, "pv", 0777);
	pvp->dir = dir;
}

void 
newpv(PV *pv)
{
	Inode *dir;
	char buf[16];
	PVPool *pvp;

	pvp = pv->pool;
	snprint(buf, sizeof buf, "%d", pv->id);
	dir = newdir(pvp->subdir, buf, 0777);
	if (dir == nil) {
		xsyslog("Failed to create directory for PV %s\n", buf);
		return;
	}
	addfilter(&pv->rf);
	addfilter(&pv->wf);
	pv->sysalrt = 0;
	pv->statsamp = pv->iopssamp = 4;
	newfile(dir, "data", 	0666, pv->length, pvread, pvwrite, pv);
	newfile(dir, "status", 	0444, 0, pvstatus, nowrite, pv);
	newfile(dir, "ctime", 	0444, 0, pvctime, nowrite, pv);
	newfile(dir, "histo", 	0444, 0, pvhisto, nowrite, pv);
	newfile(dir, "pvt", 	0444, 0, pvtread, nowrite, pv);
	newfile(dir, "label", 	0666, 0, rdpvlab, wrpvlab, pv);
	newfile(dir, "pv", 	0444, 0, rdpv, nowrite, pv);
	newfile(dir, "iostats", 0666, 0, rdpvstats, wrpvstats, pv);
	newfile(dir, "iops", 0666, 0, rdpviops, wrpviops, pv);
	pv->dir = dir;
}

void
newlv(LV *l)
{
	Inode *dir;
	int mode;
	
	mode = (l->mode & LVSNAP) ? 0444 : 0666;
	dir = newdir(lvdir, l->name, 0777);
	if (dir == nil) {
		xsyslog("Failed to create directory for LV %s\n", l->name);
		return;
	}
	addfilter(&l->rf);
	addfilter(&l->wf);
	l->statsamp = l->iopssamp = 4;
	newfile(dir, "data", 	mode, l->length, lunread, lunwrite, l);
	newfile(dir, "ctl", 	0666, 0, rdlvctl, lvctl, l);
	newfile(dir, "status", 	0444, 0, lvstatus, nowrite, l);
	newfile(dir, "ctime", 	0444, 0, lvctime, nowrite, l);
	newfile(dir, "label", 	0666, 0, rdlvlab, wrlvlab, l);
	newfile(dir, "mask", 	0666, 0, rdmask, nowrite, l);
	newfile(dir, "reserve", 0666, 0, rdres, nowrite, l);
	newfile(dir, "pool",	0444, 0, rdlvpool, nowrite, l);
	newfile(dir, "iostats", 0666, 0, rdlvstats, wrlvstats, l);
	newfile(dir, "iops", 0666, 0, rdlviops, wrlviops, l);
	newfile(dir, "serial", 0666, 0, rdlvserial, wrlvserial, l);
	newfile(dir, "snaplimit", 0444, 0, rdsnaplimit, nowrite, l);
	newfile(dir, "snapsched", 0444, 0, rdsnapsched, nowrite, l);
	newfile(dir, "lvt", 0444, 0, rdlvt, nowrite, l);
	newfile(dir, "snap", 0444, 0, rdsnap, nowrite, l);
	newfile(dir, "extents", 0444, 0, rdlvexts, nowrite, l);

	l->dir = dir;
	
}

void
newlun(LV *l)
{
	Inode *dir;
	char buf[32];
	
	snprint(buf, sizeof buf, "%d", l->lun);
	dir = newdir(lundir, buf, 0777);
	if (dir == nil) {
		xsyslog("Failed to create directory for LUN %s\n", buf);
		return;
	}
	newfile(dir, "data", 	0666, l->length, lunread, lunwrite, l);
	l->cfgino = newfile(dir, "config",  0666, l->nqc, confread, confwrite, l);
	newfile(dir, "ctl", 	0222, 0, noread, lunctl, l);
	newfile(dir, "status", 	0444, 0, lunstatus, nowrite, l);
	l->lundir = dir;
}

int
nowrite(void *, void *, int count, vlong)
{
	return count;
}
	
int
noread(void *, void *, int , vlong)
{
	return 0;
}

static char *
isenabled(int enabled)
{
	return enabled ? "enabled" : "disabled";
}

int
rdstatus(void *, void *a, int count, vlong offset)	/* misc stuff */
{
	char buf[8192];

	if (offset)
		return 0;
	/*
	 * msgcount is how many we have created; msgavail is how
	 * many are on the free list.
	 */
	 
	snprint(buf, sizeof buf,
		"msgs: %d\nmsgavail: %d\nmsgfrcount: %d\ntotalext: %uld\n"
		"iotimeo: %uld\nmaxshadows: %d\n"
		"%s: %s\n%s: %s\n%s: %s\n%s: %s\n"	// persistent flags
		"%s: %s\n%s: %s\n%s: %s\n",		// ephemeral flags
		msgcount, msgavail, msgfrcount, totalext, iotimeo,
		maxshadows,
							// persistent flags
		"tagdebug", isenabled(tagdebug),
		"logstale", isenabled(logstale),
		"dosnapcal", isenabled(dosnapcal),
		"lvmddebug", isenabled(lvmddebug),
							// ephemeral flags
		"enableredir", isenabled(enableredir),
		"fakextntioread", isenabled(fakextntioread),
		"fakextntiowrite", isenabled(fakextntiowrite));
	return readstr(offset, a, count, buf);
}
 
int
rdwork(void *, void *a, int count, vlong offset)
{
	uvlong b, total, bytes;
	char buf[8192], *cp, *ep;
	int n;
	LV *lv;

	if (offset)
		return 0;
	memset(buf, 0, sizeof buf);
	cp = buf;
	ep = buf + sizeof buf;

	qlock(&mirrorstat);
	if (mirrorstat.total) {
		n = filtersum(&mirrorstat.filter, &b, nil, nil, 30);
		cp = seprint(cp, ep, "%d %s %T %T %ulld %ulld %ulld\n",
			mirrorstat.wid, "mirror", mirrorstat.src, mirrorstat.dest,
			n ? b / n : 0, mirrorstat.bytes, mirrorstat.total);
	}
	qunlock(&mirrorstat);

	rlock(&lk);
	for (lv = vols; lv; runlock(lv), lv = lv->next) {
		rlock(lv);
		if (!isshadowsend(lv))
			continue;
		if (!lv->pid)
			continue;
		qlock(&lv->quick);
		total = lv->total;
		bytes = lv->bytes;
		qunlock(&lv->quick);
		if (!total)
			continue;
		n = filtersum(&lv->filter, &b, nil, nil, 30);
		cp = seprint(cp, ep,
			     "%d shadow %s.%d %s@%s %ulld %ulld %ulld\n",
			     lv->pid, lv->name, lv->copysnapsend,
			     lv->rmtlv, lv->rmtname,
			     n ? b / n : 0, bytes, total);
	}
	runlock(&lk);

	return readstr(offset, a, count, buf);
}

static int
wrsleep(void*, void *a, int count, vlong)
{
	char buf[32];

	snprint(buf, sizeof buf, "%.*s", count, (char *)a);
	sleep(atoi(buf));
	return count;
}

static int
wrctlx(void *a, int count)
{
	Cmdbuf *cb;
	Cmdtab *ct;
	
	cb = parsecmd((char *)a, count);
	if (cb == nil)
		return 0;
	ct = lookupcmd(cb, ctltab, nelem(ctltab));
	if (ct == nil) {
		u->err = "unknown ctl command";
		free(cb);
		return 0;
	}
	wassert(nil);

	switch (ct->index) {
	case CMmkpool:
		if (xlmkpool(cb->f[1], "") == nil)
			return 0;
		writeconfig();
		xsyslog("Pool %s: created\n", cb->f[1]);
		break;
	case CMrmpool:
		if (xlrmpool(cb->f[1]) < 0)
			return 0;
		writeconfig();
		xsyslog("Pool %s: removed\n", cb->f[1]);
		break;
	case CMdestroypool:
		xldestroypool(cb->f[1]);
		break;
	case CMmaintmode:
		xlmaintmode(0);
		break;
	case CMiotimeo:
		xliotimeo(cb->f[1]);
		break;
	case CMclrpvmeta:
		clrpvmeta(cb);
		break;
	case CMhasmeta:
		hasmeta(cb);
		break;
	case CMupdatelegacy:
		xlupdatelegacy();
		break;
	case CMrestorepool:
		xlrestorepool(cb->nf - 1, cb->f + 1);
		break;
	case CMisnotremote:
		isnotremote(cb->f[1]);
		break;
	case CMtagdebug:
		tagdebug = atoi(cb->f[1]);
		writeconfig();
		break;
	case CMlogstale:
		logstale = atoi(cb->f[1]);
		writeconfig();
		break;
	case CMdosnapcal:
		dosnapcal = atoi(cb->f[1]);
		writeconfig();
		xsyslog("dosnapcal is now %s\n", isenabled(dosnapcal));
		break;
	case CMlvmddebug:
		lvmddebug = atoi(cb->f[1]);
		writeconfig();
		break;
	case CMmemchk:
		poolcheck(mainmem); /* This panics on failure, see pool(2) */
		break;
	case CMmemflag:
		xlmemflag(cb->f[1]);
		break;
	case CMmemstir:
		xlmemstir(atoi(cb->f[1]));
		break;
	case CMenableredir:
		enableredir = atoi(cb->f[1]);
		xsyslog("Redirect is now %s\n", isenabled(enableredir));
		break;
	case CMfakextntiord:
		fakextntioread = atoi(cb->f[1]);
		xsyslog("fakextntioread is now %s\n", isenabled(fakextntioread));
		break;
	case CMfakextntiowr:
		fakextntiowrite = atoi(cb->f[1]);
		xsyslog("fakextntiowrite is now %s\n", isenabled(fakextntiowrite));
		break;
	case CMfixsnaplist:
		xlfixsnaplist();
		break;
	case CMprobetarg:
		probetarg(cb);
		break;
	case CMmaxshadows:
		maxshadows = atoi(cb->f[1]);
		writeconfig();
		break;
	default:
		u->err = "not yet";
		count = 0;
		break;
	}
	wassert(cb->f[0]);

	free(cb);
	return count;
}

int
wrctl(void *, void *a, int count, vlong)
{
	int ret;

	xsyslog(LOGCOM "ctl %.*s\n", endspace(a, count), a);
	ret = wrctlx(a, count);
	if (u->err)
		xsyslog(LOGCOM "ctl error %s\n", u->err);
	return ret;
}

/*
 * dump the binary of the pvpool structure.  for debugging.
 */

int
pooldmp(void *arg, void *a, int count, vlong offset)
{
	return readmem(offset, a, count, arg, sizeof(PVPool));
}

int
readmem(vlong offset, void *a, int count, void *arg, int size)
{
	if (offset >= size)
		return 0;
	if (offset + count > size)
		count = size - offset;
	memmove(a, (uchar *)arg + offset, count);
	return count;
}
	
static int
poolctlx(void *arg, void *a, int count)
{
	Cmdbuf *cb;
	Cmdtab *ct;
	PVPool *pvp;

	pvp = arg;
	cb = parsecmd((char *)a, count);
	if (cb == nil)
		return 0;
	ct = lookupcmd(cb, ctltab, nelem(ctltab));
	if (ct == nil) {
		u->err = "unknown ctl command";
		free(cb);
		return 0;
	}
	wassert(nil);

	switch (ct->index) {
	case CMlabpool:
		xllabpool(pvp, cb->f[1]);
		break;
	case CMaddpv:
		edaddpv(pvp, cb);
		break;
	case CMmirror:
		mirror(pvp, cb, 0);
		break;
	case CMxmirror:
		mirror(pvp, cb, 1);
		break;
	case CMlegacy:
		addleg(pvp, cb);
		break;
	case CMunmirror:
		unmirror(pvp, cb->f[1]);
		break;
	case CMbrkmirror:
		breakmirror(pvp, cb->f[1]);
		break;
	case CMdebugpvpfree:
		debugpvpfree(pvp);
		break;
	case CMmklv:
		xlmklv(pvp, cb->f[1], cb->f[2], 0);
		break;
	case CMmklvthin:
		xlmklv(pvp, cb->f[1], cb->f[2], 1);
		break;
	case CMmvlv:
		xlmvlv(pvp, cb->f[1], cb->f[2]);
		break;
	case CMrmlv:
		xlrmlv(pvp, cb->f[1]);
		break;
	case CMrmpv:
		xlrmpv(pvp, cb->f[1]);
		break;
	case CMpromote:
		mpromote(pvp, cb->f[1]);
		break;
	case CMmode:
		stripe(pvp, cb->f[1]);
		break;
	case CMflush:
		xlflushpvmeta(pvp, cb->f[1]);
		break;
	case CMmigrate:	
	case CMoospv:
	case CMispv:
	default:
		u->err = "not yet";
		count = 0;
	}
	wassert(cb->f[0]);

	free(cb);
	return count;
}

int
poolctl(void *arg, void *a, int count, vlong)
{
	PVPool *pvp;
	int ret;

	pvp = arg;
	xsyslog(LOGCOM "pool/%s/ctl %.*s\n", pvp->name, endspace(a, count), a);
	ret = poolctlx(arg, a, count);
	if (u->err)
		xsyslog(LOGCOM "pool/%s/ctl error %s\n", pvp->name, u->err);
	return ret;
}

static int
lvctlx(void *arg, void *a, int count)
{
	Cmdbuf *cb;
	Cmdtab *ct;
	LV *l;

	l = arg;
	cb = parsecmd((char *)a, count);
	if (cb == nil)
		return 0;
	ct = lookupcmd(cb, ctltab, nelem(ctltab));
	if (ct == nil) {
		u->err = "bad ctl command";
		free(cb);
		return 0;
	}
	rlock(l);
	if (l->flags & LVFsuspended) {
		runlock(l);
		uerr("LV %s suspended", l->name);
		free(cb);
		return 0;
	}
	runlock(l);

	wassert(nil);

	switch (ct->index) {
	case CMclone:
		xlclone(l, cb->f[1]);
		break;
	case CMmklun:
		xlmklun(l, cb->f[1]);
		break;
	case CMrmlun:
		xlrmlun(l);
		break;
	case CMdumplv:
		printlv(l, 1);
		break;
	case CMreadonly:
		xlsetmode(l, OREAD);
		break;
	case CMrdwr:
		xlsetmode(l, ORDWR);
		break;
	case CMunshadow:
		unshadow(l);
		break;
	case CMshadowrecv:
		shadowrecv(l, cb->f[1], cb->f[2], cb->f[3]);
		break;
	case CMshadowsend:
		shadowsend(l, cb->f[1], cb->f[2]);
		break;
	case CMshadowdebug:
		shadowdebug(l, cb->f[1]);
		break;
	case CMmask:
		xlmask(l, cb->nf-1, cb->f+1);
		break;
	case CMresize:
		xlresize(l, cb->f[1]);
		break;
	case CMrollback:
		xlrollback(l);
		break;
	case CMresetserial:
		xlresetserial(l);
		break;
	case CMclrres:
		xlclrres(l);
		break;
	case CMthin:
		xlthin(l);
		break;
	case CMthick:
		xlthick(l);
		break;
	case CMprevsnap:
		xlprevsnap(l, cb->f[1]);
		break;
	case CMsnaplimit:
		xlsnaplimit(l, cb->f[1]);
		break;
	case CMsnapsched:
		xlsnapsched(l, cb->f[1], cb->f[2], cb->f[3]);
		break;
	case CMclrsnapsched:
		xlclrsnapsched(l, cb->f[1], cb->f[2]);
		break;
	case CMsethold:
		xlcanprune(l, 0);
		break;
	case CMclrhold:
		xlcanprune(l, 1);
		break;
	case CMflush:
		xlflushlvmeta(l);
		break;
	// XXX add command to copy clone in background
	default:
		u->err = "not yet";
		count = 0;
	}
	wassert(cb->f[0]);

	free(cb);
	return count;
}

int
lvctl(void *arg, void *a, int count, vlong)
{
	LV *l;
	int ret;

	l = arg;
	xsyslog(LOGCOM "lv/%s/ctl %.*s\n", l->name, endspace(a, count), a);
	ret = lvctlx(arg, a, count);
	if (u->err)
		xsyslog(LOGCOM "lv/%s/ctl error %s\n", l->name, u->err);
	return ret;
}

int
rdlvctl(void *arg, void *a, int count, vlong offset)
{
	LV *lv;
	uchar *buf;
	int size, n, tot;
	
	lv = arg;
	buf = a;
	n = readmem(offset, buf, count, arg, sizeof(LV));
	count -= n;
	buf += n;
	offset -= n;
	tot = n;
	size = lv->frstdat * sizeof(LVE);
	n = readmem(offset, buf, count, lv->lve, size);
	tot += n;
	return tot;
}

static int
rdsnapx(void *arg, void *a, int count, vlong offset)
{
	LV *l, *new;
	uchar *buf;
	int n;
	
	if (offset)
		return 0;
	l = arg;
	buf = a;
	wlock(&lk);
	wlock(l);
	if (l->flags & LVFsuspended) {
		uerr("LV %s suspended", l->name);
		wunlock(l);
		wunlock(&lk);
		return 0;
	}
	if (isshadowrecv(l)) {
		uerr("can't snap shadowrecv LV %s", l->name);
		wunlock(l);
		wunlock(&lk);
		return 0;
	}
	n = snapclone(l, OREAD, time(nil), nil, nil, 0, &new);
	if (n == 0)
		n = readmem(offset, buf, count, new->name, strlen(new->name));
	else
		n = 0;
	wunlock(l);
	wunlock(&lk);
	return n;
}

int
rdsnap(void *arg, void *a, int count, vlong offset)
{
	LV *l;
	int ret;

	l = arg;
	xsyslog(LOGCOM "lv/%s/snap\n", l->name);
	wassert(nil);
	ret = rdsnapx(arg, a, count, offset);
	wassert(nil);
	if (u->err)
		xsyslog(LOGCOM "lv/%s/snap error %s\n", l->name, u->err);
	return ret;
}

static int
lunctlx(void *arg, void *a, int count)
{
	Cmdbuf *cb;
	Cmdtab *ct;
	LV *l;

	l = arg;
	cb = parsecmd((char *)a, count);
	if (cb == nil)
		return 0;
	ct = lookupcmd(cb, ctltab, nelem(ctltab));
	if (ct == nil) {
		u->err = "unknown ctl command";
		free(cb);
		return 0;
	}
	wassert(nil);

	switch (ct->index) {
	case CMoffline:
		xloffline(l);
		break;
	case CMonline:
		xlonline(l);
		break;
	default:
		u->err = "not yet";
		count = 0;
	}
	wassert(cb->f[0]);

	free(cb);
	return count;
}

int
lunctl(void *arg, void *a, int count, vlong)
{
	LV *l;
	int ret;

	l = arg;
	xsyslog(LOGCOM "lun/%d %.*s\n", l->lun, endspace(a, count), a);
	ret = lunctlx(arg, a, count);
	if (u->err)
		xsyslog(LOGCOM "lun/%d error %s\n", l->lun, u->err);
	return ret;
}

int
confread(void *arg, void *a, int count, vlong offset)
{
	LV *l;
	
	l = arg;
	rlock(l);
	if (offset > l->nqc) {
		runlock(l);
		return 0;
	}
	if (offset + count > l->nqc)
		count = l->nqc - offset;
	memmove(a, l->qc+offset, count);
	runlock(l);
	return count;
}

int
confwrite(void *arg, void *a, int count, vlong offset)
{
	LV *l;
	uchar qc[sizeof l->qc];
	int nqc;
	
	l = arg;
	wlock(l);
	nqc = l->nqc;
	memmove(qc, l->qc, nqc);
	if (waserror()) {
		wunlock(l);
		return 0;
	}
	if (offset > sizeof l->qc)
		error("config string size is limited to %d", sizeof l->qc);
	if (offset + count > sizeof l->qc)
		count = sizeof l->qc - offset;
	memmove(l->qc, a, count);
	l->nqc = offset + count;
	if (savemeta(l, 0) < 0) {
		l->nqc = nqc;
		memmove(l->qc, qc, nqc);
		error("failure writing LV metadata");
	}
	l->cfgino->length = l->nqc;
	poperror();
	wunlock(l);
	return count;
}

/*
 * online|offline
 * name of logical volume
 */
 
int
lunstatus(void *arg, void *a, int count, vlong offset)
{
	LV *l;
	char buf[8192];

	if (offset)
		return 0;
	l = arg;
	rlock(l);
	snprint(buf, sizeof buf, "%s %s %T\n",
		l->mode & LVONL ? "online" : "offline",
		l->name, lun2targ(l->lun));
	runlock(l);
	return readstr(offset, a, count, buf);
}

int
lunread(void *arg, void *a, int count, vlong offset)
{
	LV *l;
	char buf[Xblk];
	int ocount, n;
	vlong o;
	uchar *p;
	
	l = arg;
	p = a;
	ocount = count;

	/* move first part of a blk */
	if (offset % Xblk) {
		o = offset & Xblk-1;
		n = lvio(l, buf, Xblk, o, OREAD);
		if (n != Xblk) {
			return 0;
		}
		n = Xblk - offset % Xblk;
		if (n > count)
			n = count;
		memmove(a, buf + offset % Xblk, n);
		p += n;
		count -= n;
		offset += n;
	}
	
/**/	assert(count == 0 || offset % Xblk == 0);
	/* move any complete blocks */
	while (count >= Xblk) {
		n = lvio(l, p, Xblk, offset, OREAD);
		if (n != Xblk) {
			return 0;
		}
		p += n;
		count -= n;
		offset += n;
	}
	
	/* if needed read last part of a block */
	if (count > 0) {
		n = lvio(l, buf, Xblk, offset, OREAD);
		if (n != Xblk) {
			return 0;
		}
		memmove(p, buf, count);
	}
	return ocount;
}

int
lunwrite(void *arg, void *a, int count, vlong offset)
{
	char buf[Xblk];
	int ocount, n, len;
	vlong o;
	uchar *p;
	LV *l;
	
	p = a;
	ocount = count;
	l = arg;

	/* move first part of a blk */
	if (offset % Xblk) {
	
		/* get existing block in buffer and modify it */
		o = offset & Xblk-1;
		n = lvio(l, buf, Xblk, o, OREAD);
		if (n != Xblk) {
			return 0;
		}
		len = Xblk - offset % Xblk;
		if (len > count)
			len = count;
		memmove(buf + offset % Xblk, a, len);
		n = lvio(l, buf, Xblk, o, OWRITE);
		if (n != Xblk) {
			return 0;
		}
		p += len;
		count -= len;
		offset += len;
	}
	
/**/	assert(count == 0 || offset % Xblk == 0);
	/* move any complete blocks */
	while (count >= Xblk) {
		n = lvio(l, p, Xblk, offset, OWRITE);
		if (n != Xblk) {
			return 0;
		}
		p += n;
		count -= n;
		offset += n;
	}
/**/	assert(count == 0 || offset % Xblk == 0);
	/* read last part of a block */
	if (count > 0) {
		n = lvio(l, buf, Xblk, offset, OREAD);
		if (n != Xblk) {
			return 0;
		}
		memmove(buf, p, count);
		n = lvio(l, buf, Xblk, offset, OWRITE);
		if (n != Xblk) {
			return 0;
		}
	}
	return ocount;
}

#ifdef notdef
int
lunread(void *arg, void *a, int count, vlong offset)
{
	return lvio((LV *)arg, a, count, offset, OREAD);
}

int
lunwrite(void *arg, void *a, int count, vlong offset)
{
	return lvio((LV *)arg, a, count, offset, OWRITE);
}
#endif 

/*
 * translate an IO request and call pvio to do the work.
 */
 
int
lvio(LV *l, void *a, int acount, vlong off, int mode)
{
	PVIO pvv;
	uvlong tod;
	int count, todo, done, len, n;
	ulong xext;
	uchar *buf;

	todo = acount;
	if (off > l->length)
		return 0;
	if (off+todo > l->length)
		todo = l->length - off;
	done = 0;
	if (todo > Xextent) {
		u->err = "request too large";
		return 0;
	}
	buf = a;
	tod = ticks;
	if ((off % Xextent) == 0 && todo > Xextent/2) {
		n = xlate(&pvv, l, off, todo, mode);
		if (n == 0) {
			u->err = "xlate failed";
			return 0;
		}
		n = bufxtntcopy(pvv.pv, pvv.offset/Xextent, a, pvv.count, mode);
		xlatefini(&pvv);
		if (n == -1) {
			u->err = "copy failed";
			return 0;
		}
		done = pvv.count;
		todo = 0;
	}
	while (done < todo) {
		len = (todo - done > 8192) ? 8192 : todo - done;
		xext = off%Xextent + len;
		if (xext > Xextent)
			len -= xext % Xextent;
		n = xlate(&pvv, l, off, len, mode);
		if (n == 0)
			return done;
		count = pvio(pvv.pv, buf, pvv.count, pvv.offset, mode);
		xlatefini(&pvv);
		if (count != pvv.count)
			break;

		off += count;
		done += count;
		buf += count;
	}
	incfilter(mode == OREAD ? &l->rf : &l->wf, done, ticks - tod);
	return done;
}

/*
 * Do PV IO.  handles split chuncks.
 * We know that the IO is in 8K or less chunks.
 */

int
pvvio(PVIO pvv[], int n, void *ap, int mode)
{
	int count, count1;
	uchar *a;
	
	a = ap;
	count = pvio(pvv[0].pv, a, pvv[0].count, pvv[0].offset, mode);
	if (count < 0)
		return 0;
	a += count;
	if (n == 1)
		return count;
	count1 = pvio(pvv[1].pv, a, pvv[1].count, pvv[1].offset, mode);
	if (count1 < 0)
		return count;
	return count + count1;
}

void
dirtyxtnt(PV *pv, vlong offset, int wlocked)
{
	int xtnt;

	xtnt = offset/Xextent;

	if ((pv->ref[xtnt] & REFused) && !(pv->ref[xtnt] & REFdirty)) {
		if (!wlocked)
			wlock(pv);

		pv->ref[xtnt] |= REFdirty|REFnf;

		if (!wlocked)
			wunlock(pv);
	}
}

/*
 * pvio does the mirroring.
 * We have a parallel routine checking on the failure of
 * one of the mirror members.
 * pv locked flag is set if called with pv locked.
 */

long
pviox(PV *pv, void *a, long count, vlong offset, int mode, int locked)
{
	int t, n, m, state, targ, mirror, lost;
	enum { Xldb=0, };
	XLock *x;
	long cnt;
	uvlong tod;
	vlong length;

	cnt = 0;

	if (!pv) {
		xsyslog("Warning: attempting to read from nil PV\n");
		return -1;
	}

	if (!locked)
		rlock(pv);

		state = pv->state;
		targ = pv->targ;
		mirror = pv->mirror;
		length = pv->length;
		lost = pv->flags & PVFlost;

	if (!locked)
		runlock(pv);

	if (lost) {
		xsyslog("%s at offset %lld attempted on lost PV %T\n", 
			mode == OREAD ? "Read" :"Write", offset, pv->targ);
		return 0;
	}
	if (offset >= length) {
		if (Xldb)
			print("pvio: offset=%lld length=%lld\n",
			      offset, length);
		return 0;
	}
	tod = ticks;
	if (offset + count > length)
		count = length - offset;
	if (Xldb)
		print("pvio: count=%ld mode=%d state=%d\n",
		      count, mode, state);

	switch (mode) {
	case OREAD:
		switch (state) {
		case PVSdouble:
			t = xaddu(&pv->toggle, 1);
			t = t & 1 ? targ : mirror;
			cnt = edio(t, a, count, offset, mode);
			if (cnt == count)
				break;
			cnt = edio(t == mirror ? targ : mirror, a, count, offset, mode);
			if (cnt != count)
				break;
			failmir(pv, t, locked);
			break;
		case PVSsingle:
		case PVSbroke:
		case PVSresilver:
		case PVSoosync:
			cnt = edio(targ, a, count, offset, mode);
			break;
		case PVSvirtual:
			memset(a, 0, count);
			cnt = count;
		}
		break;
	case OWRITE:
		x = xrlock(pv, offset/Xextent); // exclude resilver copy
		switch (state) {
		case PVSbroke:
		case PVSoosync:
			dirtyxtnt(pv, offset, locked);
			// fall through
		case PVSsingle:
			cnt = edio(targ, a, count, offset, mode);
			xrunlock(x);
			break;
		case PVSresilver:
		case PVSdouble:
			n = edio(targ, a, count, offset, mode);
			m = edio(mirror, a, count, offset, mode);
			xrunlock(x);
			if (n != count && m != count) {
				/* crap, both bad.  Don't spew errors at the
				 * user if things go unplugged.
				 */
				break;
			}
			cnt = count;
			if (n != count) {
				dirtyxtnt(pv, offset, locked);
				failmir(pv, targ, locked);
			}
			if (m != count) {
				dirtyxtnt(pv, offset, locked);
				failmir(pv, mirror, locked);
			}
			break;
		case PVSvirtual:
			xrunlock(x);
			cnt = -1;
			xsyslog("Internal error: attempt to write virtual PV\n");
		}
		break;
	}
	incfilter(mode == OREAD ? &pv->rf : &pv->wf, cnt, ticks - tod);
	return cnt;
}

long
pvio(PV *pv, void *a, long count, vlong offset, int mode)
{
	long ret;

	ret = pviox(pv, a, count, offset, mode, 0);
	return ret;
}


/*
 * xlread and xlwrite handle the case where counts
 * and offsets are not a multiple of a sector.  I highly
 * suspect this should simply be designed out, or we need
 * to consider sector locking to ensure a read, modify, write
 * doesn't collide with someone else's in the same sector.
 * It's unlikely as nonsector offset/count should only happen
 * at the end
 * of writing metadata where we don't have a full sector
 * of ref counts, but it's still an open door.
 */

enum { Xsec= 512, };

long
xlread(PV *pv, void *a, long count, vlong offset)
{
	char buf[Xsec];
	int ocount, n, o, m;
	uchar *p;
	
	p = a;
	ocount = count;
	
	/* sector align us */
	o = offset % Xsec;
	if (o) {
/**/		print("xlread: had to read head of transfer\n");
		/* round down to previous sector */
		if (pvio(pv, buf, Xsec, offset - o, OREAD) != Xsec)
			goto e;
		n = Xsec - o;
		if (n > count)
			n = count;
		memmove(p, buf + o, n);
		p += n;
		count -= n;
		offset += n;
	}
/**/	assert(count == 0 || offset % Xsec == 0);

	/* do chunk i/o */
	while (count >= Xsec) {
		if (count >= Xblk)
			n = Xblk;
		else
			n = count & ~(Xsec-1);
		m = pvio(pv, p, n, offset, OREAD);
		if (m != n) {
			xsyslog("xlread: pvio failure: %T:%lld:%d (%d)\n", pv->targ, offset, n, m);
			goto e;
		}
		p += n;
		count -= n;
		offset += n;
	}
	
	/* if needed read last part of a sector */
	if (count > 0) {
/**/		print("xlread: had to read tail of transfer\n");
		if (pvio(pv, buf, Xsec, offset, OREAD) != Xsec)
			goto e;
		memmove(p, buf, count);
	}
e:	return ocount - count;
}

long
xlwrite(PV *pv, void *a, long count, vlong offset)
{
	char buf[Xsec];
	int ocount, n, o;
	uchar *p;
	
	p = a;
	ocount = count;
	
	/* move first part of a blk */
	o = offset % Xsec;
	if (o) {
/**/		print("xlwrite: had to read head of transfer\n");
		/* get existing block in buffer and modify it */
		if (pvio(pv, buf, Xsec, offset - o, OREAD) != Xsec)
			goto e;
		n = Xsec - o;
		if (n > count)
			n = count;
		memmove(buf + o, a, n);
		if (pvio(pv, buf, Xsec, offset - o, OWRITE) != Xsec)
			goto e;
		p += n;
		count -= n;
		offset += n;
	}
/**/	assert(count == 0 || offset % Xsec == 0);

	/* do chunk i/o */
	while (count >= Xsec) {
		if (count >= Xblk)
			n = Xblk;
		else
			n = count & ~(Xsec-1);
		if (pvio(pv, p, n, offset, OWRITE) != n)
			goto e;
		p += n;
		count -= n;
		offset += n;
	}
/**/	assert(count == 0 || offset % Xsec == 0);

	/* read last part of a block */
	if (count > 0) {
/**/		print("xlwrite: had to read tail of transfer\n");
		if (pvio(pv, buf, Xsec, offset, OREAD) != Xsec)
			goto e;
		memmove(buf, p, count);
		if (pvio(pv, buf, Xsec, offset, OWRITE) != Xsec)
			goto e;
	}
e:	return ocount - count;
}

/* Subtle difference in types push me to have two sets. */
int
pvread(void *arg, void *a, int count, vlong offset)
{
	return xlread((PV *)arg, a, count, offset);
}

int
pvwrite(void *arg, void *a, int count, vlong offset)
{
	return xlwrite(arg, a, count, offset);
}

void
incref(Ref *r)
{
	_xinc(&r->ref);
}

long
decref(Ref *r)
{
	return _xdec(&r->ref);
}

void
failmir(PV *pv, int targ, int locked)
{
	char sn[Nserial];

	if (pv->state != PVSdouble)
		return;
	if (!locked) 
		wlock(pv);

	pv->sysalrt = 1;
	pv->state = PVSbroke;
	if (pv->targ == targ) {
		pv->targ = pv->mirror;
		pv->mirror = targ;
		memcpy(sn, pv->sn[0], Nserial);
		memcpy(pv->sn[0], pv->sn[1], Nserial);
		memcpy(pv->sn[1], sn, Nserial);
	}
	xsyslog("PV mirror element %T failed, %T is primary\n",
		pv->mirror, pv->targ);

	if (updpv(pv) < 0)
		xsyslog(LOGCOM "failmir updpv %T failure ignored\n", pv->targ);

	if (!locked)
		wunlock(pv);
	schedsave();
}

struct {
	int act;
	Rendez;
	QLock;
} save;

static void
saveproc(void)
{

	save.l = &save;
	for (;;) {
		qlock(&save);
		if (save.act == 0)
			rsleep(&save);
		save.act = 0;
		qunlock(&save);
		writeconfig();
	}
}

void
schedsave(void)
{
	qlock(&save);
	save.act = 1;
	rwakeup(&save);
	qunlock(&save);
}

static void
missingpv(PV *pv) // called with PV wlocked
{
	if (pv->flags & PVFmissp) {
		xsyslog("PV target %T is missing\n", pv->mtarg);
		pv->flags |= PVFmissplgd;
	}
	if (pv->flags & PVFmissm) {
		xsyslog("PV mirror target %T is missing\n", pv->mirror);
		pv->flags |= PVFmissmlgd;
	}
}

static void
brokenpv(PV *pv) // called with PV wlocked
{
	PV opv;

	if (pv->flags & PVFlost)
		return;

	xsyslog("%T->%T mirror is in a broken state\n", pv->targ, pv->mirror);

	if ((pv->flags & PVFuserbroke) || pv->sysalrt < 3 ||
		targlen(pv->mirror) == 0 || mendmirror(pv) != 1)
		return;

	opv = *pv;
	pv->state = PVSoosync;
	pv->sysalrt = 0;
	if (updpv(pv) < 0) {
		xsyslog(LOGCOM "%T->%T remirror updpv failed\n",
			pv->targ, pv->mirror);
		pv->state = opv.state;
		pv->sysalrt = opv.sysalrt;
		return;
	}
	xsyslog("PV %T: remirrored to target %T %s silver\n",
		pv->targ, pv->mirror,
		(pv->flags & PVFfullsilver) ? "full" : "partial");
	mirrorck = 1;
}

/*
	alert: once a min for the first three minutes.
	then once every 15 minutes for the first hour.
	then once an hour. */

int
needalrt(uvlong a) // a is in minutes
{
	return ((a < 3) || (a < 60 && !(a % 15)) || !(a % 60));
}

int
needalrtsec(uvlong a) // a is in seconds
{
	return ((a < 180 && !(a % 60)) || ((a < 3600 && !(a % 900))) || !(a % 3600));
}

static uvlong busyold, busyminutes;

static void
busyalerts(void)
{
	if (busyold == servingbusy) {
		if (busyminutes) {
			xsyslog(LOGCOM
				"servingbusy cleared after %ulld minutes\n",
				busyminutes);
			busyminutes = 0;
		}
		return;
	}
	busyold = servingbusy;

	if (needalrt(++busyminutes))
		xsyslog(LOGCOM "servingbusy now %ulld\n", busyold);
}

static int totalpvs;

static void
pvsyslog(void)
{
	PVPool *pvp;
	PV *pv;
	int i, pvs;

	for (;; sleep(1000 * 60)) {
		if (shutdown)
			xlexits(0);
		busyalerts();
		pvs = 0;
		rlock(&lk);
		for (pvp = pools; pvp; pvp = pvp->next) {
			for (i = 0; i < Npvsperpool; i++) {
				pv = pvp->pv[i];
				if (pv)
					pvs++;
				if (pv == nil)
					continue;
				switch (pv->state) {
				case PVSmissing:
					wlock(pv);
					if (needalrt(pv->sysalrt++))
						missingpv(pv);
					wunlock(pv);
					break;
				case PVSbroke:
					wlock(pv);
					if (needalrt(pv->sysalrt++))
						brokenpv(pv);
					wunlock(pv);
					break;
				}
			}
		}
		runlock(&lk);
		totalpvs = pvs;
	}
}

static ulong
modnum(ulong num, int per)
{
	return num - num % per;
}

/* VSX-4587: notification and clearing syslogs when	*/
/* pool's % crosses certain thresholds:			*/
/* 50 - 80 : Every 5%					*/
/* 80 - 100: Every 1%					*/
/*    > 95 : Every 30 minutes				*/
static int
xthresh(ulong old, ulong new)
{
	int lc, hc, per;

	if (old == new)
		return 0;
	if (old < 50 && new < 50)
		return 0;
	per = new < 80 ? 5 : 1;
	/* don't care about direction; only if it crosses */
	if (old < new) {
		lc = modnum(old, per) + per;
		hc = modnum(new, per);
	} else {
		lc = modnum(new, per) + per;
		hc = modnum(old, per);
	}
	if (hc >= lc)
		return 1;
	return 0;
}

static void
poolpctsyslog(void)
{
	PVPool *pvp;
	ulong etotal, efree, new;
	uint w;
	
	w = 0;
	for (;; sleep(1000 * 10)) {
		if (shutdown)
			xlexits(0);
		w++;
		rlock(&lk);
		for (pvp = pools; pvp; pvp = pvp->next) {
			etotal = pvp->etotal;
			efree = pvp->efree;
			new = etotal ? (uvlong)(etotal - efree) * 100 / etotal : 0;
			if (xthresh(pvp->cpct, new) || (new >= 95 && w % 180 == 0))
				xsyslog("Pool %s: %uld percent full\n", pvp->name, new);
			pvp->cpct = new;
		}
		runlock(&lk);
	}
}

static void
cmppvlog(PV *pv, char *msg)
{
	xsyslog("PV %T: metadata status %s %s %ulx %ud %T %lld %uld %s\n",
		pv->targ, msg, fmtpvstate(pv->state),
		pv->flags & PVFmeta,
		pv->npve, pv->mirror, pv->length, pv->ctime, pv->label);
}

static void
cmppv(PV *pv, uchar *buf)	// called with PV locked
{
	uchar *e;
	ushort s;
	PV *cpv;

	if (memcmp(buf, VSPVMAGIC, 4) != 0) {
		xsyslog("PV %T: metadata status missing header magic\n",
			pv->targ);
		return;
	}
	s = GBIT16(buf+4);
	buf[4] = buf[5] = 0;
	if (onesum(buf, Xblk) != s) {
		xsyslog("PV %T: metadata status checksum error\n", pv->targ);
		return;
	}
	cpv = mallocz(sizeof *cpv, 1);
	if (cpv == nil)
		return;

	e = &buf[Xblk];
	convM2PV(buf, e, cpv);
	cmppvlog(pv, "mem ");
	cmppvlog(cpv, "disk");
	freepv(cpv);
}

static void
haltxlate(void)
{
	xsyslog("xlate has been halted\n");
	wlock(&lk);
}

static void
logpvmeta(PV *pv, uchar *buf, uchar *meta)
{
	char *file;
	int fd;

	file = smprint("/app/scp/%T:%lld", pv->targ, pv->offset);
	fd = create(file, OWRITE, 0666);
	free(file);
	write(fd, meta, Xblk);
	write(fd, buf, Xblk);
	close(fd);

	xsyslog("PV %T: metadata invalid\n", pv->targ);
	cmppv(pv, buf);
	if (access("/n/kfs/conf/strict", AEXIST) >= 0) {
		haltxlate();
	}
}

static void
ckpvmeta(PV *pv) // called with pv rlocked and returns unlocked
{
	PVPool *pvp;
	int targ, uid;
	uchar buf[Xblk], meta[Xblk];

	targ = pv->targ;
	uid = pv->uid;
	mkpvmeta(pv, meta);
	runlock(pv);

	if (pviox(pv->meta, buf, Xblk, pv->offset, OREAD, 0) != Xblk)
	{
		xsyslog("PV %T: metadata read error\n", pv->targ);
		return;
	}
	rlock(&lk);
	for (pvp = pools; pvp; pvp = pvp->next) {
		rlock(pvp);
		pv = lookuppv(pvp, targ);
		if (pv) {
			rlock(pv);
			if (uid == pv->uid && memcmp(buf, meta, Xblk) != 0)
				logpvmeta(pv, buf, meta);
			runlock(pv);
			runlock(pvp);
			runlock(&lk);
			return;
		}
		runlock(pvp);
	}
	runlock(&lk);
}

static void
dosnsaveconfig(void)
{
	if (!snsaveconfig)
		return;

	while (xchg(&snsaveconfig, 0)) {
		writeconfig();
		xsyslog(LOGCOM "PV serial numbers saved to config.\n");
	}
}

/* Chirp once a minute, suspended LVs are serious */
static void
lvsyslog(void)
{
	LV *lv;

	sleep(60*3000); /* wait for things to get going */
	while(!shutdown) {
		dosnsaveconfig();	/* the above sleep must be > 45s */
		sleep (60*1000);
		rlock(&lk);
		for (lv = vols; lv; lv = lv->next) {
			rlock(lv);
			if (lv->flags & LVFsuspended)
				xsyslog("LV %s: state suspended\n", lv->name);
			runlock(lv);
		} 
		runlock(&lk);
	}
	xlexits(0);
}

/* Chirp once a minute, we don't like legacy anymore */
static void
legsyslog(void)
{
	PVPool *pvp;
	PV *pv;
	int i, leg;

	sleep (30 * 1000);
	while (!shutdown) {
		leg = 0;
		sleep (60 * 1000);
		rlock(&lk);
		for (pvp = pools; pvp; pvp = pvp->next) {
			rlock(pvp);
			for (i = 0; i < Npvsperpool; i++) {
				pv = pvp->pv[i];
				if (pv == nil)
					continue;
				if (pv->offset)
					leg++;
			}
			runlock(pvp);
		}
		runlock(&lk);
		if (leg)
			xsyslog("Please run /updatelegacy to optimize legacy volumes\n");
		else
			xlexits(0);
	}
}

enum { PVseconds = 30 }; // each PV is checked every PVseconds

/*
 * Every PVseconds/totalpvs find the next PV to check. Grab big locks to find
 * it; lock the PV; and then release the big locks before checking the
 * PV.
 */

static void
sanityproc(void)
{
	PVPool *pvp;
	PV *pv;
	int i, n, lastpool, lastpv, pvs;

	lastpool = lastpv = -1;

	while (!shutdown) {
		pvs = totalpvs;
		if (pvs > 0) {
			sleep(PVseconds * 1000 / pvs);
		} else {
			sleep(60*1000);
			continue;
		}
		pv = nil;
		n = 0;
		rlock(&lk);
		for (pvp = pools; pvp; pvp = pvp->next) {
			if (n > lastpool) {
				for (i = lastpv + 1; i < Npvsperpool; i++) {
					pv = pvp->pv[i];
					if (pv && !(pv->flags & PVFBZ3318)) {
						rlock(pv);
						break;
					}
				}
				if (pv) {
					lastpv = i;
					break;
				} else {
					lastpool = n;
					lastpv = -1;
				}
			}
			n++;
		}
		runlock(&lk);
		if (pv) {			// pv is still rlocked here
			switch (pv->state) {
			case PVSsingle:
			case PVSdouble:
			case PVSbroke:
				if (pv->flags & PVFlost) {
					runlock(pv);
					xsyslog("PV %T lost\n", pv->targ);
				} else
					ckpvmeta(pv);	// returns with pv unlocked
				break;
			default:
				runlock(pv);
				break;
			}
		} else {
			lastpool = -1;
		}
	}
	xlexits(0);
}

int
snaplist(LV *pl, LVL **ll, Snapsched *s)		/* called with lk locked */
{
	LVL *p, *q, *r, **pp, *lp;
	LV *l;
	int n;

	n = 0;
	if (waserror()) {
		while (q = p) {
			p = p->next;
			free(q);
		}
		*ll = nil;
		return -1;
	}
	p = nil;
	for (l = vols; l; l = l->next) {
		if ((l->mode & LVSNAP) == 0)
			continue;
		if (snapstrcmp(pl->name, l->name) != 0)
			continue;
		if (s != nil && schedcmp(s, l->sched))
			continue;
		q = mallocz(sizeof *q, 1);
		if (q == nil)
			error("allocation failure");
		q->l = l;
		pp = &p;
		lp = nil;
		for (; (r = *pp) && l->snaps > r->l->snaps; pp = &r->next, lp = r)
			;
		if (q->next = r)
			r->prev = q;
		*pp = q;
		q->prev = lp;
		n++;
	}
	poperror();
	*ll = p;
	return n;
}

void
freelvl(LVL *p)
{
	LVL *np;

	while (np = p) {
		p = np->next;
		free(np);
	}
}

/*
 * see if 'b' is a snap of 'a'
 */
int
snapstrcmp(char *a, char *b)
{
	char *p;

	p = strchr(b, '.');
	if (p == nil)
		return -1;
	if (strcspn(a, ".\0") != p-b)
		return -1;
	return strncmp(a, b, p-b);
}

int
Tfmt(Fmt *f)
{
	int t;
	
	t = va_arg(f->args, int);
	if (t == -1)
		return fmtprint(f, "unset");
	return fmtprint(f, "%d.%d", t >> 8, t & 0xff);
}

int
rdperf(void *, void *a, int count, vlong offset)
{
	char buf[8192];

	if (offset)
		return 0;

	snprint(buf, sizeof buf,
		"serving: %d\n"
		"serving high: %d\n"
		"serving busy: %ulld\n"
		"AoE queued: %d\n"
		"AoE queued high: %d\n"
		"AoE  requests: %ulld\n"
		"AoE responses: %ulld\n"
		"AoE request resends: %ulld\n"
		"AoE responses dropped: %ulld\n"
		"AoE request failures: %ulld\n"
		"AoE error responses: %uld\n"
		"ATA error responses: %uld\n"
		"CoWs: %uld\n"
		"extent lock waits: %uld\n"
		"extent engine waits: %uld\n"
		"nonresponsive MACs: %ulld\n",
		serving, servingmax, servingbusy, *nqp, *nqmaxp,
		xmits, recvs, rexmits, resmiss, xmitfails,
		aoeerrs, ataerrs, cows, xlockwaits,
		xcbwaits, rexmitlims);

	return readstr(offset, a, count, buf);
}

static void
isnotremote(char *name)
{
	LV *l;

	rlock(&lk);

	for (l = vols; l; l = l->next) {
		if (isshadow(l) && strcmp(l->rmtname, name) == 0) {
			uerr("LV %s is using %s", l->name, name);
			break;
		}
	}
	runlock(&lk);
}

/* Soli Deo Gloria */
/* Brantley Coile */
