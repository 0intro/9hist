#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

static void
genericreset(void)
{
	print("Reset the machine!\n");
	for(;;);
}

PCArch generic =
{
	"generic",
	genericreset,
	0,
	0,
	0,
	0,
	0,
	0,
};
