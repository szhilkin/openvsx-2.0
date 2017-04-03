#include <u.h>
#include <libc.h>
#include "haconfig.h"

enum {
	Nline = 8192,
};

static char *procname;

static char *
pname(void)
{
	int fd, n;
	char buf[64], *p;

	if (procname)
		return procname;
	snprint(buf, sizeof buf, "/proc/%d/args", getpid());
	fd = open(buf, OREAD);
	if (fd < 0)
		return "?";
	n = read(fd, buf, sizeof buf);
	if (n < 0)
		return "?";
	buf[n] = 0;
	p = strpbrk(buf, " \t\r\n");
	if (p)
		*p = 0;
	return procname = strdup(buf);
}

static int
iserr(char *e)
{
	char buf[ERRMAX];

	buf[0] = 0;
	rerrstr(buf, ERRMAX);
	if (strstr(buf, e))
		return 1;
	return 0;
}

static int
openlocked(char *file, int omode)
{
	int fd;

	fd = -1;
	while (fd < 0) {
		fd = open(file, omode);
		if (fd >= 0)
			break;
		if (iserr("locked") == 0)
			break;
		sleep(100);
	}
	return fd;
}

static void
syncmtime(int fd, int fd2)
{
	Dir d, *d2;

	nulldir(&d);
	d2 = dirfstat(fd2);
	if (d2) {
		d.mtime = d2->mtime;
		dirfwstat(fd, &d);
	} else
		halog("%s: cannot sync mtime on config files: %r\n", pname());
	free(d2);
}

int
haopenconfig(char *cfile)
{
	int fd, fd2, i;
	char buf2[Nline], *bkup;

	bkup = smprint("%s.old", cfile);
	fd2 = openlocked(bkup, OREAD);
	if (fd2 >= 0) {
		halog("%s: found backup from incomplete configuration update\n", pname());
		fd = openlocked(cfile, ORDWR|OTRUNC);
		if (fd < 0) {
			close(fd2);
			halog("%s: failed to open configuration file for fallback: %r", pname());
			goto e;
		}
		while ((i = read(fd2, buf2, sizeof buf2)) > 0)
			if (write(fd, buf2, i) < 0)
				break;
		if (i != 0) {
			close(fd2);
			close(fd);
			halog("%s: failed to revert to backup configuration: %r", pname());
			fd = -1;
			goto e;
		}
		syncmtime(fd, fd2);
		remove(bkup);
		close(fd2);
		seek(fd, 0, 0);
	}
	else {
		fd = openlocked(cfile, OREAD);
		if (fd < 0) {
			if (iserr("exist") == 0)
				halog("%s: failed to open configuration file for loading: %r", pname());
		}
	}
e:	free(bkup);
	return fd;
}

/* Return the order from youngest to oldest by modification*/
static int
dirmtimecmp(void *v1, void *v2) {
	Dir *d1, *d2;

	d1 = v1;
	d2 = v2;
	/* I don't want to subtract here 'cause mtime is a ulong
	    There is a remote possibility of sign extension weirdness */
	if (d2->mtime > d1->mtime)
		return 1;
	if (d2->mtime < d1->mtime)
		return -1;
	return 0;
}

/* Prune down to 31 epoch files */
static void
pruneepoch(char *file)
{
	char buf[8192];
	char del[8192];
	char *fname, *e;
	Dir *d;
	int flen, n, fd, i, j;
	
	strncpy(buf, file, sizeof buf);
	e = strrchr(buf, '.');
	if (e == nil) 
		return;
	*e = 0;
	fname = strrchr(buf, '/');
	if (fname == nil)
		return;
	*fname = 0;
	fname++; 
	flen = e - fname;
	if ((fd = open(buf, OREAD)) < 0) {
		print("prune open failed %s: %r\n", buf);
		return;
	}
	n = dirreadall(fd, &d);
	if (n  >= 0) {
		qsort(d, n, sizeof *d, dirmtimecmp);
		j = 0;
		for (i = 0; i < n; i++) {
			if (strncmp(d[i].name, fname, flen) == 0) {
				/* don't consider rr, xlate, or *.old */
				snprint(del, sizeof del, "%s.old", fname);
				if (strcmp(d[i].name, fname) == 0 || 
				    strcmp(d[i].name, del) == 0)
					continue;
				if (++j >= 32) {
					snprint(del, sizeof del, "%s/%s", buf, d[i].name);
					if (remove(del))
						halog("%s: %r\n", del);
				}
					
			}
		}	
	} else 
		halog("dirread fail %s: %r\n", buf);
	free(d);
	close(fd);
}

