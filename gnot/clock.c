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
	a->dt = ms/MS2HZ;
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

	ms *= 1000;	/* experimentally determined */
	for(i=0; i<ms; i++)
		;
}

void
clock(Ureg *ur)
{
	int i;
	Alarm *a;
	void (*f)(void*);
	Proc *p;

	SYNCREG[1] = 0x5F;	/* clear interrupt */
	m->ticks++;
	p = m->proc;
	if(p){
		p->pc = ur->pc;
		p->time[p->insyscall]++;
	}
	if(canlock(&m->alarmlock)){
		if(m->alarm){
			a = m->alarm;
			a->dt--;
			while(a && a->dt<=0){
				f = a->f;	/* avoid race with cancel */
				if(f)
					(*f)(a);
				delete(&m->alarm, 0, a);
				a->busy = 0;
				a = m->alarm;
			}
		}
		unlock(&m->alarmlock);
	}
	kbdclock();
	mouseclock();
	if((ur->sr&SPL(7)) == 0){
		spllo();
		if(p && p->state==Running)
			checksched();
	}
}
