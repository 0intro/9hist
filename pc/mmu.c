#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

/*
 *  global descriptor table describing all segments
 */
Segdesc gdt[1024] =
{
[NULLSEG]	{ 0, 0},		/* null descriptor */
[KESEG]		EXECSEG(0),		/* kernel code */
[KDSEG]		DATASEG(0),		/* kernel data/stack */
[UESEG]		EXECSEG(3),		/* user code */
[UDSEG]		DATASEG(3),		/* user data/stack */
[SYSGATE]	CALLGATE(KESEG, syscall, 3),	/* call gate for system calls */
		{ 0, 0},		/* the rest */
};
