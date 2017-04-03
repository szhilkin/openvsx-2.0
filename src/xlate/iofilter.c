// copyright © 2010 CORAID
// coraid, inc. All rights reserved.
// this has been forked out to let devrd, devsr share this code. 

#include <u.h>
#include <libc.h>
#include	"dat.h"
#include	"fns.h"

static struct {
	QLock;
	Iofilter *f;
} filters;

uvlong
todµs(void)
{
	return nsec() / 1000;
}

void
zfilter(Iofilter *f)
{
	qlock(f);
	memset(f->samples, 0, sizeof f->samples);
	f->nsamples = 0;
	f->bytes = 0;
	f->lmax = 0;
	f->nlat = 0;
	f->lsum = 0;
	f->io512 = 0;
	f->io1024 = 0;
	f->io2048 = 0;
	f->io4096 = 0;
	f->io8192 = 0;
	f->iog8192 = 0;
	qunlock(f);
}

/*
 * Return a pointer to the next pointer containing f in the filters
 * list.  Called with filters locked.
 */
static Iofilter **
findfilter(Iofilter *f)
{
	Iofilter **fp;

	for (fp = &filters.f; *fp && *fp != f; fp = &(*fp)->next)
		;
	return fp;
}

void
addfilter(Iofilter *f)
{
	Iofilter **fp;

	zfilter(f);
	qlock(&filters);
	fp = findfilter(f);
	if (*fp == nil) {
		f->next = filters.f;
		filters.f = f;
	}
	qunlock(&filters);
}

void
delfilter(Iofilter *f)
{
	Iofilter **fp;

	if (f == nil) 
		return;
	qlock(&filters);
	fp = findfilter(f);
	if (*fp != nil)
		*fp = f->next;	
	qunlock(&filters);
}

void
incfilter(Iofilter *f, uvlong bytes, uvlong ms)
{
	qlock(f);
	f->bytes += bytes;
	if (f->lmax == 0 || ms > f->lmax)
		f->lmax = ms;
	f->lsum += ms;
	f->nlat++;
	if (bytes == 512)
		f->io512++;
	else if (bytes <= 1024)
		f->io1024++;
	else if (bytes <= 2048)
		f->io2048++;
	else if (bytes <= 4096)
		f->io4096++;
	else if (bytes <= 8192)
		f->io8192++;
	else if (bytes > 8192)
		f->iog8192++;

	qunlock(f);
}

int
filtersum(Iofilter *f, uvlong *bps, uvlong *lat, uvlong *iops, int ns)
{
	uvlong b, lavg;
	uvlong lmax,*slat;
	uvlong io512, io1024, io2048, io4096, io8192, iog8192;
	uint i, n, s;

	if (bps)
		*bps = 0;
	if (lat) {
		lat[Lmax] = 0;
		lat[Lavg] = 0;
	}

	if (iops) {
		iops[Io512] = 0;
		iops[Io1024] = 0;
		iops[Io2048] = 0;
		iops[Io4096] = 0;
		iops[Io8192] = 0;
		iops[Iog8192] = 0;
	}
	b = 0;
	lavg = 0;
	lmax = 0;
	io512 = 0;
	io1024 = 0;
	io2048 = 0;
	io4096 = 0;
	io8192 = 0;
	iog8192 = 0;
	qlock(f);
	if (f->nsamples == 0) {
		qunlock(f);
		return 0;
	}
	if (ns <= 0 || ns > Niosamples)
		ns = Niosamples;
	if (ns > f->nsamples)
		ns = f->nsamples;
	i = f->nsamples - 1;
	n = i - ns;
	for (; i != n; i--) {
		s = i % Niosamples;
		b += f->samples[s].b;
		slat = f->samples[s].lat;
		lavg += slat[Lavg];
		if (lmax < slat[Lmax])
			lmax = slat[Lmax];
		slat = f->samples[s].iops;
		io512 += slat[Io512];
		io1024 += slat[Io1024];
		io2048 += slat[Io2048];
		io4096 += slat[Io4096];
		io8192 += slat[Io8192];
		iog8192 += slat[Iog8192];
	}

	qunlock(f);

	if (bps)
		*bps = b;
	if (lat) {
		lat[Lavg] = lavg;
		lat[Lmax] = lmax;
	}

	if (iops) {
		iops[Io512] = io512;
		iops[Io1024] = io1024;
		iops[Io2048] = io2048;
		iops[Io4096] = io4096;
		iops[Io8192] = io8192;
		iops[Iog8192] = iog8192;
	}
	return ns;
}

void
filtertimer(void)
{
	Iofilter *f;
	uint s;

	for (;; sleep(1000)) {
		if (shutdown) {
			xlexits(0);
		}
		qlock(&filters);
		for (f = filters.f; f; f = f->next) {
			qlock(f);
			s = f->nsamples++ % Niosamples;
			f->samples[s].b = f->bytes;
			f->samples[s].lat[Lmax] = f->lmax;
			f->samples[s].lat[Lavg] = f->nlat ? f->lsum / f->nlat : 0;
			f->samples[s].iops[Io512] = f->io512;
			f->samples[s].iops[Io1024] = f->io1024;
			f->samples[s].iops[Io2048] = f->io2048;
			f->samples[s].iops[Io4096] = f->io4096;
			f->samples[s].iops[Io8192] = f->io8192;
			f->samples[s].iops[Iog8192] = f->iog8192;
			f->bytes = 0;
			f->lmax = 0;
			f->lsum = 0;
			f->nlat = 0;
			f->io512 = 0;
			f->io1024 = 0;
			f->io2048 = 0;
			f->io4096 = 0;
			f->io8192 = 0;
			f->iog8192 = 0;
			qunlock(f);
		}

		qunlock(&filters);
	}
}

int
iostatfmt(Fmt *fmt)
{
	uvlong b;
	uvlong lat[Lsz];
	Iofilter *f;
	int n = fmt->width;

	f = va_arg(fmt->args, Iofilter *);
	if ((n = filtersum(f, &b, lat, nil, n)) <= 0) 
		return 0;

	return fmtprint(fmt, "%d:%ulld/%ulld/%ulld",
		n, b/n, lat[Lavg]/n, lat[Lmax]); 
}

int
iopsfmt(Fmt *fmt)
{
	uvlong iops[Iosz];
	Iofilter *f;
	int n = fmt->width;

	f = va_arg(fmt->args, Iofilter *);
	if ((n = filtersum(f, nil, nil, iops, n)) <= 0) 
		return 0;

	return fmtprint(fmt, "%d:%ulld/%ulld/%ulld/%ulld/%ulld/%ulld",
		n,iops[Io512]/n, iops[Io1024]/n, iops[Io2048]/n, 
		iops[Io4096]/n, iops[Io8192]/n, iops[Iog8192]/n);
}
