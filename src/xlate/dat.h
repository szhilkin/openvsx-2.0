/*
 * data include for the VSX code.
 * WARNING: this system uses multiple proc that share
 * common memory segments (text, data+bss), BUT NOT THE STACKS.
 * You cannot use pointers to automatic variables that are
 * visible between processes.  If you don't understand this,
 * undo any changes, back away slowly, and go ask Sam.
 */

/*
 * A note on locking:
 * There are two global locks in xlate:
 * - lk which protects the pools array, the vols linked list,
 *	and the luns array
 * - lclock (only in xlate.c) which protects the block cache
 *	holding blocks of LVE descriptors
 * The structures LV, PV, and PVP each have their own lock.
 * Individual extents can be locked with an XLock, either
 * for reading or for writing.  The extents in the xrlock and
 * xwlock calls are identified by a pointer to the descriptive
 * structure (either an LV or a PV) and the extent number.
 *
 * To prevent deadlock, anytime locks are held simultaneously
 * they must be obtained in the following order:
 *	lk
 *	LV structure
 *	PVP structure
 *	PV structure
 *	XLock for an extent
 *	lclock
 */

enum
{
	Nluns	= 4096,
	MAXEXT	= 131072000,	/* 500T / 4M */
	OPERM	= 0x3,		/* mask of all permission types in open mode */
	Ninode	= 2048,		/* Inode allocation chunks.  Inodes are 76 bytes */
	Maxfdata= 8192,
	Maxulong= (1ULL << 32) - 1,
	Xextent = 4*1024*1024LL,	/* extent size */
	Xblk	= 8192,			/* block size */
	Xblkperext = Xextent / Xblk,	/* blocks per extent */
	Npools = 32,		/* maximum number of pools */
	
	NXLOCKS	= 251,		
	
	Ntargprocs= 128,	/* number of procs servicing target requests, systemwide */
	BCNT	= 64,		/* number of request we can take */
	MAXSEC	= 16,		/* largest request we can take */
	Nmasks	= 255,		/* number of lun masks we support per lv */
	Nname = 16,		/* max permitted pool/lv name length */
	Npvsperpool = 128,	/* Max PVs per pool */
	Nsched = 32,		/* number of snapshot schedules */
	Nretain = 256,		/* Max retain count for schedules */

	Rttscale = 16,
	Rttdscale = 8,
	Rttavginit = 250 << Rttscale,
	Rttdevinit = Rttavginit / 4,
	Rtomin = 50,
	Rtomax = 10*1000,	/* max scaled back i/o on rexmit */
	Ntiomax = 30*1000,
	Ntiomaxmaint = 180*1000,	/* based on time for SRX4200 cold boot and spin-up */
	Nlabelmax = 16,
	Nchash		= 1021,
	Nserial = 20,		/* characters in a serial number */
};

enum
{
	Sun = 0,
	Mon,
	Tue,
	Wed,
	Thu,
	Fri,
	Sat,	
};

enum
{
	Jan = 0,
	Feb,
	Mar,
	Apr,
	May,
	Jun,
	Jul,
	Aug,
	Sep,
	Oct,
	Nov,
	Dec,
};

typedef int	(*Iop)(void *, void *, int, vlong);
typedef struct 	Fid 	Fid;
typedef struct 	Inode 	Inode;
typedef	struct	PV	PV;
typedef struct	PVE	PVE;
typedef	struct	LV	LV;
typedef struct	LVE	LVE;
typedef struct	LVEBUF	LVEBUF;
typedef	struct	PVPool	PVPool;
typedef	struct	XLock	XLock;		/* used to lock extents */
typedef	struct	Cmdtab	Cmdtab;
typedef struct	Cmdbuf	Cmdbuf;
typedef struct	Aoe 	Aoe;
typedef struct	AoeDisk AoeDisk;
typedef struct	AoeQC 	AoeQC;
typedef struct	AoeSnap	AoeSnap;
typedef struct	AoeKRR	AoeKRR;
typedef struct	AoeRKRR	AoeRKRR;
typedef	struct	Msg	Msg;
typedef	struct	Sch	Sch;
typedef	struct	Inits	Inits;		/* Initiators */
typedef	struct	PVIO	PVIO;		/* IO work for PVs */
typedef	struct	Ref	Ref;		/* copied from thread.h */
typedef	struct	User	User;
typedef	struct	AoeMac	AoeMac;
typedef struct	AoeMDir	AoeMDir;
typedef struct	Tmac	Tmac;
typedef struct	Target	Target;
typedef struct	XCB	XCB;
typedef	struct	LVL	LVL;
typedef struct Iofilter Iofilter;
typedef struct Snapsched Snapsched;
typedef struct XferStat XferStat;
typedef	struct	Tag	Tag;
typedef struct	Shadow	Shadow;

