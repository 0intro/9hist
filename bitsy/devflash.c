#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

/* flash partitions */
typedef struct FlashPart FlashPart;
struct FlashPart
{
	QLock;
	char	name[NAMELEN];
	ulong	start;		/* byte offsets */
	ulong	end;
};
