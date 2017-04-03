// copyright © 2011 CORAID
// coraid, inc. All rights reserved.
// Routines for handling snapshot scheduling

#include <u.h>
#include <libc.h>
#include	"dat.h"
#include	"fns.h"
#include	"ctime.h"

struct {
	Sch 	*hd;
	Sch	*tl;
	int	nq;
	Rendez;
	QLock;
} schq;

/* Called with pl and l locked */
int
canprune(LV *pl, LV *l) {
	int snap;

	if ((l->mode & LVCANPRUNE) == 0) 
		return 0;
	if (isshadowsend(pl) == 0) {
		return 1;
	}
	snap = issnap(l);
	if (snap == 0) {
		xsyslog("LV %s: unable to acquire snap number\n", l->name);
		return 0;
	}
	if (snap > pl->copysnap) {
		xsyslog("LV %s: not eligible for automatic removal until shadow copy complete %d>%uld\n",
			l->name, snap, pl->copysnap);
		return 0;
	}
	if (snap == pl->copysnap) {
		xsyslog("LV %s: not eligible for automatic removal because it is the last snapshot copied %d\n",
			l->name, snap);
		return 0;
	}
	return 1;
}

/* cmp everything EXCEPT retain count */
int
schedcmp(Snapsched *s1, Snapsched *s2)
{
	int t;

	if ((t = s1->class - s2->class))
		return t;
	if ((t = s1->mon - s2->mon))
		return t;
	if ((t = s1->mday - s2->mday))
		return t;
	if ((t = s1->wday - s2->wday))
		return t;
	if ((t  = s1->hour - s2->hour))
		return t;
	if ((t = s1->min - s2->min))
		return t;
	return 0;
}

static Sch *
schalloc(void)
{
	Sch *s;

	qlock(&schlock);
	if ((s = freesch) == nil) {
		schcount++;
		s = malloc(sizeof *s);
		if (s == nil) {
			qunlock(&schlock);
			return nil;
		}
	} else {
		schavail--;
		freesch = s->next;
	}
	qunlock(&schlock);
	memset(s, 0, sizeof *s);
	return s;
}

static void
schfree(Sch *s)
{
	qlock(&schlock);
	s->next = freesch;
	freesch = s;
	schavail++;
	qunlock(&schlock);
}

static int
snaptime(Tm *t, Snapsched *s)
{

	// Everybody has to match minute
	if(t->min != s->min) {
		return 0;
	}
	switch (s->class) {
	case LVShour:
		return 1;
	case LVSday:
		if (t->hour == s->hour) 
			return 1;
	case LVSweek:
		if (t->hour == s->hour && t->wday == s->wday)
			return 1;
	case LVSmonth:
		if (t->hour == s->hour && t->mday == s->mday)
			return 1;
	case LVSyear:
		if (t->hour == s->hour && t->mday == s->mday && t->mon == s->mon)
			return 1;
	}
	return 0;
}

void
queuesch(Sch *s)
{
	s->next = nil;
	qlock(&schq);
	if (schq.hd)
		schq.tl->next = s;
	else
		schq.hd = s;
	schq.tl = s;
	schq.nq++;
	rwakeup(&schq);
	qunlock(&schq);	
}

void
worksch(LV *l, Snapsched *ss)
{	
	Sch *sch;

	sch = schalloc();
	if (sch == nil)
		return;
	memcpy(&sch->s, ss, sizeof(Snapsched));
	sch->lv = l;
	queuesch(sch);
}
static int
getschedidx(LV *l, Snapsched *s)
{
	int i;

	if (s == nil) 
		return -1;
	for (i = 0; i < Nsched; i++) {
		if (schedcmp(&l->sched[i], s) == 0)
			return i;
	}
	return -1;
}

int
sched2retain(LV *l, Snapsched *s)	/* called with lv locked */
{
	int idx;

	if (l == nil || s == nil)
		return -1;
	idx = getschedidx(l, s);
	return idx == -1 ? -1 : l->sched[idx].retain;
}

