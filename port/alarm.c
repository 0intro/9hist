#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

struct {
	QLock;
	Alarm	*tab;	/* table of all alarm structures */
	void	*list;	/* busy alarms */
	Rendez	r;
} alarmalloc;

Alarms	alarms;

Alarm*
alarm(int ms, void (*f)(Alarm*), void *arg)
{
	Alarm *a, *w, *pw;

	if(ms < 0)
		ms = 0;
	a = newalarm();
	a->when = MACHP(0)->ticks+MS2TK(ms);
	a->f = f;
	a->arg = arg;
	qlock(&alarmalloc);
	pw = 0;
	for(w=alarmalloc.list; w; pw=w, w=w->next){
		if(w->when > a->when)
			break;
	}
	insert(&alarmalloc.list, pw, a);
	qunlock(&alarmalloc);
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

	for(i=0,a=alarmalloc.tab; i<conf.nalarm; i++,a++)
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
	return 0;	/* not reached */
}

/* allocate locks here since you can't allocate them at interrupt time (on the SGI) */
void
alarminit(void)
{
	int i;

	alarmalloc.tab = ialloc(conf.nalarm*sizeof(Alarm), 0);
	for(i=0; i<conf.nalarm; i++){
		lock(&alarmalloc.tab[i]);
		unlock(&alarmalloc.tab[i]);
	}
	qlock(&alarmalloc);
	qunlock(&alarmalloc);
	qlock(&alarms);
	qunlock(&alarms);
}

#define NA 10		/* alarms per wakeup */
void
alarmkproc(void *arg)
{
	int i, n;
	Alarm *a;
	void (*f)(void*);
	Alarm *alist[NA];
	ulong now;
	Proc *rp;

	USED(arg);

	for(;;){
		now = MACHP(0)->ticks;

		qlock(&alarmalloc);
		a = alarmalloc.list;
		if(a){
			for(n=0; a && a->when<=now && n<NA; n++){
				alist[n] = a;
				delete(&alarmalloc.list, 0, a);
				a = alarmalloc.list;
			}
			qunlock(&alarmalloc);

			/*
			 *  execute alarm functions outside the lock since they
			 *  might call alarm().
			 */
			f = 0;
			if(waserror()){
				print("alarm func %lux caused error %s\n", f, u->error);
			} else {
				for(i = 0; i < n; i++){
					f = alist[i]->f; /* avoid race with cancel */
					if(f)
						(*f)(alist[i]);
					alist[i]->busy = 0;
				}
				poperror();
			}
		}else
			qunlock(&alarmalloc);

		qlock(&alarms);
		while((rp = alarms.head) && rp->alarm <= now){
			if(rp->alarm != 0L){
				if(canqlock(&rp->debug)){
					if(!waserror()){
						postnote(rp, 0, "alarm", NUser);
						poperror();
					}
					qunlock(&rp->debug);
					rp->alarm = 0L;
				}else
					break;
			}
			alarms.head = rp->palarm;
		}
		qunlock(&alarms);

		sleep(&alarmalloc.r, return0, 0);
	}
}

/*
 *  called every clock tick
 */
void
checkalarms(void)
{
	ulong now;
	Proc *p;
	Alarm *a;

	if(m != MACHP(0))
		return;
	a = alarmalloc.list;
	p = alarms.head;
	now = MACHP(0)->ticks;

	if((a && a->when <= now) || (p && p->alarm <= now))
		wakeup(&alarmalloc.r);
}

ulong
procalarm(ulong time)
{
	Proc **l, *f;
	ulong when, old;

	old = TK2MS(u->p->alarm - MACHP(0)->ticks);
	if(time == 0){
		u->p->alarm = 0;
		return old;
	}
	when = MS2TK(time);
	when += MACHP(0)->ticks;

	qlock(&alarms);
	l = &alarms.head;
	for(f = *l; f; f = f->palarm){
		if(u->p == f){
			*l = f->palarm;
			break;
		}
		l = &f->palarm;
	}

	u->p->palarm = 0;
	if(alarms.head){
		l = &alarms.head;
		for(f = *l; f; f = f->palarm){
			if(f->alarm > when){
				u->p->palarm = f;
				*l = u->p;
				goto done;
			}
			l = &f->palarm;
		}
		*l = u->p;
	}
	else
		alarms.head = u->p;
done:
	u->p->alarm = when;
	qunlock(&alarms);

	return old;			
}

/*
 * Insert new into list after where
 */
void
insert(List **head, List *where, List *new)
{
	if(where == 0){
		new->next = *head;
		*head = new;
	}else{
		new->next = where->next;
		where->next = new;
	}
		
}

/*
 * Delete old from list.  where->next is known to be old.
 */
void
delete(List **head, List *where, List *old)
{
	if(where == 0){
		*head = old->next;
		return;
	}
	where->next = old->next;
}