/* paranoia, move a *.old file to a *.epoch file */
static int
backupold(char *file)
{
	char buf[8192];
	char *p;
	Dir *d;

	strncpy(buf, file, sizeof buf);
	p = strrchr(buf, '.');
	if (p == nil) {
		print("hadaemon backup error, file %s incorrect\n", buf);
		return -1;
	}
	pruneepoch(file);
	for (;;) {
		snprint(p, sizeof buf - (p - buf), ".%lld", nsec());
		d = dirstat(buf);
		if (d == nil)
			break;
		free(d);	// file exists
		sleep(1);
	}
	d = dirstat(file);
	if (d == nil) {
		print("hadaemon backup state invalid\n");
		return -1;
	}
	p = strrchr(buf, '/');
	if (p == nil)
		p = buf;
	else
		p++;
	d->name = p;
	dirwstat(file, d);
	free(d);
	return 0;
}

int
hawriteconfig(char *cfile, ulong mtime, char *buf, int len, int force)
{
	Dir d2, *d;
	int fd, fdp, i;
	char *bkup;
	ulong told = 0;
	char buf2[Nline];

e:	for (;;) {
		fd = openlocked(cfile, OREAD);
		if (fd >= 0)
			break;
		fd = create(cfile, OREAD|OEXCL, DMEXCL | 0666);
		if (fd >= 0)
			break;
		if (iserr("exist") == 0) {
			halog("%s: failure opening config file %s: %r\n", pname(), cfile);
			goto e;
		}
	}

	d = nil;
	/* Don't bother writing nothing to an empty file */
	if (force == 0 && len == 0 && (d = dirfstat(fd)) && d->length == 0) {
		free(d);
		close(fd);
		return 0;
	}			
	free(d);
	if (mtime == -1)
	{
		told = 0;
		d = dirfstat(fd);
		if (d) {
			told = d->mtime;
			free(d);
		}
	}			
	/* Make sure we don't lose it if we crash and that time never goes backward */
	bkup = smprint("%s.old", cfile);			
	fdp = create(bkup, OWRITE, DMEXCL | 0666);
	if (fdp < 0) {
		close(fd);
		halog("%s: failure opening backup file %s: %r\n", pname(), bkup);
		goto e;
	}
	while ((i = read(fd, buf2, sizeof buf2)) > 0)
		if (write(fdp, buf2, i) < 0)
			break;
	if (i != 0) {
		close(fd);
		remove(bkup);
		close(fdp);
		halog("%s: failed to make backup copy of config file: %r\n", pname());
		goto e;
	}
	syncmtime(fdp, fd);
	close(fd);
	fd = openlocked(cfile, OWRITE|OTRUNC);
	if (fd < 0) {
		remove(bkup);
		close(fdp);
		halog("%s: failed to reopen config file for writing: %r\n", pname());
		goto e;
	}
	if (write(fd, buf, len) < 0) {
		/* this is odd.  Take a stab at restoring the config we just backed up */
		halog("%s: failure to write config file %s: %r\n", pname(), cfile);
		close(fd);
		close(fdp);			/* close bkup so haopenconfig can reopen */
		fd = haopenconfig(cfile);	/* replaces conf file with .old */
		if (fd < 0)
			halog("%s: failure to restore config file after failed update: %r\n", pname());
		else
			close(fd);
		remove(bkup);
		goto e;
	}
	d = dirfstat(fd);
	if (d != nil) {
		nulldir(&d2);
		if (mtime != -1) {
			d2.mtime = mtime;
			if (dirfwstat(fd, &d2) < 0)
				halog("%s: failure to update config file time %s: %r\n", pname(), cfile);
		}
		else if (told >= d->mtime) {
			d2.mtime = told + 1;
			if (dirfwstat(fd, &d2) < 0)
				halog("%s: failure to update config file time %s: %r\n", pname(), cfile);
		}	
	}
	free(d);
	close(fd);
	if (backupold(bkup) < 0)
		remove(bkup);
	close(fdp);
	free(bkup);
	return 0;
}

