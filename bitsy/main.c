#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"init.h"
#include	"pool.h"

Mach *m;
Conf conf;

void
main(void)
{
	putuartstr("hello ken");
}

/*
 *  exit kernel either on a panic or user request
 */
void
exit(int ispanic)
{
	void (*f)();

	f = nil;
	(*f)();
}

/*
 *  set mach dependent process state for a new process
 */
void
procsetup(Proc *p)
{
	p->fpstate = FPinit;
}

/*
 *  Save the mach dependent part of the process state.
 */
void
procsave(Proc *p)
{
	USED(p);
}

/* place holder */
void
serialputs(char*, int)
{
}

/*
 *  dummy since rdb is not included 
 */
void
rdb(void)
{
}