enum { 
	NXCB 	= 8, 
	NWRKRS 	= 16,

	Xfree = (1<<0), 
	Xstop = (1<<1), 
	Xfail = (1<<2),
	Xexit = (1<<3),
	Xdone = (1<<4),
};

struct XCB
{
	int	flag;		/* protected by xcblk */
	QLock	qlk;
	Rendez	worker;
	Rendez	user;
	PV	*pv;		/* one must only define a pv OR a targ for i/o */
	int	targ;
	vlong	off;
	ulong	length;
	int	mode;
	int	nworking;	/* how many procs are working on it */
	char	*buf;		/* copy to/from buf */
	char	*xbuf;		/* allocated Xextent buffer */
};

struct Ref 
{
	long	ref;
};

struct Fid
{
	short	busy;
	short	open;
	short	rclose;
	int	fid;
	Fid	*next;
	char	*user;
	Inode	*inode;
	Inode	*dirread;
	void	*arg;
};

struct Inode
{
	Inode	*next;
	Inode	*cnext;
	Inode	*parent;
	Inode	*children;
	Inode	**chash;
	int	dead;
	Qid	qid;
	ushort	open;
	ulong	perm;
	char	*name;
	ulong	atime;
	ulong	mtime;
	char	*user;
	char	*group;
	char	*muid;
	void	*arg;
	int	(*read)(void *, void *, int, vlong);
	int	(*write)(void *, void *, int, vlong);
	vlong	length;
	Ref;
};

enum
{
	Pexec =		1,
	Pwrite = 	2,
	Pread = 	4,
	Pother = 	1,
	Pgroup = 	8,
	Powner =	64,
};

/*
 * LVEs are used to hold the logical block information
 * for the vector of extents in a logical volume.
 * It is also used to return an newly allocated pve.
 * In that case, flags == ~0 when there are no extents
 * to be had.
 */
 
#pragma pack on

struct	LVE			/* logical volume extent entry */
{
	uchar	flag;		/* state of this extent */
	ushort	pid;		/* physical ID for this extent */
	uint	off;		/* offset in 4 MB units into PV */
};

#pragma pack off

/*
 * LV metadata have their own extents.  The first block of the first
 * extent has the machine independent format of the lv structure.  If
 * more than one extent is needed for the metadata the subsequent extents
 * have nothing in the first block.
 */

enum {
	Xlveperblk = Xblk/sizeof(LVE),	/* lve's per block */
	Xrefperblk = Xblk/sizeof (ushort),
	
	LFthin	= (1<<0),		/* extent not allocated yet */
	LFdirty = (1<<1),		/* this extent has been modified */
	LFnf	= (1<<2),		/* lve needs flushing */
	
	/* volume modes bits */
	
	LVREAD		=       0400,
	LVWRITE		=       0200,
	LVSNAP		= 0x80000000,
	LVLUN		= 0x40000000,
	LVONL		= 0x20000000,
	LVSEND		= 0x10000000,	/* shadowsend */
	LVCANPRUNE	= 0x04000000,	/* can autoremove snapshot */
	LVVLAN		= 0x02000000,	/* use vlan */
	LVLEG		= 0x01000000,	/* legacy; toss last ref on rmlv */
	LVDELPEND	= 0x00800000,	/* Delete Pending */
	LVWIP		= 0x00400000,	/* LV is a work in progress */
	LVTHIN		= 0x00200000,	/* LV is thin */

	LVSAVMSK	= 0xf7ffffff,	/* push mode thru when saving */

	LVFshstop =	1<<1,		/* shadow stop */
	LVFshdebug =	1<<2,		/* shadow debug */
	LVFpartial =	1<<3,		/* LV is missing PV backing storage */
	LVFwarned =	1<<4,		/* Warned missing PVs */
	LVFresew =	1<<6,		/* set if write exclusive, clear if r/w exclusive */
	LVFsuspended =	1<<7,
	LVFallsnaps =	1<<8,		/* all snapshots are found */

