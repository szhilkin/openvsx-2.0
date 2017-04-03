#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <authsrv.h>

/*
 * print the response to an ha challange.
 */
 
void
usage(void)
{
	fprint(2, "usage: %s <challange>\n", argv0);
	exits("usage");
}

int
mkpwhash(uchar *out, int lim, char *hash)
{
	uchar buf[256];
	Nvrsafe *nv;
	DigestState *s;
	int n;
	
	if (lim < SHA2_256dlen)
		return -1;
	nv = malloc(sizeof *nv);
	if (readnvram(nv, 0) != 0) {
		free(nv);
		sysfatal("can't read nvram:%r");
	}
	n = dec16(buf, sizeof buf, hash, strlen(hash));
	s = sha2_256((uchar *)nv->config, strlen(nv->config), nil, nil);
	sha2_256(buf, n, out, s);
	free(nv);
	return SHA2_256dlen;
}

void
main(int argc, char **argv)
{
	char *u;
	int n;
	uchar out[256];

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc != 1)
		usage();
	fmtinstall('H', encodefmt);
	u = getenv("user");
	n = mkpwhash(out, sizeof out, *argv);
	print("login %s %.*H\n", u, n, out);
	exits(nil);
}