/* wlock on lk, qlock on l */
int
retainprune(LV *l, Snapsched *s)
{
	LVL *op, *p;
	int n, retain, ret;
	char *lname, *pname;

	op = nil;
	retain = sched2retain(l, s);
	if (retain == -1) {
		return 0;
	}
	pname = strdup(l->pool->name);
	if (pname == nil) {
		xsyslog("error: retainprune dup failure\n");
		goto e;
	}
	n = snaplist(l, &op, s);
	if (n < 0) {
		xsyslog("error: retainprune cannot find snaplist\n");
		goto e;
	}
	for (p = op; p && n >= retain; p = p->next) {
		wlock(p->l);
		if (canprune(l, p->l) == 0) {
			wunlock(p->l);
			continue;
		}
		lname = strdup(p->l->name);
		if (lname == nil) {
			wunlock(p->l);
			xsyslog("error: retainprune dup failure\n");
			goto e;
		}
		wunlock(l);
		if (rmlv(p->l) < 0) {
			xsyslog("error: retainprune cannot remove LV %s: %s\n", lname, u->err);
			wunlock(p->l);
		} else {	/* rmlv will unlock p->l on success*/
			xsyslog("Snap %s: %s scheduled removal from pool %s\n", lname, fmtsnapclass(s), pname);
			n--;
		}
		wlock(l);
		free(lname);
	}
	ret = n >= retain ? -1 : 0;
	goto r;
e:
	ret = -1;
r:
	free(pname);
	freelvl(op);
	return ret;
}

int
limitprune(LV *l) 	/* called with l locked and wlock on lk */
{
	LVL *op, *p;
	char *lname;
	ulong used;
	
	/* Don't signal an error if snaplimit is unset, just return. We'll handle it later */
	if (l->snaplimit == SLunset || l->snaplimit == SLign)
		return 0;
	used = usedextents(l);
	if ((used + l->nse) * Xextent < l->snaplimit)
		return 0;
	snaplist(l, &op, nil);
	for (p = op; p; p = p -> next) {
		/* sweet, we are done */
		if ((used + l->nse) * Xextent < l->snaplimit) {
			freelvl(op);
			return 0;
		}
		wlock(p->l);
		if (canprune(l, p->l) == 0) {
			wunlock(p->l);
			continue;
		}
		lname = strdup(p->l->name);
		if (lname == nil) {
			xsyslog("error: allocation failure in limitprune\n");
			wunlock(p->l);
			freelvl(op);
			return -1;
		}
		wunlock(l); /* rmlv may attempt to lock this LV, best unlock */
		if (rmlv(p->l) < 0) {
			xsyslog("error: snaplimit cannot remove LV %s: %s\n", lname, u->err);
			wunlock(p->l);
		} else {	/* rmlv will unlock p->l on success*/
			xsyslog("Snap %s: limit reached: removed from pool %s\n", lname, l->pool->name);
		}
		wlock(l);
		free(lname);
	}
	freelvl(op);
	return ((used + l->nse) * Xextent < l->snaplimit) ? 0 : -1;
}

static void
doschedproc(void) 
{
	Sch *s;
	int r;
	LV *l;
	
	for(;;) {
		qlock(&schq);
		while((s = schq.hd) == nil)
			rsleep(&schq);
		schq.hd = s->next;
		schq.nq--;
		qunlock(&schq);
		if (s->retries && s->time == time(nil))
			sleep(1000);
		wlock(&lk);
		/* paranoid, the lv could be stale */
		for (l = vols; l; l = l->next) {
			if (l == s->lv)
				break;
		}
		if (l != s->lv) {
			wunlock(&lk);
			schfree(s);
			continue;
		}
		/* paranoid, the schedule could be stale */
		wlock(s->lv);
		if (getschedidx(l, &s->s) == -1) {
			wunlock(s->lv);
			wunlock(&lk);
			schfree(s);
			continue;
		}
		if (l->mode & LVSNAP || isshadowrecv(l)) {
			wunlock(s->lv);
			wunlock(&lk);
			schfree(s);
			continue;
		}
		if (l->flags & LVFsuspended) {
			wunlock(s->lv);
			wunlock(&lk);
			schfree(s);
			xsyslog("LV %s: %s snapshot failed because LV suspended\n", 
				l->name, fmtsnapclass(&s->s));
			continue;
		}
		s->time = time(nil);
		r = snapclone(s->lv, OREAD, s->time, nil, &s->s, 0, nil);
		wunlock(s->lv);
		wunlock(&lk);
		switch (r) {
		case -1:
			xsyslog("LV %s: %s snapshot failure: %s\n", l->name, fmtsnapclass(&s->s), u->err);
			schfree(s);
			break;
		case -2:
			if (s->retries++ < 5) {
				queuesch(s);
			} else {
				schfree(s);
				xsyslog("LV %s: %s snapshot skipped\n", l->name, fmtsnapclass(&s->s));
			}
			break;
		case 0:
			schfree(s);
		}
	}
}

