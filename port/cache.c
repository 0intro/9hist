#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"devtab.h"

typedef struct Fcache Fcache;
typedef struct Fcalloc Fcalloc;
struct Fcache
{
	QLock;
	Qid;
	Page*	alloc;
	Fcache*	hash;
	Fcache*	next;
};

struct Fcalloc
{
	ulong	start;
	short	len;
	Page*	data;
};

Fcache*
clook(Qid *qid)
{

}
