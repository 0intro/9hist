#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "ip.h"
#include "kernel.h"

static void
nullbind(Ipifc*, int, char**)
{
	error("can't bind null device");
}

static void
nullunbind(Ipifc*)
{
}

static void
nullbwrite(Ipifc*, Block*, int, uchar*)
{
	error("nullbwrite");
}

Medium nullmedium =
{
	"null",
	0,		/* medium header size */
	0,		/* default min mtu */
	0,		/* default max mtu */
	0,		/* mac address length  */
	nullbind,
	nullunbind,
	nullbwrite,
	nil,		/* addmulti */
	nil,		/* remmulti */
	nil,		/* pktin */
	nil,		/* addroute */
	nil,		/* remroute */
	nil,		/* flushroute */
	nil,		/* joinmulti */
	nil,		/* leave multi */
	0,		/* don't unbind on last close */
};

void
nullmediumlink(void)
{
	addipmedium(&nullmedium);
}
