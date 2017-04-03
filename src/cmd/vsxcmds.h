#define max(A, B) ((A) > (B) ? (A) : (B))

enum {
	Yesto	= 0,			/* 'to' name is allowed */
	Noto	= 1,			/* 'to' is a reserved name */
	SameOK	= 2,			/* skip same name checking */

	Pri,				/* getpv			*/
	Mir,				/* getpv			*/
	Write = 0200,			/* Writable bit in LVS mode	*/
	Ext2B = 4194304,
	Byte2G = 1000000000
};

enum {
	RespondOne,			/* ask respond one	*/
	RespondAll			/* ask respond all	*/
};
extern int askres;			/* ask response	var	*/

void askhdr(int argc, char **argv);	/* confirmation header		*/
int askresponse(void);			/* returns 0 for yes		*/
					/*	   1 for no		*/
					/*  sysfatal for anything else	*/
void ask(char *str);			/* asks for confirmation	*/

extern char *delim;
enum {			/* Print lengths */
	Ptotal	= 80,				/* Max print width			*/		
	Pahdr	= 15,				/* -a flag width			*/
	Pleft	= Pahdr + 3,			/* filler space for Pahdr + delim	*/
	Plen	= Ptotal - Pleft		/* remaining avaiable space		*/
};

/* PV Flags */
enum {
	PVFpartial = (1<<0),	/* partial extent on end */
	PVFsilvering = (1<<1),	/* mirror being silvered */
	PVFabort = (1<<2),	/* abort silver */
	PVFBZ3318 = (1<<3),	/* BZ3318 workaround */
	PVFlost = (1<<4),	/* PV was loaded, but disappeared */
};

int ispool(char *name);				/* Return true if name is a pool	*/
int islv(char *name);				/* Return true if name is an lv		*/
char *getpvindex(char *pv);			/* Return index of pv;  nil if unknown	*/
char *getpvpool(char *pv);			/* Return pool  of pv;  nil if unknown	*/
char *getmirindex(char *aoe);			/* Return index of mir; nil if unknown	*/
char *getmirpool(char *aoe);			/* Return pool  of mir; nil if unknown	*/
int getpv(char *pool, char *index, int state);	/* Return pri target; -1 on error	*/

int ctlwrite(char *fmt, ...);			/* Write msg into top    ctl file	*/
int poolctlwrite(char *pool, char *fmt, ...);	/* Write msg into pool's ctl file	*/
int lvctlwrite(char *lv, char *fmt, ...);	/* Write msg into lv's   ctl file	*/
int lunctlwrite(int offset, char *fmt, ...);	/* Write msg into lun's  ctl file	*/
int writefile(char *file, char *fmt, ...);	/* Write msg into file			*/
int readfile(char *b, int len, char *fmt, ...);	/* Return number of stored bytes at b	*/
int numfiles(char *directory, Dir **dp);	/* Return number of files in directory	*/
						/* Stores Dir buffers at dp; -1 on error*/
void timeoutfatal(void);			/* errfatals if the cmd timed out       */

#pragma varargck argpos lvctlwrite 2
#pragma varargck argpos ctlwrite 1
#pragma varargck argpos poolctlwrite 2
#pragma varargck argpos lunctlwrite 2
#pragma varargck argpos writefile 2
#pragma varargck argpos readfile 3

int parsess(char *s);		/* parse shelf.slot to an int, print with Tfmt	*/
int Tfmt(Fmt *f);		/* formating of shelf.slot from integer		*/
				/* %-T;	left justified	(outputs 9 characters)	*/
				/* %#T;	. justified	(outputs 9 characters)	*/
				/* %T;	right justified	(outputs 9 characters)	*/
				/* %,T;	no formatting				*/
int Zfmt(Fmt *f);		/* formatting of extents to GB			*/
				/* %Z; right justified	(outputs 10 characters)	*/
				/* %,Z: precision of 3	(outputs x.000)		*/
int Bfmt(Fmt *f);		/* formatting of bytes to GB			*/
				/* %B; right justified	(outputs 10 characters)	*/
				/* %,B: precision of 3	(outputs x.000)		*/
#pragma varargck type "T" int
#pragma varargck type "Z" int
#pragma varargck type "Z" uint
#pragma varargck type "Z" ulong
#pragma varargck type "B" vlong
#pragma varargck type "B" uvlong

int dirnamecmp(Dir *a, Dir *b);		/* cmp for Dir names that are strings		*/
int dirintcmp(Dir *a, Dir *b);		/* cmp for Dir names that are ints		*/
int dirnamesscmp(Dir *a, Dir *b);	/* cmp for Dir names that are shelf.slots	*/
int dirlvcmp(Dir *a, Dir *b);		/* cmp for Dir names that are text.index	*/
vlong fstrtoll(char *str);		/* string to vlong byte converstion		*/
int getposint(char *str);		/* return pos int, -1 if non-digit exists	*/
char *mustsmprint(char *fmt, ...);	/* Returns a pointer from malloc call		*/
void *mustmalloc(ulong size);		/* Sysfatals if it fails 			*/
#pragma varargck argpos mustsmprint 1

