#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

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

#define NA 10		/* alarms per clock tick */
void
checkalarms(void)
{
	int i, n;
	Alarm *a;
	void (*f)(void*);
	Alarm *alist[NA];

	if(canlock(&m->alarmlock)){
		a = m->alarm;
		if(a){
			for(n=0; a && a->dt<=0 && n<NA; n++){
				alist[n] = a;
				delete(&m->alarm, 0, a);
				a = m->alarm;
			}
			if(a)
				a->dt--;
			unlock(&m->alarmlock);

			/*  execute alarm functions outside the lock */
			for(i = 0; i < n; i++){
				f = alist[i]->f;	/* avoid race with cancel */
				if(f)
					(*f)(alist[i]);
				alist[i]->busy = 0;
			}
		} else
			unlock(&m->alarmlock);
	}
}
