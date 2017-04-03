#include <u.h>
#include <libc.h>
#include "dat.h"
#include "fns.h"

void
statinit(XferStat *s, int dest, int src, uvlong total)
{
	qlock(s);
	addfilter(&s->filter);
	zfilter(&s->filter);
	s->bytes = 0;
	s->total = 0;
	s->src = src;
	s->dest = dest;
	s->wid = getpid();
	s->total = total;
	qunlock(s);
}

void
statinc(XferStat *s, uvlong bytes, uvlong ms)
{
	qlock(s);
	s->bytes += bytes;
	incfilter(&s->filter, bytes, ms);
	qunlock(s);
}

void
statclr(XferStat *s)
{
	qlock(s);
	s->total = 0;
	delfilter(&s->filter);
	qunlock(s);
}
