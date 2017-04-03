#include <u.h>
#include <libc.h>
#include <bio.h>
#include <libsec.h>
#include "dat.h"
#include "fns.h"
#include "shadow.h"

int
shadowserver(char *ldial, char *certfile)
{
	int dfd, acfd, lcfd, pid, repfd;
	char adir[40], ldir[40], *err, *dfile;
	NetConnInfo *cfo;
	Remote *r;
	Node *n;
	ulong ciphersmask;
	TLSconn *tlsc;
	uchar digest[SHA1dlen];

	acfd = announce(ldial, adir);
	if (acfd < 0)
		return -1;

	while ((lcfd = listen(adir, ldir)) >= 0) {
		r = nil;
		tlsc = nil;
		pid = -1;
		dfd = repfd = -1;
		if (!(cfo = getnetconninfo(ldir, 0))) {
			err = "shadowserver error #1\n";
			xsyslog(err);
			reject(lcfd, ldir, err);
			goto x;
		}
		if (!(r = remotegetip(cfo->rsys))
		    || !(n = remotenode(r, cfo->rsys))) {
			err = "shadowserver no security information for %s\n";
			xsyslog(err, cfo->rsys);
			reject(lcfd, ldir, err);
			goto x;
		}
		if (!shadowrecvlv(r->name, nil)) {
			err = "shadowserver no LV for %s %s\n";
			xsyslog(err, r->name, cfo->rsys);
			reject(lcfd, ldir, err);
			goto x;
		}
		if (n->flags & Nencrypt)
			ciphersmask = Ciphersmaskencrypt;
		else
			ciphersmask = Ciphersmasknull;

		if (!(tlsc = (TLSconn*)mallocz(sizeof *tlsc, 1))) {
			err = "shadowserver error #2\n";
			xsyslog(err);
			reject(lcfd, ldir, err);
			goto x;
		}
		if (!(tlsc->cert = readcert(certfile, &tlsc->certlen))) {
			err = "shadowserver error #3\n";
			xsyslog(err);
			reject(lcfd, ldir, err);
			goto x;
		}
		pid = xlrfork(RFPROC|RFMEM|RFNOWAIT|RFFDG,
			      "accept %s", r->name);

		switch (pid) {
		case -1:
			err = "shadowserver error #4\n";
			xsyslog(err);
			reject(lcfd, ldir, err);
			goto x;
		case 0:
			break;		// child
		default:
			close(lcfd);
			continue;	// parent
		}
		if ((dfd = accept(lcfd, ldir)) < 0) {
			xsyslog("shadowserver accept %s failed %r\n",
				cfo->raddr);
			goto x;
		}
		tlsc->ciphersmask = ciphersmask;
		tlsc->clicertreq = 1;
		if (shadowserverdebug) {
			dfile = smprint("/tmp/shadowserver.%s", cfo->rsys);
			repfd = create(dfile, OWRITE|OTRUNC, 0644);
			free(dfile);
			tlsc->dbfd = repfd;
		}
		if ((dfd = tlsServer(dfd, tlsc)) < 0) {
			xsyslog("shadowserver handshake with %s failed %r\n",
				n->ipaddr);
			goto x;
		}
		if (!tlsc->cert || tlsc->certlen <= 0) {
			xsyslog("shadowserver no TLS certificate from %s\n",
				n->ipaddr);
			close(dfd);
			dfd = -1;
			goto x;
		}
		sha1(tlsc->cert, tlsc->certlen, digest, nil);
		if (memcmp(digest, n->certhash, sizeof digest) != 0) {
			fmtinstall('H', encodefmt);
			fprint(dfd, "END bad client certificate hash %.*lH\n",
			       SHA1dlen, digest);
			xsyslog("shadowserver bad certificate hash %.*lH from %s\n",
				SHA1dlen, digest, n->ipaddr);
			close(dfd);
			dfd = -1;
			goto x;
		}
	x:
		close(lcfd);
		freenetconninfo(cfo);
		remotefree(r);
		tlsconnfree(tlsc);
		if (pid == 0) {
			if (repfd >= 0)
				close(repfd);

			if (dfd >= 0) {
				shadowrecvhs(dfd);
				xsyslog("shadowrecv failed: %r\n");
				close(dfd);
			}
			xlexits(nil);
		}
	}
	return 0;
}