	LVEBpresent = 1<<0,
	LVEBdirty = 1<<1,
	LVEBinuse = 1<<2,

	LVSmanual =	0,		/* For snaps, these define classes */
	LVShour,
	LVSday,
	LVSweek,
	LVSmonth,
	LVSyear,

	Nmacmask=	255,
	Nmacres=	255,
	Nlvebuf = 16*1024,

	AThin = 1<<0,
	AClrDirty = 1<<1,
	AFreeOrphan = 1<<2,
	ANoInc = 1<<3,
	AMkDirty = 1<<4,

	SLunset	= -1,	/* Snap limit unset */
	SLign	= 0,	/* Snap limit ignored */
};

struct	LVEBUF
{
	int	flags;
	uchar	buf[Xblk];
	LV	*lv;
	int	blkno;
	LVEBUF	*nlru, *plru;
};

enum { Niosamples= 32, Lsum=0, Lmax, Lavg, Lsz, };
enum { Io512, Io1024, Io2048, Io4096, Io8192, Iog8192, Iosz, };

struct Iofilter {
	QLock;
	Iofilter *next;
	ulong nsamples;			// total samples taken
	struct {
		ulong b;
		uvlong lat[Lsz];		// latency sum, min, max, avg for bytes in b
		uvlong iops[Iosz];		// iops 512, 1024, 2048, 4096, 8192, >8192
	} samples[Niosamples];

	uvlong bytes;
	uvlong lmax;
	uvlong lsum;
	uvlong nlat;
	uvlong io512;
	uvlong io1024;
	uvlong io2048;
	uvlong io4096;
	uvlong io8192;
	uvlong iog8192;
};

struct	Shadow
{
	char	*rmtname;	/* remote VSX-pair name */
	char	*rmtlv;		/* remote LV */
	vlong	 rmtlvsize;	/* remote LV size */
	short	 copyclass;	/* snapshot class being copied */
	ulong	 copysnap;	/* on send the last snapshot copied
				   on recv the snapshot being copied */
	int	 copysnapsend;	/* on send the snapshot being copied */
	vlong	 lastoffset;	/* offset in snapshot volume being copied */
	int	 pid;		/* pid of recv/send proc */
	int	 fd;		/* fd for recv/send proc */
	uvlong	 total;		/* total bytes to be copied for a snapshot */
	uvlong	 bytes;		/* bytes copied so far for a snapshot */
	Iofilter filter;	/* wstat filter */
	char	*b;		/* buffer for I/O */
};

struct Snapsched {
	short retain;			/* -1 means hold */
	char class;			 
	uchar mon;
	uchar mday;
	uchar wday;
	uchar hour;
	uchar min;
};

struct	LV			/* logical volume */
{
	RWLock;
	LV	*next;		/* list of LV's in system */
	Inode	 *dir;		/* our inode dir */
	Inode	*lundir;	/* dir of our exported lun */
	Inode	*cfgino;	/* so we can change sizes */
	ulong	 flags;
	ulong	 mode;		/* as in stat(2) */
	vlong	 length;	/* declared length */
	vlong	 snaplimit;	/* size limit for all scheduled snapshots */
	char	*name;		/* what we call this volume */
	char	*label;		/* comments */
	int	 lun;		/* our lun number */
	ulong	 ctime;		/* when we were created */
	PVPool	*pool;		/* which pool we are a member of */
	ulong	 snaps;		/* how many snapshots have been taken */
	ulong	 prevsnap;	/* previous snapshot in history */
	int	 vlan;		/* vlan if used */
	int	 nqc;		/* bytes in the query/config string */
	ulong	 nlve;		/* number of extents in this logical volume */
	int	 frstdat;	/* number of metadata extents, ie. first data extent */
	uchar	 qc[1024];	/* the string itself */
	int	 nmmac;		/* number of mask macs  */
	int	 nrmac;		/* number of reserve macs */
	uchar 	 mmac[Nmacmask][6];	/* mask macs */
	uchar	 rmac[Nmacres*6];	/* reserve macs */
	LVE	*lve;		/* map of logical volume extents */
	ulong	 dirty;
	ulong	 thin;
	ulong	loadtime;
	char	serial[21];  	/* serial number of lv */
	int	iopssamp;	/* num sec to sample for iops */
	int	statsamp;	/* num sec to sample iostats */
	Iofilter	rf;	/* track reads */
	Iofilter	wf;	/* track writes */
	Snapsched 	sched[Nsched];	/* Snap Schedules, LVs have 32 entries, snaps use first entry */
	ulong		nse;		/* number snap extents */
	Shadow;
	QLock	quick;		/* for quick updating of non-metadata, so do
				   not go get other locks while this is held */
	ulong	 exts[Npvsperpool];	/* number of extents allocated per PV */
};
	