static void
schedproc(void)
{
	LV *l;
	int i;
	long emin, last, n;
	Tm *t;

	last =  time(nil) / 60; /* Minutes since the epoch */
	for (;;) {
		if (shutdown) 
			xlexits(0);
		emin = time(nil) / 60;
		/* The clock shifted backwards in time */
		if (last > (emin + 1))
			last = emin;
		while (last <= emin) {
			t = localtimetz(last * 60);
			rlock(&lk);
			for (l = vols; l; l = l->next) {
				rlock(l);
				if (l->mode & LVSNAP || l->snaplimit == SLunset || isshadowrecv(l)) {
					runlock(l);
					continue;
				}
				for (i = 0; i < Nsched; i++) {
					if (l->sched[i].retain && snaptime(t, &l->sched[i])) {
						worksch(l, &l->sched[i]);
					}
				}
				runlock(l);
			}
			runlock(&lk);
			last++;
		}
		n = time(nil);
		/* Check if sleep is needed */
		if (n / 60 == emin) {
			t = localtimetz(n);
			sleep ((60 - t->sec) * 1000);
		}
	}
}

int
strtoday(char *c)
{
	long cl;

	cl  = strlen(c);
	if (cl < 2) {
		uerr("Unknown day: %s", c);
		return -1;
	}
	if (cistrncmp(c, "sunday", cl) == 0)
		return Sun;
	if (cistrncmp(c, "monday", cl) == 0)
		return Mon;
	if (cistrncmp(c, "tuesday", cl) == 0)
		return Tue;
	if (cistrncmp(c, "wednesday", cl) == 0)
		return Wed;
	if (cistrncmp(c, "thursday", cl) == 0)
		return Thu;
	if (cistrncmp(c, "friday", cl) == 0)
		return Fri;
	if (cistrncmp(c, "saturday", cl) == 0)
		return Sat;
	uerr("Unknown day: %s", c);
	return -1;

}

char *
wdaytostr(int day) 
{
	switch (day) {
	case Sun:
		return "Sunday";
	case Mon:
		return "Monday";
	case Tue:
		return "Tuesday";
	case Wed:
		return "Wednesday";
	case Thu:
		return "Thursday";
	case Fri:
		return "Friday";
	case Sat:
		return "Saturday";
	}
	return nil;
}

char *
monthtostr(int month)
{
	switch(month) {
	case Jan:
		return "January";
	case Feb:
		return "February";
	case Mar:
		return "March";
	case Apr:
		return "April";
	case May:
		return "May";
	case Jun:
		return "June";
	case Jul:
		return "July";
	case Aug:
		return "August";
	case Sep:
		return "September";
	case Oct:
		return "October";
	case Nov:
		return "November";
	case Dec:
		return "December";
	}
	return nil;
}

void
schedtostr(Snapsched *sc, char *buf,  int len) 
{
	char *e;

	e = buf + len;
	switch(sc->class) {
	case LVShour:
		seprint(buf, e, "hourly @%.2d", sc->min);
		break;
	case LVSday:
		seprint(buf, e, "daily @%.2d:%.2d", sc->hour, sc->min);
		break;
	case LVSweek:
		seprint(buf, e, "weekly on %s@%.2d:%.2d", wdaytostr(sc->wday), sc->hour, sc->min);
		break;
	case LVSmonth:
		seprint(buf, e, "monthly on day %d@%.2d:%.2d", sc->mday, sc->hour, sc->min);
		break;
	case LVSyear:
		seprint(buf, e, "yearly on %s %d@%.2d:%.2d", monthtostr(sc->mon), 
				sc->mday, sc->hour, sc->min);
		break;
	default:
		if (len) 
			*buf = 0;
	}

}

