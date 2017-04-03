#include <u.h>
#include <libc.h>
#include <bio.h>
#include <authsrv.h>
#include <libsec.h>
#include <ip.h>
#include "dat.h"

/*
 * ha server.
 * primary runs as a server that allows the secondary
 * to log in and control it.
 */

int	login(char *);
int	getcrnl(char *, int);
void	sendcrnl(char *, ...);
int	mkpwhash(uchar *, int, char *, NetConnInfo*);

Biobuf	in;
Biobuf	out;

int	debug;

void
usage(void)
{
	fprint(2, "usage: %s primary\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	int fd, n;
	char line[Nline];
	
	fmtinstall('H', encodefmt);
	fmtinstall('E', eipfmt);
	ARGBEGIN{
	case 'd':
		debug++;
		break;
	default:
		usage();
	} ARGEND
	if (argc != 1)
		usage();
	fd = login(*argv);
	if (fd == -1)
		exits("no login");
	print("connected\n");
	while ((n = read(0, line, sizeof line)) > 0) {
		line[n-1] = 0;
		sendcrnl("%s", line);
		do {
			n = getcrnl(line, sizeof line);
			if (n <= 0)
				exits("eof");
			print("%s\n", line);
			if (strncmp(line, "enter ", 6) == 0)
				while ((n = read(0, line, sizeof line)) > 0) {
					line[n-1] = 0;
					sendcrnl("%s", line);
					if (line[0] == '.' && line[1] == 0)
						break;
				}
			
		} while (line[0] != '-' && line[0] != '+');
	}
}

int
login(char *s)		/* dial and login to a primary */
{
	char buf[32], line[Nline], *p, *q, cdir[40];
	uchar hash[32];
	int fd, cfd;
	NetConnInfo *ni;
	
	snprint(buf, sizeof buf, "%s!%s!"HAport, strncmp("5100", s, 4) ? "tcp" : "el", s);
	fd = dial(buf, nil, cdir, &cfd);
/**/	print("dial(%s) => fd=%d cfd=%d cdir=%s\n", buf, fd, cfd, cdir);
	if (fd == -1)
		return -1;
	ni = getnetconninfo(cdir, cfd);
	if (ni == nil)
		sysfatal("getnetconninfo: %r");
	// more stuff here
	Binit(&in, fd, OREAD);
	Binit(&out, fd, OWRITE);
	
	getcrnl(line, sizeof line);
/**/	print("line=%s\n", line);
	if (strncmp(line, "+OK HA", 6) != 0) {
		print("%s\n", line);
		freenetconninfo(ni);
		exits("no hello");
	}
	if ((p = strchr(line, '<')) && (q = strchr(p+1, '>'))) {
		*q = 0;
		if (mkpwhash(hash, sizeof hash, p+1, ni) == -1)
			return -1;
		sendcrnl("login %s %.*H", getuser(), sizeof hash, hash);
		getcrnl(line, sizeof line);
		freenetconninfo(ni);
		if (line[0] != '+')
			return -1;
		return fd;
	} 
	freenetconninfo(ni);
	return -1;
}

int
getcrnl(char *buf, int n)
{
	int c;
	char *ep;
	char *bp;
	Biobuf *fp = &in;

	Bflush(&out);

	bp = buf;
	ep = bp + n - 1;
	while(bp != ep){
		c = Bgetc(fp);
		switch(c){
		case -1:
			*bp = 0;
			if(bp==buf)
				return 0;
			else
				return bp-buf;
		case '\r':
			c = Bgetc(fp);
			if(c == '\n'){
				*bp = 0;
				return bp-buf;
			}
			Bungetc(fp);
			c = '\r';
			break;
		case '\n':
			*bp = 0;
			if (debug)
				fprint(2, "<- %s\n", buf);
			return bp-buf;
		}
		*bp++ = c;
	}
	*bp = 0;
	if (debug)
		fprint(2, "<- %s\n", buf);
	return bp-buf;
}

void
sendcrnl(char *fmt, ...)
{
	char buf[Nline];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf+sizeof(buf), fmt, arg);
	va_end(arg);
	if(debug)
		fprint(2, "-> %s\n", buf);
	Bprint(&out, "%s\r\n", buf);
}

int
mkpwhash(uchar *out, int lim, char *chal, NetConnInfo *ni)
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
		return -1;
	}
//**/	print("%s\n", nv->config);
//**/	print("%s\n", ni->laddr);
//**/	print("%s\n", ni->raddr);
//**/	print("chal=%s\n", chal);
	n = dec16(buf, sizeof buf, chal, strlen(chal));
	s = sha2_256((uchar *)nv->config, strlen(nv->config), nil, nil);
	s = sha2_256((uchar *)ni->laddr, strlen(ni->laddr), nil, s);
	s = sha2_256((uchar *)ni->raddr, strlen(ni->raddr), nil, s);
	sha2_256(buf, n, out, s);
	free(nv);
	return SHA2_256dlen;
}