struct LVL
{
	LVL	*list;		/* list of volumes */
	LVL	*next;		/* sorted list of snapshot for that volume */
	LVL	*prev;
	LV	*l;		/* pointer to the volume */
};
	
/*
 * Ref has two flags and a reference count.
 * The high bit, b15, is set if the pv has been written at all.
 * The next bit, b14, is set if the pv has been written to since
 * it lost its mirror partner.  If its mirror partner
 * returns we don't have to copy the whole mirror.
 * B13 indicates a first lvt extent.
 * B12 is used to mark ref's that need to be updated.
 * Not the data in the extent, but the ref itself.
 */

enum {
	REFused	= (1<<15),	/* extent written */
	REFdirty= (1<<14),	/* extent needs mirroring */
	REFlvt	= (1<<13),	/* meta data for lv */
	REFnf	= (1<<12),	/* needs flushing */
	
	/* PV states */
	PVSmissing = 0,		/* PV not there */
	PVSsingle,		/* no mirror; just single target */
	PVSdouble,		/* active mirror */
	PVSbroke,		/* mirror fractured */
	PVSoosync,		/* mirror out of sync */
	PVSresilver,		/* catching reflection back up */
	PVSvirtual,		/* not on the disk */
	
	PVFpartial = (1<<0),	/* partial extent on end */
	PVFsilvering = (1<<1),	/* mirror being silvered */
	PVFabort = (1<<2),	/* abort silver */
	PVFBZ3318 = (1<<3),	/* BZ3318 workaround */
	PVFlost = (1<<4),	/* PV was loaded, but disappeared */
	PVFfullsilver = (1<<5),	/* if silvering, copy all REFused "Hi-yo..." */
	PVFuserbroke = (1<<6),	/* brkmirror command, so don't remirror */
	PVFfull = (1<<7),	/* PV has no free extents */
	PVFmissp = (1<<8),	/* missing primary target */
	PVFmissm = (1<<9),	/* missing mirror target */
	PVFmissplgd = (1<<10),	/* missing primary target logged */
	PVFmissmlgd = (1<<11),	/* missing mirror target logged */
				/* flags saved to metadata: */
	PVFmeta = PVFpartial|PVFfullsilver
};

struct	PVE			/* used to talk about a pv extent */
{
	int	pid;
	uint	off;			/* in extents */
};

struct	PV			/* physical volume description */
{
	RWLock;
	int	state;
	Inode	*dir;		/* our inode number */
	ulong	flags;
	int	id;
	int	uid;		/* version of pv metadata state in memory */
	char	*label;
	int	targ;		/*  target descriptor */
	int	mirror;		/*  target descriptor for the mirror */
	int	mtarg;		/* target with meta data, != targ on legacy */
	ulong	toggle;		/* lsb used to pick mirror element */
	ulong	ctime;		/* time pv was created */
	vlong	length;		/* length of physical volume */
	vlong	offset;		/* location of metadata on pv in bytes */
	uint	npve;		/* how many pv entries on this volume */
	PV	*meta;		/* pointer to metadata pv, usually self */
	int	mirwait;	/* timeout for mirror targs to come online before loading */
	int	frstfree;	/* free search speedup */
	PVPool *pool;
	ushort	*ref;		/* # of references to this extent at mtarg:offset */
	int	statsamp;		/* num sec to sample iostats */
	int	iopssamp;		/* num sec to sample for iops */
	Iofilter	rf;		/* Tracks reads */
	Iofilter	wf;		/* Tracks writes */
	uvlong	sysalrt;	/* track when to send syslog msgs */
	PVE	lm;		/* Legacy metadata new home */
	char	sn[2][Nserial];	/* targ and mirror serial numbers in ATA fmt */
	ulong	ldlverrs;	/* load LV errors */
};

