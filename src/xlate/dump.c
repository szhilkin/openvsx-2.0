// Copyright © 2010 Coraid, Inc.
// All rights reserved.

#include "u.h"
#include "libc.h"
#include "bio.h"

void
dump(void *ap, int len)
{
	uchar *p = ap;
	int i, j;
	static Biobuf *bp, b;

	if (bp == nil) {
		bp = &b;
		Binit(bp, 1, OWRITE);
	}
	while (len > 0) {
		Bprint(bp, "%p: ", p);
		j = len < 16 ? len : 16;
		for (i = 0; i < j; i++) 
			Bprint(bp, "%2.2x ", p[i]);
		while (i < 16) {
			Bprint(bp, "   ");
			i++;
		}
		Bprint(bp, " *");
		for (i = 0; i < j; i++)
			if (' ' <= p[i] && p[i] <= '~')
				Bprint(bp, "%c", (char)p[i]);
			else
				Bprint(bp, ".");
		while (i < 16) {
			Bprint(bp, " ");
			i++;
		}
		Bprint(bp, "*");
		Bprint(bp, "\n");
		len -= j;
		p += j;
	}
	Bflush(bp);
}

