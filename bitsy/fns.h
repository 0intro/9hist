#include "../port/portfns.h"

int	cistrcmp(char*, char*);
int	cistrncmp(char*, char*, int);
#define	clearmmucache()				/* x86 doesn't have one */
void	clockintr(Ureg*, void*);
void	clockintrsched(void);
void	(*coherence)(void);
void	delay(int);
void	evenaddr(ulong);
#define	getpgcolor(a)	0
void	idle(void);
#define	idlehands()			/* nothing to do in the runproc */
int	iprint(char*, ...);
#define	procrestore(p)
void	procsave(Proc*);
void	procsetup(Proc*);
void	screeninit(void);
int	screenprint(char*, ...);			/* debugging */
void	(*screenputs)(char*, int);
void	touser(void*);
void	trapenable(int, void (*)(Ureg*, void*), void*, char*);
void	trapinit(void);
int	tas(void*);
void	wbflush(void);

#define	waserror()	(up->nerrlab++, setlabel(&up->errlab[up->nerrlab-1]))
#define KADDR(a)	((void*)((ulong)(a)|KZERO))
#define PADDR(a)	((ulong)(a)&~KZERO)

#define	dcflush(a, b)
