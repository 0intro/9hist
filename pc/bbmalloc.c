#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

/*
 * Allocate memory for use in kernel bitblts.
 * The allocated memory must have a flushed instruction
 * cache, and the data cache must be flushed by bbdflush().
 * To avoid the need for frequent cache flushes, the memory
 * is allocated out of an arena, and the i-cache is only
 * flushed when it has to be reused.  By returning an
 * address in non-cached space, the need for flushing the
 * d-cache is avoided.
 *
 * Currently, the only kernel users of bitblt are devbit,
 * print, and the cursor stuff in devbit.  The cursor
 * can get drawn at clock interrupt time, so it might need
 * to bbmalloc while another bitblt is going on.
 *
 * This code will have to be interlocked if we ever get
 * a multiprocessor with a bitmapped display.
 */

/* a 0->3 bitblt can take 900 words */
enum {
	nbbarena=8192	/* number of words in an arena */
};

static ulong	bbarena[nbbarena];
static ulong	*bbcur = bbarena;
static ulong	*bblast = 0;

void *
bbmalloc(int nbytes)
{
	int nw;
	int s;
	ulong *ans;

	nw = nbytes/sizeof(long);
	s = splhi();
	if(bbcur + nw > &bbarena[nbbarena])
		ans = bbarena;
	else
		ans = bbcur;
	bbcur = ans + nw;
	splx(s);
/*
	if(ans == bbarena)
		icflush(ans, sizeof(bbarena));
*/
	bblast = ans;
	ans = (void *)ans;
	return ans;
}

void
bbfree(void *p, int n)
{
	ulong *up;

	if(p == bblast)
		bbcur = (ulong *)(((char *)bblast) + n);
}

void *
bbdflush(void *p, int n)
{
	return p;
}
