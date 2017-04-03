#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <bio.h>
#include "dat.h"
#include "shadow.h"

Remote *
remoteget(char *name)
{
	Biobuf *b;
	char *s, *f[4];
	Remote *r;
	int n, cmp;

	if (!(b = Bopen("/n/remote/name", OREAD)))
		return nil;

	for (r = 0; s = Brdstr(b, '\n', 0); free(s)) {
		if ((n = getfields(s, f, nelem(f), 1, " \n")) < 2
		    || (cmp = strcmp(f[0], name)) > 0) {
			break;
		} else if (cmp == 0) {
			if ((r = mallocz(sizeof *r, 1))
			    && (r->name = strdup(name))
			    && (r->n[0] = nodeget(f[1]))
			    && (n == 2 || (r->n[1] = nodeget(f[2])))) {
				// null
			} else {
				remotefree(r);
				r = 0;
			}
			break;
		}
	}
	free(s);
	Bterm(b);
	return r;
}

Remote *
remotegetip(char *ipaddr)
{
	Biobuf *b;
	char *s, *f[4];
	Remote *r;
	int n;

	if (!(b = Bopen("/n/remote/name", OREAD)))
		return nil;

	for (r = 0; s = Brdstr(b, '\n', 0); free(s)) {
		if ((n = getfields(s, f, nelem(f), 1, " \n")) < 2) {
			break;
		} else if (strcmp(f[1], ipaddr) == 0
			   || (n == 3 && strcmp(f[2], ipaddr) == 0)) {
			if ((r = mallocz(sizeof *r, 1))
			    && (r->name = strdup(f[0]))
			    && (r->n[0] = nodeget(f[1]))
			    && (n == 2 || (r->n[1] = nodeget(f[2])))) {
				// null
			} else {
				remotefree(r);
				r = 0;
			}
			break;
		}
	}
	free(s);
	Bterm(b);
	return r;
}

Node *
remotenode(Remote *r, char *ipaddr)
{
	if (strcmp(r->n[0]->ipaddr, ipaddr) == 0)
		return r->n[0];
	if (r->n[1] && strcmp(r->n[1]->ipaddr, ipaddr) == 0)
		return r->n[1];
	return 0;
}

void
remotefree(Remote *r)
{
	if (r) {
		free(r->name);
		nodefree(r->n[0]);
		nodefree(r->n[1]);
		free(r);
	}
}
