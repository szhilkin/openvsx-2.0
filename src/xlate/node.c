#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <bio.h>
#include "dat.h"
#include "shadow.h"

Node *
nodeget(char *ipaddr)
{
	Biobuf *b;
	char *s, *f[4];
	Node *n;
	int bytes, cmp;

	if (!(b = Bopen("/n/remote/security", OREAD)))
		return nil;

	for (n = 0; s = Brdstr(b, '\n', 0); free(s)) {
		if (getfields(s, f, nelem(f), 1, " \n") < 3
		    || (cmp = strcmp(f[0], ipaddr)) > 0) {
			break;
		} else if (cmp == 0) {
			if ((n = mallocz(sizeof *n, 1))
			    && (n->ipaddr = strdup(ipaddr))) {
				if (strcmp(f[1], "encrypt") == 0)
					n->flags |= Nencrypt;

				bytes = dec16(n->certhash, sizeof n->certhash,
					      f[2], strlen(f[2]));
				if (bytes != sizeof n->certhash) {
					werrstr("certhash bytes %d not %d",
						bytes, sizeof n->certhash);
					nodefree(n);
					n = 0;
				}
			} else {
				nodefree(n);
				n = 0;
			}
			break;
		}
	}
	free(s);
	Bterm(b);
	return n;
}

void
nodefree(Node *n)
{
	if (n) {
		free(n->ipaddr);
		free(n);
	}
}
