#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

void	ediv(void);
void	edebug(void);
void	enmi(void);
void	ebp(void);
void	eover(void);
void	ebound(void);
void	ebadop(void);
void	enoco(void);
void	edfault(void);
void	enoway(void);
void	etss(void);
void	enoseg(void);
void	estack(void);
void	eprot(void);
void	efault(void);

/*
 *  exception types
 */
enum {
	Ediv=	0,	/* divide error */
	Edebug=	1,	/* debug exceptions */
	Enmi=	2,	/* non-maskable interrupt */
	Ebp=	3,	/* INT 3 */
	Eover=	4,	/* overflow */
	Ebound=	5,	/* bounds check */
	Ebadop=	6,	/* invalid opcode */
	Enoco=	7,	/* coprocessor not available */
	Edfault=8,	/* double fault */
	Etss=	10,	/* invalid tss */
	Enoseg=	11,	/* segment not present */
	Estack=	12,	/* stack exception */
	Eprot=	13,	/* general protection */
	Efault=	14,	/* page fault */
	Eco=	16,	/* coprocessor error */
};

/*
 *  exception/trap gates
 */
Segdesc ilt[256] =
{
[Ediv]		TRAPGATE(KESEL, ediv, 3),
[Edebug]	TRAPGATE(KESEL, edebug, 3),
[Enmi]		INTRGATE(KESEL, enmi, 3),
[Ebp]		TRAPGATE(KESEL, ebp, 3),
[Eover]		TRAPGATE(KESEL, eover, 3),
[Ebound]	TRAPGATE(KESEL, ebound, 3),
[Ebadop]	TRAPGATE(KESEL, ebadop, 3),
[Enoco]		TRAPGATE(KESEL, enoco, 3),
[Edfault]	TRAPGATE(KESEL, edfault, 3),
[Etss]		TRAPGATE(KESEL, etss, 3),
[Enoseg]	TRAPGATE(KESEL, enoseg, 3),
[Estack]	TRAPGATE(KESEL, estack, 3),
[Eprot]		TRAPGATE(KESEL, eprot, 3),
[Efault]	TRAPGATE(KESEL, efault, 3),
[Eco]		TRAPGATE(KESEL, eco, 3),
};
