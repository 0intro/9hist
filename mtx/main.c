#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"init.h"
#include	"pool.h"

Conf	conf;
FPsave	initfp;

void
main(void)
{
//	xinit();
	printinit();
	i8250console();
	print("\nPlan 9\n");

	for(;;);
}
