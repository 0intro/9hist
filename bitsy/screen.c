#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

void
flushmemscreen(Rectangle)
{
}

uchar*
attachscreen(Rectangle*, ulong*, int*, int*, int*)
{
	return nil;
}

void
getcolor(ulong p, ulong* pr, ulong* pg, ulong* pb)
{
	USED(p, pr, pg, pb);
}

int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	USED(p,r,g,b);
	return 0;
}

void
blankscreen(int blank)
{
	USED(blank);
}

void
screenputs(char *s, int n)
{
	USED(s, n);
}
