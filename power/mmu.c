#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

/*
 * Called splhi, not in Running state
 */
void
mapstack(Proc *p)
{
	short tp;
	ulong tlbvirt, tlbphys;

	tp = p->pidonmach[m->machno];
	if(tp == 0){
		tp = newtlbpid(p);
		p->pidonmach[m->machno] = tp;
	}
/*	if(p->upage->va != (USERADDR|(p->pid&0xFFFF)))
		panic("mapstack %d 0x%lux 0x%lux", p->pid, p->upage->pa, p->upage->va);
*/
	/* don't set m->pidhere[*tp] because we're only writing entry 0 */
	tlbvirt = USERADDR | PTEPID(tp);
	tlbphys = p->upage->pa | PTEWRITE | PTEVALID | PTEGLOBL;
	puttlbx(0, tlbvirt, tlbphys);
	u = (User*)USERADDR;
}

/*
 * Process must be non-interruptible
 */
int
newtlbpid(Proc *p)
{
	int i;
	Proc *sp;

	i = m->lastpid+1;
	if(i >= NTLBPID)
		i = 1;
	sp = m->pidproc[i];
	if(sp){
		sp->pidonmach[m->machno] = 0;
		purgetlb(i);
	}
	m->pidproc[i] = p;
	m->lastpid = i;
	return i;
}

void
putmmu(ulong tlbvirt, ulong tlbphys)
{
	short tp;
	Proc *p;

	splhi();
	p = u->p;
/*	if(p->state != Running)
		panic("putmmu state %lux %lux %s\n", u, p, statename[p->state]);
*/
	p->state = MMUing;
	tp = p->pidonmach[m->machno];
	if(tp == 0){
		tp = newtlbpid(p);
		p->pidonmach[m->machno] = tp;
	}
	tlbvirt |= PTEPID(tp);
	puttlb(tlbvirt, tlbphys);
	m->pidhere[tp] = 1;
	p->state = Running;
	spllo();
}

void
purgetlb(int pid)
{
	int i, rpid;

	if(m->pidhere[pid] == 0)
		return;
	memset(m->pidhere, 0, sizeof m->pidhere);
	for(i=TLBROFF; i<NTLB; i++){
		rpid = (gettlbvirt(i)>>6) & 0x3F;
		if(rpid == pid)
			puttlbx(i, KZERO | PTEPID(i), 0);
		else
			m->pidhere[rpid] = 1;
	}
}

void
flushmmu(void)
{
	splhi();
	/* easiest is to forget what pid we had.... */
	memset(u->p->pidonmach, 0, sizeof u->p->pidonmach);
	/* ....then get a new one by trying to map our stack */
	mapstack(u->p);
	spllo();
}
