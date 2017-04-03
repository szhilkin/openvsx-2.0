#include <u.h>
#include <libc.h>
#include <fcall.h>
#include "dat.h"
#include "fns.h"

/*
 * Generous estimate of number of fields, including terminal nil pointer
 */
static 
int
ncmdfield(char *p, int n)
{
	int white, nwhite;
	char *ep;
	int nf;

	if(p == nil)
		return 1;

	nf = 0;
	ep = p+n;
	white = 1;	/* first text will start field */
	while(p < ep){
		nwhite = (strchr(" \t\r\n", *p++ & 0xFF) != 0);	/* UTF is irrelevant */
		if(white && !nwhite)	/* beginning of field */
			nf++;
		white = nwhite;
	}
	return nf+1;	/* +1 for nil */
}

/*
 *  parse a command written to a device
 */
Cmdbuf*
parsecmd(char *p, int n)
{
	Cmdbuf *cb;
	int nf;
	char *sp;

	nf = ncmdfield(p, n);

	/* allocate Cmdbuf plus string pointers plus copy of string including \0 */
	sp = mallocz(sizeof(*cb) + nf * sizeof(char*) + n + 1, 1);
	if (sp == nil)
		return nil;
	cb = (Cmdbuf*)sp;
	cb->f = (char**)(&cb[1]);
	cb->buf = (char*)(&cb->f[nf]);

	memmove(cb->buf, p, n);

	/* dump new line and null terminate */
	if(n > 0 && cb->buf[n-1] == '\n')
		n--;
	cb->buf[n] = '\0';

	cb->nf = tokenize(cb->buf, cb->f, nf-1);
	cb->f[cb->nf] = nil;

	return cb;
}


/*
 * Look up entry in table
 */
Cmdtab*
lookupcmd(Cmdbuf *cb, Cmdtab *ctab, int nctab)
{
	int i;
	Cmdtab *ct;

	if(cb->nf == 0){
		werrstr("empty control message");
		return nil;
	}

	for(ct = ctab, i=0; i<nctab; i++, ct++){
		if(strcmp(ct->cmd, "*") !=0)	/* wildcard always matches */
		if(strcmp(ct->cmd, cb->f[0]) != 0)
			continue;
		if(ct->narg != 0 && ct->narg != cb->nf){
			werrstr("bad # args to command");
			return nil;
		}
		return ct;
	}

	werrstr("unknown control message %s", cb->f[0]);
	return nil;
}

int
readnum(ulong off, char *buf, ulong n, ulong val, int size)
{
	char tmp[64];

	snprint(tmp, sizeof(tmp), "%*lud", size-1, val);
	tmp[size-1] = ' ';
	if(off >= size)
		return 0;
	if(off+n > size)
		n = size-off;
	memmove(buf, tmp+off, n);
	return n;
}

int
readstr(ulong off, char *buf, ulong n, char *str)
{
	int size;

	size = strlen(str);
	if(off >= size)
		return 0;
	if(off+n > size)
		n = size-off;
	memmove(buf, str+off, n);
	return n;
}