struct	PVIO			/* IO control block for PV IO */
{
	PV	*pv;		/* which physical volume the io is for */
	long	count;		/* number of bytes to transfer */
	vlong	offset;		/* offset into volume */
	XLock	*x;		/* extent being used */
	ulong	flags;
};

enum {
	PVIOxwlock	= (1<<0),	/* x has been wlocked */
	PVIOflush	= (1<<1),	/* check pve flush after write */

	PVPFstriped	= (1<<0),	/* pool is striped */
};

struct	PVPool			/* physical volume pool */
{
	RWLock;
	int	flags;
	PVPool	*next;		/* list of pools */
	char	*name;		/* handle for this pool */
	char	*label;		/* descriptive label for pool */
	Inode	*dir;		/* my directory in the name space */
	Inode	*subdir;	/* where the pv's hang around */
	ulong	 etotal;	/* total extents */
	ulong	 efree;		/* free extents in pool */
	ulong	 npv;		/* number of pv's in this pool */
	PV	*pv[Npvsperpool];
	ulong	remapwarn;	/* keep remap spewage down */
	ulong	cpct;		/* consumption percent */
	int	rrobin;		/* index of last PV used for round-robin */
	ulong	flverrs;	/* freelve errors */
};

struct	XLock			/* used to lock extents */
{
	RWLock;
	void	*a;		/* also used as key to busy table */
	int k;		/* (a,k) form the key */
	int	ref;		/* number of users of this lock */
};


enum {	
	Ntsent	= 16,

	NERR	= 64,
	TAGDISC = 0x80000000,	/* Use a bit in tag space for discovery.
				 * Alternate solution: tagalloc() one
				 * for discovery.
				 */
};

/* 
 * Each worker process owns a slot in the tag array.
 * They fill out the tag and rsleep on it.
 * Tendtags() or the response to the request does the rwakeup.
 * This means that the entry doesn't have to be locked most of the time.
 * But, we do use the taglk to allocate a slot, and to scan.  We also
 * have to lock for the rsleep and rwakeup.
 */
 
struct Tag
{
	Rendez;
	Msg *cmd;
	Msg *rsp;
	Target *t;
	int	targ;
	uchar	used;		/* tag is allocated to process */
	uchar	nsent;		/* index of last sent message; also rto backoff factor */
	ulong	rto;		/* original rto for message */
	ulong	crto;		/* current rto, rto<<nsent */
	ulong	starttick;	/* tick i/o was started on */
	ulong	maxwait;	/* max time to wait for i/o */
	struct {
		ulong tag;
		ulong tick;	/* tick message was sent for rttavg */
		uchar tmi;	/* t->tmac[tmi] used */
		uchar tmacver;	/* t->tmacver when used */
		uchar port;	/* port used */
	} sent[Ntsent];
};

/*
 * This is very cool.  Since we use rfork() to create new processes
 * and don't use the thread library we growable stacks.  (The thread
 * library creates fixed size stacks in the data segment so you can
 * share pointers to locals.)  Any local variable is on the private
 * stack of the process.  We have a single global 'u' that points
 * to a User structure on in main().  This means that every
 * proc has it's own user structure.  No other proc can see it.
 */
 
struct	User			/* per process user structure */
{
	char	*err;
	Tag	*tag;
	char	*name;
	int	pid;
	int	debug;
	jmp_buf	errlab[NERR];
	int	nerrlab;
	char	errstr[ERRMAX];
};

/*
 * qc string on aoe target has the following information
 *	com.coraid.vsx
 *	shelf		- first shelf address of 16 vsx addresses
 *	uuid		- this pv's uuid
 *	got to find pvt for this volume; may be on different target.
 *	should have pool list duplicated on each pv
 */
 
/*
 * Note that one extent can hold enough lv metedata for a 2.5TB volume,
 * so we'll need multiple extents for an lvt.
 * The pve's are single bytes and can represent 17.6 PB.  Only one of them
 * will ever be needed.
 */

struct Aoe
{
	uchar 	d[6];
	uchar 	s[6];
	uchar 	t[2];
	uchar 	vf;
	uchar 	err;
	uchar 	shelf[2];
	uchar 	slot;
	uchar 	proto;
	uchar 	tag[4];
};

enum Atabits { /* used in AoeDisk cmd */
	/* err bits */
	WP= 1<<6, IDNF= 1<<4, ABRT= 1<<2,
	/* status bits */
	BSY= 1<<7, DRDY= 1<<6, DF= 1<<5, ERR= 1<<0,
};

