#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

void
mmuswitch(Proc *p)
{
	int tp;

	if(p->newtlb) {
		memset(p->pidonmach, 0, sizeof p->pidonmach);
		p->newtlb = 0;
	}
	tp = p->pidonmach[m->machno];
	if(tp == 0)
		tp = newtlbpid(p);

	putcontext(tp);
}

void
mmurelease(Proc *p)
{
	memset(p->pidonmach, 0, sizeof p->pidonmach);
}

/*
 * Process must be non-interruptible
 */
int
newtlbpid(Proc *p)
{
	int i, s;
	Proc **h;

	i = m->lastpid;
	h = m->pidproc;
	for(s = 0; s < NTLBPID; s++) {
		i++;
		if(i >= NTLBPID)
			i = 1;
		if(h[i] == 0)
			break;
	}

	if(h[i])
		purgetlb(i);
	if(h[i] != 0)
		panic("newtlb");

	m->pidproc[i] = p;
	p->pidonmach[m->machno] = i;
	m->lastpid = i;

	return i;
}

void
putmmu(ulong tlbvirt, ulong tlbphys, Page *pg)
{
	short tp;
	char *ctl;

	splhi();

	ctl = &pg->cachectl[m->machno]; 
	if(*ctl == PG_TXTFLUSH) {
		dcflush((void*)pg->pa, BY2PG);
		icflush((void*)pg->pa, BY2PG);
		*ctl = PG_NOFLUSH;
	}

	tp = up->pidonmach[m->machno];
	if(tp == 0)
		tp = newtlbpid(up);

	tlbvirt |= PTEPID(tp);
	putstlb(tlbvirt, tlbphys);
	puttlb(tlbvirt, tlbphys);
	spllo();
}

void
purgetlb(int tlbpid)
{
	int i, mno;
	Proc *sp, **pidproc;
	Softtlb *entry, *etab;

	m->tlbpurge++;

	/*
	 * find all pid entries that are no longer used by processes
	 */
	mno = m->machno;
	pidproc = m->pidproc;
	for(i=1; i<NTLBPID; i++) {
		sp = pidproc[i];
		if(sp && sp->pidonmach[mno] != i)
			pidproc[i] = 0;
	}

	/*
	 * shoot down the one we want
	 */
	sp = pidproc[tlbpid];
	if(sp != 0)
		sp->pidonmach[mno] = 0;
	pidproc[tlbpid] = 0;

	/*
	 * clean out all dead tlbpids from the stlb;
	 */
	entry = m->stb;
	for(etab = &entry[STLBSIZE]; entry < etab; entry++)
		if(pidproc[TLBPID(entry->virt)] == 0)
			entry->virt = 0;

	/*
	 * clean up the hardware
	 */
	for(i=TLBROFF; i<NTLB; i++)
		if(pidproc[TLBPID(gettlbvirt(i))] == 0)
			puttlbx(i, KZERO | PTEPID(i), 0);
}

void
flushmmu(void)
{
	splhi();
	up->newtlb = 1;
	mmuswitch(up);
	spllo();
}

void
clearmmucache(void)
{
}

void
putstlb(ulong tlbvirt, ulong tlbphys)
{
	Softtlb *entry;

	/* This hash function is also coded into utlbmiss in l.s */
	entry = &m->stb[((tlbvirt<<1) ^ (tlbvirt>>12)) & (STLBSIZE-1)];
	entry->phys = tlbphys;
	entry->virt = tlbvirt;
	if(tlbphys == 0)
		entry->virt = 0;
}
