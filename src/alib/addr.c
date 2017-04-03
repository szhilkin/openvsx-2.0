#include <u.h>
#include <libc.h>
#include <ctype.h>
#include "alib.h"

int
parseether(uchar *to, char *from)
{
	char nip[3];
	char *p;
	int i;

	if (to == nil || from == nil || strlen(from) != 12)
		return -1; 
	for (i = 0; i < 12; i++)
		if (!isxdigit(from[i]))
			return -1;
	p = from;
	for(i = 0; i < 6; i++){
		nip[0] = *p++;
		nip[1] = *p++;
		nip[2] = 0;
		to[i] = strtoul(nip, 0, 16);
	}
	return 0;
}

int
myetheraddr(uchar *to, char *dev)
{
	int n, fd;
	char buf[256];

	if(*dev == '/')
		sprint(buf, "%s/addr", dev);
	else
		sprint(buf, "/net/%s/addr", dev);

	fd = open(buf, OREAD);
	if(fd < 0)
		return -1;

	n = read(fd, buf, sizeof buf -1 );
	close(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;

	return parseether(to, buf);
}

