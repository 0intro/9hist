#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"../port/edf.h"

int
isedf(Proc*)
{
	return 0;
}

void
edf_bury(Proc*)
{
}

int
edf_anyready(void)
{
	return 0;
}

void
edf_ready(Proc*)
{
}

Proc*
edf_runproc(void)
{
	return nil;
}

void
edf_block(Proc*)
{
}