struct AoeDisk
{
	Aoe;
	uchar 	aflags;
	uchar 	fea;
	uchar 	sectors;
	uchar 	cmd;
	uchar 	lba[8];
	uchar 	data[1];
};

struct AoeQC
{
	Aoe;
	uchar 	bcnt[2];
	uchar 	fwver[2];
	uchar 	maxsec;
	uchar 	ccmd;		/* and version number I speak */
	uchar 	len[2];
	uchar 	conf[1];
};

	
struct 	AoeSnap
{
	Aoe;
	uchar	ctime[4];	/* time stamp for snap */
};

struct	AoeMDir		/* mac list directive */
{
	int	dcmd;
	uchar	ea[6];
};

struct	AoeMac		/* NOT the wire format; see convM2ML() */
{
	int	mcmd;
	int	merror;
	int	dircnt;
	AoeMDir	dir[1];
};

struct	AoeRes
{
	Aoe;
	uchar	rcmd;
	uchar	nmacs;
	uchar	ea[1];
};

struct	AoeKRR
{
	Aoe;
	uchar	rcmd;
	uchar	rtype;
	uchar	reserved[2];
	uchar	key[8];
	uchar	other[1];
};

struct	AoeRKRR
{
	Aoe;
	uchar	rcmd;
	uchar	rtype;
	uchar	nkeys[2];
	uchar	reserved[4];
	uchar	genctr[4];
	uchar	key[8];
	uchar	other[1];
};

enum {
	ARESP 	= 8,
	AERR 	= 4,

	Awrite 	= 1,
	Aasync 	= 2,
	Aext 	= 64,

	ACATA	= 0,
	ACQC	= 1,
	ACMASK	= 2,
	ACRES	= 3,
	ACKRR	= 4,
	ACmax 	= 5,
	AVCMASK	= 0xf0,
	AVCREDIR= 0xf1,
	AVCRES	= 0xf2,
	AVSNAP	= 0xf3,
	AVmax	= 0xff,

	AQCREAD = 0,
	AQCTEST,
	AQCPREFIX,
	AQCSET,
	AQCFORCE,
	AQCTAR,

	AECMD = 1,
	AEARG,
	AEDEV,
	AECFG,
	AEVER,
	AERES,

	DTns	= 0,		/* name space */
	DTaoe,			/* ATA over Ethernet target */

	RCStat	= 0,		/* Keyed Reserve/Release commands */
	RCRegister,
	RCSet,
	RCReplace,
	RCReset,
};

struct Cmdbuf
{
	char	*buf;
	char	**f;
	int	nf;
};

struct Cmdtab
{
	int	index;	/* used by client to switch on result */
	char	*cmd;	/* command name */
	int	narg;	/* expected #args; 0 ==> variadic */
};

enum {
	Nmsgdata	= 9000,
	Nmsghdr		= 32,
	Nmsgsz		= Nmsgdata - Nmsghdr,
};

struct	Sch
{
	Sch	*next;
	LV	*lv;
	long	time;
	Snapsched	s;
	char	retries;
};

struct	Msg
{
	Msg	*next;
	int	port;
	int	count;
	uchar	*data;
	uchar	xdata[Nmsgdata];
};

struct 	Inits		/* Aoe initiators */
{
	uchar	ea[6];
	uvlong	time;
};

struct Tmac
{
	uchar	ea[6];
	uchar	lastport;	/* last port used in rr */
	uchar	retries;	/* retransmits after using this mac */
	ulong	sanmask;	/* which ports see mac */
	ulong	recent;		/* which ports have recently seen mac */
	ulong	retried;	/* which ports caused retries */
};

enum {
	Ntmac	= 16,
	TARGckd	= 1<<0,		/* PV target check done */
	TARGpv	= 1<<1,		/* this is a PV target */
	TMACtry	= 32,
};

struct	Target
{
	QLock;
	Target	*next;
	ushort	ref;
	uchar	freeme;
	uchar	nem;
	int	target;		/* shelf << 8 | slot */
	uvlong	length;
	Inode	*dir;		/* directory for this target in ns */
	char	*name;
	uchar	ntmac;
	uchar	lasttmac;
	ushort	nqc;
	uchar	tmacver;	/* track tmac[] shrinkage */
	uchar	flags;
	ushort	pad1;		/* not used */
	ulong	srtt;
	ulong	sdev;
	ulong	taggen;
	Tmac	tmac[Ntmac];
	uchar	qc[1024];
	uchar	ident[512];
	struct	{
		long	stime;	     // first/last time serial number was seen
		char	sn[Nserial]; // sn is in ATA fmt for faster compares
	} serial[2];
};