void
schedtostimestr(Snapsched *sc, char *buf, int len)
{
	char *e;

	e = buf + len;
	switch(sc->class) {
	case LVShour:
		seprint(buf, e, "@%.2d", sc->min);
		break;
	case LVSday:
		seprint(buf, e, "@%.2d:%.2d", sc->hour, sc->min);
		break;
	case LVSweek:
		seprint(buf, e, "%s@%.2d:%.2d", wdaytostr(sc->wday),
			sc->hour, sc->min);
		break;
	case LVSmonth:
		seprint(buf, e, "%d@%.2d:%.2d", sc->mday, sc->hour, sc->min);
		break;
	case LVSyear:
		seprint(buf, e, "%s.%d@%.2d:%.2d", monthtostr(sc->mon),
			sc->mday, sc->hour, sc->min);
		break;
	default:
		seprint(buf, e, "manual");
	}
}

char *
sclstostr(int d)
{
	switch(d) {
	case LVShour:
		return "hourly";
	case LVSday:
		return "daily";
	case LVSweek:
		return "weekly";
	case LVSmonth:
		return "monthly";
	case LVSyear:
		return "yearly";
	default:
		return "unknown";
	}

}

int
strtoscls(char *c)
{
	long cl;
	
	cl = strlen (c);
	if (cistrncmp(c, "hourly", cl) == 0)
		return LVShour;
	if (cistrncmp(c, "daily", cl) == 0)
		return LVSday;
	if (cistrncmp(c, "weekly", cl) == 0)
		return LVSweek;
	if (cistrncmp(c, "monthly", cl) ==0)
		return LVSmonth;
	if (cistrncmp(c, "yearly", cl) == 0)
		return LVSyear;
	uerr("invalid class: %s",c);
	return -1;
}

static int
parsehourly(Snapsched *s, char *time)
{
	ulong l;
	char *e;

	if (*time != '@') {
		uerr("missing @: %s", time);
		return -1;
	}

	if (*++time == 0) {
		uerr("invalid minute:");
		return -1;
	}

	l = strtoul(time, &e, 10);
	if (*e || l > 59) {
		uerr("invalid minute: %s", time);
		return -1;
	}
	s->min = l;
	return 0;
}

static int
parsedaily(Snapsched *s, char *time)
{
	char *e, *e2;
	ulong l;

	if (*time != '@') {
		uerr("missing @: %s", time);
		return -1;
	}
	l = strtoul(++time, &e, 10);
	if (*e != ':' || l > 23) {
		uerr("invalid hour of day: %s", time);
		return -1;
	}
	s->hour = l;
	
	if (*++e == 0) {
		uerr("invalid hour of day:");
		return -1;
	}
	l = strtoul(e, &e2, 10);
	if (*e2 || l > 59) {
		uerr("invalid minute value: %s", time);
		return -1;
	}
	s->min = l;
	return 0;
}

static int
parseweekly(Snapsched *s, char *time)
{
	char *e;
	long l;

	e = strchr(time, '@');
	if (!e) {
		uerr("missing @: %s", time);
		return 1;
	}
	l = e - time;
	if (l < 2) {
		uerr("invalid day: %s", time);
		return -1;
	}
	if (cistrncmp("sunday", time, l) == 0)
		s->wday = Sun;
	else if (cistrncmp("monday", time, l) == 0)
		s->wday = Mon;
	else if (cistrncmp("tuesday", time, l) == 0)
		s->wday = Tue;
	else if (cistrncmp("wednesday", time, l) == 0)
		s->wday = Wed;
	else if (cistrncmp("thursday", time , l) == 0)
		s->wday = Thu;
	else if (cistrncmp("friday", time, l) == 0)
		s->wday = Fri;
	else if (cistrncmp("saturday", time, l) == 0)
		s->wday = Sat;
	else {
		uerr("invalid day: %s", time);
		return -1;
	}
	return parsedaily(s, time + l);
}

