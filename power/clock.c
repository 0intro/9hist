#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

#include	"ureg.h"

Alarm	*alarmtab;

Alarm*
alarm(int ms, void (*f)(Alarm*), void *arg)
{
	Alarm *a, *w, *pw;
	ulong s;
	if(ms < 0)
		ms = 0;
	a = newalarm();
	a->dt = MS2TK(ms);
	a->f = f;
	a->arg = arg;
	s = splhi();
	lock(&m->alarmlock);
	pw = 0;
	for(w=m->alarm; w; pw=w, w=w->next){
		if(w->dt <= a->dt){
			a->dt -= w->dt;
			continue;
		}
		w->dt -= a->dt;
		break;
	}
	insert(&m->alarm, pw, a);
	unlock(&m->alarmlock);
	splx(s);
	return a;
}

void
cancel(Alarm *a)
{
	a->f = 0;
}

Alarm*
newalarm(void)
{
	int i;
	Alarm *a;

	for(i=0,a=alarmtab; i<conf.nalarm; i++,a++)
		if(a->busy==0 && a->f==0 && canlock(a)){
			if(a->busy){
				unlock(a);
				continue;
			}
			a->f = 0;
			a->arg = 0;
			a->busy = 1;
			unlock(a);
			return a;
		}
	panic("newalarm");
}

void
alarminit(void)
{
	int i;

	alarmtab = ialloc(conf.nalarm*sizeof(Alarm), 0);
	for(i=0; i<conf.nalarm; i++){
		lock(&alarmtab[i]);	/* allocate locks, as they are used at interrupt time */
		unlock(&alarmtab[i]);
	}
}

void
delay(int ms)
{
	ulong t, *p;
	int i;

	ms *= 7000;	/* experimentally determined */
	for(i=0; i<ms; i++)
		;
}

/*
 * AMD 82C54 timer
 *
 * ctr2 is clocked at 3.6864 MHz.
 * ctr2 output clocks ctr0 and ctr1.
 * ctr0 drives INTR2.  ctr1 drives INTR4.
 * To get 100Hz, 36864==9*4096=36*1024 so clock ctr2 every 1024 and ctr0 every 36.
 */

struct Timer{
	uchar	cnt0,
		junk0[3];
	uchar	cnt1,
		junk1[3];
	uchar	cnt2,
		junk2[3];
	uchar	ctl,
		junk3[3];
};

#define	TIME0	(36*MS2HZ/10)
#define	TIME1	0xFFFFFFFF	/* later, make this a profiling clock */
#define	TIME2	1024
#define	CTR(x)	((x)<<6)	/* which counter x */
#define	SET16	0x30		/* lsbyte then msbyte */
#define	MODE2	0x04		/* interval timer */

void
clockinit(void)
{
	Timer *t;
	int i;

	t = TIMERREG;
	t->ctl = CTR(2)|SET16|MODE2;
	t->cnt2 = TIME2&0xFF;
	t->cnt2 = (TIME2>>8)&0xFF;
	t->ctl = CTR(1)|SET16|MODE2;
	t->cnt1 = TIME1&0xFF;
	t->cnt1 = (TIME1>>8)&0xFF;
	t->ctl = CTR(0)|SET16|MODE2;
	t->cnt0 = TIME0;
	t->cnt0 = (TIME0>>8)&0xFF;
	i = *CLRTIM0;
	i = *CLRTIM1;
	m->ticks = 0;
}

#define NA 10
void
clock(ulong n)
{
	int i, na;
	Alarm *a;
	void (*f)(void*);
	Proc *p;
	Alarm *alist[NA];

	if(n&INTR2){
		i = *CLRTIM0;
		m->ticks++;
		if(m->machno == 0){
			p = m->proc;
			if(p == 0)
				p = m->intrp;
			if(p)
				p->time[p->insyscall]++;
			for(i=1; i<conf.nmach; i++){
				if(active.machs & (1<<i)){
					p = MACHP(i)->proc;
					if(p && p!=m->intrp)
						p->time[p->insyscall]++;
				}
			}
			m->intrp = 0;
			printslave();
		}
		if(active.exiting && active.machs&(1<<m->machno)){
			print("someone's exiting\n");
			exit();
		}
		if(canlock(&m->alarmlock)){
			if(m->alarm){
				a = m->alarm;
				a->dt--;
				for(na = 0; a && a->dt<=0 && na<NA; na++){
					alist[na] = a;
					delete(&m->alarm, 0, a);
					a = m->alarm;
				}
				unlock(&m->alarmlock);
	
				/*  execute alarm functions outside the lock */
				for(i = 0; i < na; i++){
					f = alist[i]->f;	/* avoid race with cancel */
					if(f)
						(*f)(alist[i]);
					alist[i]->busy = 0;
				}
			} else
				unlock(&m->alarmlock);
		}
		return;
	}
	if(n & INTR4){
		i = *CLRTIM1;
		return;
	}
}
