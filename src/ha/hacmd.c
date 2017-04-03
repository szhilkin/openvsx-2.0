#include <u.h>
#include <libc.h>

void
usage(void)
{
	fprint(2, "usage: hacmd [-n server] commands\n");
	exits("usage");
}

void
main(int argc, char *argv[])
{
	char *name, buf[4*1024], *p, *q;
	int fd, n, i, errs;

	name = 0;
	ARGBEGIN{
	case 'n':
		name = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND

	if(name)
		snprint(buf, sizeof buf, "/srv/ha.%s.cmd", name);
	else
		strcpy(buf, "/srv/ha.cmd");
	fd = open(buf, ORDWR);
	if(fd < 0){
		fprint(2, "%s: can't open commands file\n", argv0);
		exits("commands file");
	}

	errs = 0;
	for(i = 0; i < argc; i++){
		if(write(fd, argv[i], strlen(argv[i])) != strlen(argv[i])){
			fprint(2, "%s: error writing %s: %r", argv0, argv[i]);
			errs++;
			continue;
		}
		for(;;){
			n = read(fd, buf, sizeof buf - 1);
			if(n < 0){
				fprint(2, "%s: error executing %s: %r", argv0, argv[i]);
				errs++;
				break;
			}
			buf[n] = '\0';
			for (p = buf; p; p = q) {
				q = strstr(p, "\r\n");
				if (q) {
					*q++ = '\n';
					*q++ = 0;
				}
				print("%s", p);
			}
			break;
		}
	}
	exits(errs ? "errors" : 0);		
}