static int
parsemonthly(Snapsched *s, char *time)
{
	ulong l;
	char *e;

	l = strtoul(time, &e, 10);
	if (*e != '@') {
		uerr("missing @: %s", time);
		return -1;
	}
	if (l > 28 || l == 0) {
		uerr("invalid day of month: %s", time);
		return -1;
	}
	s->mday = l;
	return parsedaily(s, e);
}

static int
parseyearly(Snapsched *s, char *time)
{
	ulong l;
	char *p;	
	
	p = strchr(time, '.');
	if (p == nil) {
		uerr("invalid format: %s", time);
		return -1;
	}
	l = p - time;
	if (l < 3) {
		uerr("invalid month: %s", time);
		return -1;
	}
	if (cistrncmp("january", time, l) == 0) {
		s->mon = Jan;
	} else if (cistrncmp("february", time, l) == 0) {
		s->mon = Feb;
	} else if (cistrncmp("march", time, l) == 0) {
		s->mon = Mar;
	} else if (cistrncmp("april", time, l) == 0) {
		s->mon = Apr;
	} else if (cistrncmp("may", time, l) == 0) {
		s->mon = May;
	} else if (cistrncmp("june", time, l) == 0) {
		s->mon = Jun;
	} else if (cistrncmp("july", time, l) == 0) {
		s->mon = Jul;
	} else if (cistrncmp("august", time, l) == 0) {
		s->mon = Aug;
	} else if (cistrncmp("september", time, l) == 0) {
		s->mon = Sep;
	} else if (cistrncmp("october", time, l) == 0) {
		s->mon = Oct;
	} else if (cistrncmp("november", time, l) == 0) {
		s->mon = Nov;
	} else if (cistrncmp("december", time, l) == 0) {
		s->mon = Dec;
	} else {
		uerr("invalid month: %s", time);
		return -1;
	}
	time = ++p;
	l = strtoul(time, &p, 10);
	if (*p != '@') {
		uerr("missing @: %s", time);
		return -1;
	}
	if (l > 28 || l == 0) {
		uerr("invalid day of month: %s", time);
		return -1;
	}
	s->mday = l;
	return parsedaily(s, p);
}

int
parsestime(Snapsched *s, char *time)
{
	switch (s->class) {
	case LVShour:
		return parsehourly(s, time);
	case LVSday:
		return parsedaily(s, time);
	case LVSweek:
		return parseweekly(s, time);
	case LVSmonth:
		return parsemonthly(s, time);
	case LVSyear:
		return parseyearly(s, time);
	}
	uerr("invalid class: %d", s->class);
	return -1;
}

/* Called with lv locked */
int
savesched(LV *lv, Snapsched *s) 
{
	int i, avail;

	/* We've got to make sure this isn't an update */
	avail = -1;
	for (i = 0; i < Nsched; i++) {
		if (lv->sched[i].retain == 0 && avail == -1) 
			avail = i;
		if (lv->sched[i].retain != 0 && schedcmp(&lv->sched[i], s) == 0) {
			lv->sched[i].retain = s->retain;
			return 0;
		}

	}
	if (avail != -1) {
		memcpy(&lv->sched[avail], s, sizeof(Snapsched));
	} else {
		uerr("unable to add more than %d snapsched schedules",Nsched);
		return -1;
	}
	return 0;
}

/* called with lv locked, caller responsible for saving meta */
int
delsched(LV *lv, Snapsched *s)
{
	int idx;

	idx = getschedidx(lv, s);
	if (idx == -1)
		uerr("snap schedule does not exist");
	else
		memset(&lv->sched[idx], 0, sizeof (Snapsched));
	return idx;
}

/* called with lv locked */
void
clrsched(LV *lv)
{
	memset(lv->sched, 0, sizeof lv->sched);
	xsyslog("LV %s: snapshot schedule cleared\n", lv->name);
}

void
schedinit(void)
{
	schq.l = &schq;
	if (xlrfork(RFPROC|RFMEM, "schedproc") == 0) {
		schedproc();
	}
	if (xlrfork(RFPROC|RFMEM, "doschedproc") == 0) {
		doschedproc();
	}
}
