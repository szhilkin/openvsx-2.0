#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <libcutil.h>
#include "dat.h"
#include "fns.h"
#include "haconfig.h"

int maxshadows = 4;

enum {
	Ncfg= 150*1024,
};

static char config[Ncfg + 1];
static Lock cfglk;

typedef
struct
{
	int	targ;
	int	mtarg;
	vlong	offset;
} Item;

static char *cp;

static
void
skipws(void)
{
	while (*cp == ' ' || *cp == '\t')
		cp++;
}

static
void
skiptonl(void)
{
loop:
	switch (*cp) {
	case '\n':
		cp++;
	case '\0':
		break;
	default:
		cp++;
		goto loop;
	}
}

static 
int 
eol(void)
{
	switch (*cp) {
	case '\n':
		return 1;
	case '\r':
		cp++;
		return *cp == '\n';
	default:
		return 0;
	}
}

/*
 * getstring parses a quoted string.
 * readconfig() really should use gettokens() to parse a line.
 */
static
char *
getstring(void)
{
	char *e, *p, buf[256];
	int quoted;
	
	skipws();
	if (*cp == 0)
		return nil;
	p = buf;
	e = buf + sizeof buf;
	if (*cp == '\'') {
		quoted = 1;
		*p++ = *cp++;
	} else {
		quoted = 0;
	}
	while (*cp && p < e) {
		if (quoted) {
			if (*cp == '\'') {
				if (cp[1] == '\'') {
					*p++ = *cp++;
					cp++;
				} else {
					*p++ = *cp++;
					break;
				}
			}
		}
		if (*cp == ':' || *cp == ' ' || *cp == '\n')
			break;
		*p++ = *cp++;
	}
	*p = 0;
	return unquotestrdup(buf);
}

static
vlong
getnum(void)
{
	vlong n;

	n = 0;
	while (isdigit(*cp)) {
		n *= 10;
		n += *cp - '0';
		cp++;
	}
	return n;
}

static
int
gettarg(int *targ)
{
	int sh, sl;

	sh = getnum();
	if (*cp != '.')
		return 0;
	cp++;
	sl = getnum();
	*targ = (sh<<8) + sl;
	return 1;
}

static 
Item *
getitem(Item *ip)
{
	ip->targ = ip->mtarg = -1;
	ip->offset = 0;
	skipws();
	if (*cp == '-') {
		cp++;
		return ip;
	}
	if (!gettarg(&ip->targ))
		return nil;
	if (*cp == ',') {
		cp++;
		if (!gettarg(&ip->mtarg))
			return nil;
	}
	if (*cp != ':')
		return nil;
	cp++;
	ip->offset = getnum();
	return ip;
}

static
int
getitems(Item **lp)
{
	int i;
	Item item, *items;

	*lp = items = mallocz(sizeof item * Npvsperpool, 1);

	if (items == nil)
		return -1;

	for (i = 0; getitem(&item); i++) {
		if (i < Npvsperpool)
			items[i] = item;
		else
			return -1;
	}
	return i;
}

static void
getsn(void)
{
	int t;
	char *sn;
	PV *pv;
	ushort usn[Nserial/2];

	if (!gettarg(&t)) {
		xsyslog(LOGCOM "getsn target failed\n");
		return;
	}
	rlock(&lk);
	pv = targ2pv(t);
	runlock(&lk);
	if (!pv) {
		xsyslog(LOGCOM "getsn no PV %T\n", t);
		return;
	}
	sn = getstring();
	if (!sn) {
		xsyslog(LOGCOM "getsn sn failed\n");
		return;
	}
	if (strlen(sn) != Nserial) {
		xsyslog(LOGCOM "getsn bad length \"%s\"\n", sn);
		free(sn);
		return;
	}
	setfld(usn, 0, Nserial, sn);
	wlock(pv);
	if (pv->targ == t)
		memcpy(pv->sn[0], usn, Nserial);
	else if (pv->mirror == t)
		memcpy(pv->sn[1], usn, Nserial);
	wunlock(pv);
	free(sn);
}	

static void
setstripes(void)
{
	char *p;
	PVPool *pvp;

	while (*cp != '\n') {
		p = getstring();
		if (p == nil) {
			xsyslog(LOGCOM "setstripes getstring failed\n");
			continue;
		}
		wlock(&lk);
		for (pvp = pools; pvp && strcmp(pvp->name, p); pvp = pvp->next)
			;
		if (pvp == nil) {
			xsyslog(LOGCOM "setstripes no Pool %s\n", p);
			free(p);
			wunlock(&lk);
			continue;
		}
		free(p);
		pvp->flags |= PVPFstriped;
		wunlock(&lk);
	}
}

enum {
	FLAGtagdebug	= 1<<0,
	FLAGlogstale	= 1<<1,
	FLAGdosnapcal	= 1<<2,
	FLAGlvmddebug	= 1<<3,
};

static void
getflags(void)
{
	char *s;
	ulong flags;

	s = getstring();
	if (s == nil)
		return;
	flags = strtoul(s, nil, 16);
	free(s);

	tagdebug = flags & FLAGtagdebug;
	logstale = flags & FLAGlogstale;
	dosnapcal = flags & FLAGdosnapcal;
	lvmddebug = flags & FLAGlvmddebug;
}

static char *
writeflags(char *p, char *ep)
{
	ulong flags;

	flags = 0;
	if (tagdebug)
		flags |= FLAGtagdebug;
	if (logstale)
		flags |= FLAGlogstale;
	if (dosnapcal)
		flags |= FLAGdosnapcal;
	if (lvmddebug)
		flags |= FLAGlvmddebug;

	return seprint(p, ep, "%%flags 0x%ulx\n", flags);
}

