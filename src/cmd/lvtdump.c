#include <u.h>
#include <libc.h>
#include <ctype.h>

typedef struct LVE LVE;

#pragma pack on

struct LVE {
	uchar	flag;
	ushort	pid;
	uint	off;
};

#pragma pack off

enum {
	Xblk		= 8192,
	Xlveperblk	= Xblk / sizeof (LVE),

	LFthin		= (1<<0),
	LFdirty		= (1<<1),
	LFnf		= (1<<2),
};

void fl2str(uchar, char *);
void lve2str(LVE *, char *);

void
main(int argc, char *argv[])
{
	int fd, ext;
	uchar buf[Xblk];
	char flstr[8];
	char lvestr[16];
	LVE *p, *e;

	ARGBEGIN {
	} ARGEND;

	if (argc != 1)
		sysfatal("usage: %s lvtfile", argv0);

	fd = open(*argv, OREAD);
	if (fd < 0)
		sysfatal("error: cannot open %s", *argv);

	ext = 0;
	while (read(fd, buf, sizeof buf) == sizeof buf) {
		p = (LVE *) buf;
		e = p + Xlveperblk;
		for (; p<e; p++) {
			fl2str(p->flag, flstr);
			lve2str(p, lvestr);
			print("%05d %4s %04x %08x '%7s'\n", ext++, flstr, p->pid, p->off, lvestr);
		}
	}
}

void
fl2str(uchar fl, char *p)
{
	if (fl & LFthin)
		*p++ = 'T';
	if (fl & LFdirty)
		*p++ = 'D';
	if (fl & LFnf)
		*p++ = 'F';
	*p = 0;
}

void
lve2str(LVE *lve, char *p)
{
	uchar *x, *xe;

	x = (uchar *) lve;
	xe = x + sizeof *lve;
	for (; x<xe; x++)
		*p++ = isprint(*x) ? *x : '.';
	*p = 0;
}