typedef struct Pool Pool;
struct Pool
{				/* pool status fields		*/
	ulong	freeext;	/* free extents			*/
	ulong	totalext;	/* total extents		*/
	ulong	unique;		/* unique extents 		*/
	char*	mode;		/* pool PV allocation mode	*/
};
Pool *getpoolstatus(char *pool);/* Return a malloced *Pool	*/
void freepool(Pool *pool);	/* free a Pool struct		*/

typedef struct Pvs Pvs;
struct Pvs
{				/* pv status fields		*/
	char	*pool;		/* pv pool			*/
	char	*state;		/* missing, single, mirroed, 	*/
				/* broke, oosync, relivering	*/
	int	freeext;	/* free extents			*/
	int	dirtyext;	/* dirty extents		*/
	int	usedext;	/* used extents			*/
	int	metaext;	/* extents used for meta data	*/
	uint	totalext;	/* total amount of extent	*/
	int	primary;	/* PV target			*/
	int	mirror;		/* mir, -1.255 if unset		*/
	vlong	length;		/* length in bytes		*/
	ulong	ctime;		/* creation date		*/
	ulong	flags;		/* pv flags			*/
};
Pvs *getpvstatus(char *pv);	/* Return a malloced *Pvs	*/
Pvs *getpvstatusbyindex(char *, char *); /* return *Pvs		*/
void freepvs(Pvs *p);		/* free a Pvs struct		*/

typedef struct Lvs Lvs;
struct Lvs
{				/* lv status fields		*/
	ulong	mode;		/* mode (in hex)		*/
	char	*pool;		/* lv's pool			*/
	uint	totalext;	/* total extents		*/
	int	thinext;	/* thin extents			*/
	int	dirtyext;	/* dirty extents		*/
	int	uniqext;	/* unique extents		*/
	ulong	ctime;		/* creation date		*/
	char	*rmtname;	/* remote VSX-pair name		*/
	char	*rmtlv;		/* remote VSX-pair LV		*/
	vlong	length;		/* declared length in bytes	*/
	int	issnap;		/* Is snapshot (normal or snap)	*/
	int	lun;		/* Lun, -1.255 if unset		*/
	vlong	actlen;		/* actual length		*/
	int	vlan;		/* vlan 			*/
	uint	snapext;		/* snap extents	*/
	char 	*name;		/* name of the LV		*/
	char	serial[21];
	char	class[16];
	int	nscheds;	/* num schedules in array */
	int	scheds[32][7];	/* schedule array */
	char	*state;		/* healthy or suspended		*/
};
Lvs *getlvstatus(char *lv);	/* Return a malloced *Lvs	*/
void freelvs(Lvs *lv);		/* free a Lvs struct		*/

typedef struct Lun Lun;
struct Lun 
{				/* lun status fiels		*/
	char	*status;	/* online/offilne		*/
	char	*lv;		/* lv name			*/
	int 	lun;		/* lun assignment		*/
	int	loffset;		/* offset of lun relative to base addr. 0-4095 */
};
extern int bshelf;		/* base shelf for getshelf	*/
void getshelf(void);		/* return shelf or -1 if unset	*/
Lun *getlunstatus(char *lun);	/* Return a malloced *Lun	*/
				/* lun must be shelf.slot form	*/
void freelun(Lun *l);		/* free a lun struct		*/

int  serieserr(int argc, char **argv, int to);	/* series check */
int serieserrs(int argc, char **argv, int to, int start); /* with start */

extern int lvlen;
extern int poollen;
void lvmaxlen(void);		/* return max lv name len 	*/
void poolmaxlen(void);		/* return max pool name len 	*/

typedef struct Ios Ios;
struct Ios {
	int ns;
	uvlong bytes;
	uvlong lavg;
	uvlong lmax;
};

void parseiostats(Ios *ios, char *str);	/* parse iostats output */
char * mbstr(char *p, int n, ulong bytes);
typedef struct Iops Iops;
struct Iops {
	int ns;
	uvlong io[6];
};

void parseiops(Iops *iops, char *str);	/* parse iops output */

void errfatal(char *fmt, ...);	/* my own sysfatal 		*/
#pragma varargck argpos errfatal 1

void errskip(int argc, char **argv);

int isinactive(void);
void mountremote(void);

enum {
	LVSEND		= 0x10000000,	/* shadowsend */
	LVNOHOLD	= 0x04000000,	/* can autoremove snapshot */
	LVTHIN		= 0x00200000,	/* LV is thin */
};

enum {
	LVSmanual =	0,		/* For snaps, these define classes */
	LVShour,
	LVSday,
	LVSweek,
	LVSmonth,
	LVSyear,
};

char *clsstr(int);
char *schedstr(int *);
int getsnapsched(char *, int, int *);