int rcd;
#define rcdp if (!rcd) USED(rcd); else print
	
void
readconfig(void)		/* get the config from the local store */
{
	int nitems, n;
	int i, sign, fd;
	Item *items;
	char *name, *label; 
	PVPool *pvp;

	items = nil;
	name = nil;
	label = nil;

	lock(&cfglk);
	fd = haopenconfig(conffile);
	if (fd < 0) {
		unlock(&cfglk);
		return;
	}
	// XXX read in entire file, regardless of size (8K might not be big enough!)
	n = read(fd, config, Ncfg);
	if (n <= 0)
		goto e;
	config[n] = 0;
	cp = config;
	// first read pools specification
	while (*cp != '%') {
		name = getstring();
		skipws();
		if (*cp != ':') {
			goto e;
		}
		cp++;
		skipws();
		if (!isdigit(*cp)) {	/* fake 0 for pool reserve */
			goto e;
		}
		strtol(cp, &cp, 10);
		skipws();
		label = getstring();
		skipws();
		if (!eol()) {
			xsyslog("readconfig: unknown pool config format\n");
			goto e;
		}
		cp++;
		skipws();
		nitems = getitems(&items);
		if (nitems < 0) {
			xsyslog("readconfig: cannot add more than %d PVs per pool\n", Npvsperpool);
			goto e;
		}
		skipws();
		if (!eol()) {
			xsyslog("readconfig: unknown pool element config format\n");
			goto e;
		}
		cp++;
		pvp = xlmkpool(name, label);
		if (pvp == nil) {
			xsyslog("failure loading pool %s: %r\n", name);
			goto e;
		}
		rcdp("making pool %q lab=%q nitems=%d\n", name, label, nitems);
		for (i = 0; i < nitems; i++) {
			if (items[i].targ == -1)
				continue;
			rcdp("adding %T,%T:%lld\n", items[i].targ, items[i].mtarg, items[i].offset);
			if (xlinitpv(pvp, i, items[i].targ, items[i].mtarg, items[i].offset) < 0) {
				xsyslog("failure initializing pool %s from config, halting", pvp->name);
				assert(0);
			}
		}
	}
	while (*cp == '%') {
		cp++;
		if (strncmp(cp, "end", 3) == 0)
			break;
		if (strncmp(cp, "shelf", 5) == 0) {
			cp += 5;
			skipws();
			if (*cp == '-') {
				sign = -1;
				cp++;
			} else
				sign = 1;
			shelf = getnum() * sign;
		} else if (strncmp(cp, "sn", 2) == 0) {
			cp += 2;
			skipws();
			getsn();
		} else if (strncmp(cp, "stripe", 6) == 0) {
			cp += 6;
			setstripes(); 
		} else if (strncmp(cp, "flags", 5) == 0) {
			cp += 5;
			skipws();
			getflags();
		} else if (strncmp(cp, "maxshadows", 10) == 0) {
			cp += 10;
			skipws();
			maxshadows = getnum();
		}
		skiptonl();
	}
e:	close(fd);
	free(items);
	free(name);
	free(label);
	unlock(&cfglk);
}

void
writeconfig(void)	/* save the configuration to the local store */
{
	int i, targ, npv;
	char *p, *ep;
	PVPool *pvp;
	PV *pv;
	vlong offset;
	char sn[Nserial+1];

	lock(&cfglk);
	p = config;
	ep = p + Ncfg;
	rlock(&lk);
	for (pvp = pools; pvp; pvp = pvp->next) {
		p = seprint(p, ep, "%s: 0 %q\n\t", pvp->name, pvp->label); /* false 0 for pool reserve */
		for (i = npv = 0; i < Npvsperpool && npv < pvp->npv; i++) {
			rlock(pvp);
			pv = pvp->pv[i];
			if (pv) {
				npv++;
				offset = pv->offset;
				targ = offset ? pv->mtarg : pv->targ;
				p = seprint(p, ep, "%T", targ);
				if (pv->state == PVSdouble)
					p = seprint(p, ep, ",%T", pv->mirror);
				p = seprint(p, ep, ":%lld ", offset);
			} else
				p = seprint(p, ep, "- ");
			runlock(pvp);
		}
		p = seprint(--p, ep, "\n");
	}
	for (pvp = pools; pvp; pvp = pvp->next) {
		rlock(pvp);
		for (i = npv = 0; i < Npvsperpool && npv < pvp->npv; i++) {
			pv = pvp->pv[i];
			if (!pv)
				continue;
			npv++;
			if (!pv->sn[0][0])
				continue;
			sncpy(sn, pv->sn[0]);
			p = seprint(p, ep, "%%sn %T %q\n", pv->targ, sn);
			if (!pv->sn[1][0])
				continue;
			sncpy(sn, pv->sn[1]);
			p = seprint(p, ep, "%%sn %T %q\n", pv->mirror, sn);
		}
		runlock(pvp);
	}
	p = seprint(p, ep, "%%stripe");
	for (pvp = pools; pvp; pvp = pvp->next) {
		rlock(pvp);
		if (pvp->flags & PVPFstriped)
			p = seprint(p, ep, " %s", pvp->name);
		runlock(pvp);
	}
	p = seprint(p, ep, "\n");
	runlock(&lk);
	p = writeflags(p, ep);
	p = seprint(p, ep, "%%maxshadows %d\n", maxshadows);
	p = seprint(p, ep, "%%shelf %d\n", shelf);
	p = seprint(p, ep, "%%end of config\n");
/**/	if (p >= ep)
/**/		print("WARNING: writeconf buffer too small\n");

	hawriteconfig(conffile, -1, config, p - config, 0);
	unlock(&cfglk);
}


/* Soli Deo Gloria */
/* Brantley Coile */