struct XferStat {
	QLock;
	uvlong total;
	int src;		//shelf.lun
	int dest;		//shelf.lun
	int wid;		//working id ; pid
	uvlong bytes;
	Iofilter filter;
};

/* iostat format */
#pragma	varargck	type	"I"	Iofilter*

/* iops format */
#pragma	varargck	type	"O"	Iofilter*

enum	{
	Ninits = 1000,

	Nmyea = 32,		/* one for each port we're using */
	Nsanports = Nmyea,

	READSTR = 4000,
};

#define	SH(x)	((x) >> 8)
#define	SL(x)	((x) & 0xff)
#define	REFCNT(r)	((r) & 0xfff)
/* Note that EXT2GB returns a float */
#define EXT2GB(x)	((x) * Xextent / 1000000000.0)

/* Remove sector rounding error from lun length */
#define	SECTORLEN(len)	((len) & ~0x1ff)

LV	*vols;			/* all the volumes */
PVPool	*pools;			/* physical volume pools */
XLock	busy[NXLOCKS];		/* locked extents */
QLock	xqlk;
Rendez	xlockwait;
ulong	xlockwaits;
int	*nqp;
int	*nqmaxp;
int	serving;
int	servingmax;
uvlong	servingbusy;
Inode	*pooldir;
Inode	*lvdir;
Inode	*lundir;
Inode	*targdir;	/* control and status for targets */
RWLock	lk;		/* lock the xlate structures */
LV*	luns[Nluns];	/* our current luns */
Msg	*freemsg;	/* list of free mesg buffers */
QLock	msglock;	/* lock for messages */
Sch	*freesch;	/* list of free snapshot sch requests */
QLock	schlock;	/* lock for snapshot sch requests */
int	shelf;		/* the first of my 16 shelf addresses */
char	*conffile;	/* where the configuration is saved */
User	*u;		/* per process data structure */

int	tracing;	/* show aoe frames as they arrive */
int	schcount;
int	schavail;
int	msgcount;
int	msgavail;
int	msgfrcount;	/* how many times msgfree was called */
QLock	targetlk;		/* locks targ hash */
ulong	totalext;	/* system wide */
uvlong 	ticks;
int	mirrorck;
int	assembleck;
int	lostck;		/* Check if PVs need to be declared lost */
int	lostpvcnt;	/* Count of lost PVs, protected by lostlk */
int	restricted;
uchar	myea[Nmyea*6];		/* our port addresses */
int	myeaindx;		/* index for next free entry */
RWLock	myealck;
uchar	peerea[Nmyea*6];	/* HA peer port addresses */
int	peereaindx;		/* index for next free entry */
RWLock	peerealck;
Inode	root;
int	shutdown;
ulong	iotimeo;	/* set in xlate.c */
ulong	xcbwaits;	/* ++ with xcblk held */
uvlong	xmits;		/* ++ with taglk held */
uvlong	recvs;		/* ++ with taglk held */
uvlong	rexmits;	/* ++ with taglk held */
uvlong	rexmitlims;	/* ++ with taglk held */
uvlong	resmiss;	/* ++ with taglk held */
uvlong	xmitfails;	/* ++ with taglk held */
ulong	ataerrs;	/* xincu */
ulong	aoeerrs;	/* xincu */
ulong	cows;		/* xincu */
int	tagdebug;
int	logstale;
int	dosnapcal;
int	lvmddebug;
int	togb;	/* B-02862: LV timings of take-over/give-back */
int	nworkers;
int	fakextntiowrite;
int	fakextntioread;
int	enableredir;	/* Toggle redirect */

int 	nsanports;
struct {
	int fd;
	int mbps;
	char name[64];
	uvlong alrt;
} sanports[Nsanports];

#define VSPVMAGIC	"VSP0"
#define VSLVMAGIC	"VSL0"

char *shadowclose;
char *releaselast;
int	alarmflag;
long	snsaveconfig;
int	maxshadows;

XferStat mirrorstat;

/* Soli Deo Gloria */
/* Brantley Coile */
