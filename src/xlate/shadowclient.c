#include <u.h>
#include <libc.h>
#include <bio.h>
#include <libsec.h>
#include "dat.h"
#include "fns.h"
#include "shadow.h"

static int
connto(Node *n, char *cert, int repfd)
{
	char *dst;
	TLSconn conn;
	int fd;
	uchar digest[SHA1dlen];
	long old;

	fd = -1;
	memset(&conn, 0, sizeof conn);

	if (!(dst = smprint("tcp!%s!%d", n->ipaddr, shadowport)))
		goto error;
	reporter(repfd, "connecting to %s\n", dst);

	old = alarmset(5*1000);
	if ((fd = dial(dst, 0, 0, 0)) < 0) {
		alarmclr(old);
		reporter(repfd, "dial %r\n");
		goto error;
	}
	alarmclr(old);

	if (n->flags & Nencrypt)
		conn.ciphersmask = Ciphersmaskencrypt;
	else
		conn.ciphersmask = Ciphersmasknull;

	conn.dbfd = repfd;

	if (!(conn.cert = readcert(cert, &conn.certlen))) {
		reporter(repfd, "can't read certificate %s", cert);
		goto error;
	}
	old = alarmset(10*1000);
	if ((fd = tlsClient(fd, &conn)) < 0) {
		alarmclr(old);
		reporter(repfd, "tlsClient: %r");
		goto error;
	}
	alarmclr(old);
	if (!conn.cert || conn.certlen <= 0) {
		reperrstr(repfd, "no TLS certificate from %s", n->ipaddr);
		goto error;
	}
	sha1(conn.cert, conn.certlen, digest, nil);
	if (memcmp(digest, n->certhash, sizeof digest) != 0) {
		fmtinstall('H', encodefmt);
		reperrstr(repfd, "bad server certificate hash %.*lH",
			  SHA1dlen, digest);
		goto error;
	}
	reporter(repfd, "%s OK fd is %d", n->ipaddr, fd);
	free(conn.cert);
	free(conn.sessionID);
	free(dst);
	return fd;
error:
	reporter(repfd, "%s error fd is %d", n->ipaddr, fd);
	free(conn.cert);
	free(conn.sessionID);
	free(dst);
	if (fd >= 0)
		close(fd);
	return -1;
}

int
shadowclient(char *remote, char *cert, int debug)
{
	Remote *r;
	char *dfile;
	int fd, repfd;
	char err[ERRMAX];

	if (!remote) {		// check for shadow.c:sendproc close case
		return -1;
	}
	dfile = 0;
	repfd = 0;

	if (!(r = remoteget(remote))) {
		return -1;
	}
	if (debug) {
		dfile = smprint("/tmp/shadowclient.%s", remote);
		repfd = create(dfile, OWRITE|OTRUNC, 0644);
	}
	if ((fd = connto(r->n[0], cert, repfd)) < 0
	    && (!r->n[1] || (fd = connto(r->n[1], cert, repfd)) < 0)) {
		rerrstr(err, sizeof err);
		if (strcmp(err, "interrupted") == 0)
			werrstr("connection timed out");
		else
			werrstr("connection failed %r");
	}
	if (debug) {
		free(dfile);
		close(repfd);
	}
	remotefree(r);
	return fd;
}
