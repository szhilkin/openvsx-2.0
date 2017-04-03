#include <u.h>
#include <libc.h>
#include "dat.h"
#include "fns.h"

char *
uerr(char *fmt, ...)
{
	va_list arg;

	assert(u);
	va_start(arg, fmt);
	vsnprint(u->errstr, nelem(u->errstr), fmt, arg);
	va_end(arg);
	werrstr(u->errstr);
	return u->err = u->errstr;
}

void
error(char *fmt, ...)
{
	va_list arg;

	assert(u);
	va_start(arg, fmt);
	vsnprint(u->errstr, nelem(u->errstr), fmt, arg);
	va_end(arg);
	werrstr(u->errstr);
	u->err = u->errstr;

	assert(u->nerrlab < NERR);
	nexterror();
}

void
nexterror(void)
{
	assert(u->nerrlab > 0);
	longjmp(u->errlab[--u->nerrlab], 1);
}

static void
vsyslog(char *fmt, va_list arg)
{
	static int fd = -1;

	if (fd < 0)
		fd = open("/dev/syslog", OWRITE);

	if (vfprint(fd, fmt, arg) <= 0)
		vfprint(1, fmt, arg);	// at least try to be visible
}

void
xsyslog(char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	vsyslog(fmt, arg);
	va_end(arg);
}

void
halog(char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	vsyslog(fmt, arg);
	va_end(arg);
}

void
setprompt(void)
{
	int fd;

	fd = create("#ec/cliprompt", OWRITE, 0666);
	if (fd < 0) {
		xsyslog("Warning: cannot set prompt\n");
		return;
	}
	if (shelf == -1)
		fprint(fd, "VSX shelf unset> ");
	else
		fprint(fd, "VSX shelf %d> ", shelf);
	close(fd);

	fd = create("#ec/shelf", OWRITE, 0666);
	if (fd < 0) {
		xsyslog("Warning: cannot store shelf as env variable\n");
		return;
	}
	if (shelf == -1)
		fprint(fd, "unset");
	else
		fprint(fd, "%d", shelf);
	close(fd);
}

void
system(char *cmd)
{
	switch (rfork(RFCFDG|RFREND|RFPROC)) {
	case -1:
		xsyslog("system(%s) fork failed: %r\n", cmd);
		break;
	case 0:
		execl("/bin/rc", "rc", "-c", cmd, nil);
		xsyslog("system(%s) execl failed: %r\n", cmd);
		exits(nil);
	default:
		waitpid();
	}
}

int
readfile(char *buf, int len, char *fmt, ...)
{
	int fd, n;
	va_list arg;

	va_start(arg, fmt);
	n = vsnprint(buf, len, fmt, arg);
	va_end(arg);
	if (n < 0) {
		werrstr("vsnprint failed %r");
		return -1;
	}
	fd = open(buf, OREAD);
	if (fd < 0) {
		return -1;
	}
	n = read(fd, buf, len);
	close(fd);
	return n;
}

/*
  One level alarm nesting. More levels needs something more sophisticated.

	long old = alarmset(3*1000);
	read(...);
	alarmclr(old);
*/

long
alarmset(ulong ms)
{
	alarmflag = 1;
	return alarm(ms);
}

void
alarmclr(long ms)
{
	if (ms > 0)
		alarm(ms);
	else
		alarm(0);
	alarmflag = 0;
}
