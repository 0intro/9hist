#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "ip.h"
#include "kernel.h"


static void	pktbind(Ipifc *ifc, int argc, char **argv);
static void	pktunbind(Ipifc *ifc);
static void	pktbwrite(Ipifc *ifc, Block *bp, int version, uchar *ip);
static void	pktin(Fs*, Ipifc *ifc, Block *bp);

Medium pktmedium =
{
	"pkt",
	14,
	40,
	4*1024,
	6,
	pktbind,
	pktunbind,
	pktbwrite,
	nil,		/* addmulti */
	nil,		/* remmulti */
	pktin,
	nil,		/* addroute */
	nil,		/* remroute */
	nil,		/* flushroute */
	nil,		/* joinmulti */
	nil,		/* leave multi */
	nil,		/* ares */
	1,		/* unbind on last close */
};

/*
 *  called to bind an IP ifc to an ethernet device
 *  called with ifc wlock'd
 */
static void
pktbind(Ipifc*, int, char**)
{
}

/*
 *  called with ifc wlock'd
 */
static void
pktunbind(Ipifc*)
{
}

/*
 *  called by ipoput with a single packet to write
 */
static void
pktbwrite(Ipifc *ifc, Block *bp, int, uchar*)
{
	/* enqueue onto the conversation's rq */
	bp = concatblock(bp);
	qpass(ifc->conv->rq, bp);
}

/*
 *  called with ifc rlocked when someone write's to 'data'
 */
static void
pktin(Fs *f, Ipifc *ifc, Block *bp)
{
	if(ifc->lifc == nil)
		freeb(bp);
	else
		ipiput(f, ifc->lifc->local, bp);
}

void
pktmediumlink(void)
{
	addipmedium(&pktmedium);
}
