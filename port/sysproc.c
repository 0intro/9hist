#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"errno.h"

#include	<a.out.h>

int	shargs(char*, int, char**);

long
sysr1(ulong *arg)
{
	print("[%s %s %d] r1 = %d\n", u->p->pgrp->user, u->p->text, u->p->pid, arg[0]);
	return 0;
}

long
sysfork(ulong *arg)
{
	Proc *p;
	Seg *s;
	Page *np, *op;
	ulong usp, upa, pid;
	Chan *c;
	Orig *o;
	KMap *k;
	int n, on, i;
	int lastvar;	/* used to compute stack address */

	/*
	 * Kernel stack
	 */
	p = newproc();
	p->upage = newpage(1, 0, USERADDR|(p->pid&0xFFFF));
	k = kmap(p->upage);
	upa = VA(k);

	/*
	 * Save time: only copy u-> data and useful stack
	 */
	clearmmucache();	/* so child doesn't inherit any of your mappings */
	memmove((void*)upa, u, sizeof(User));
	n = USERADDR+BY2PG - (ulong)&lastvar;
	n = (n+32) & ~(BY2WD-1);	/* be safe & word align */
	memmove((void*)(upa+BY2PG-n), (void*)(USERADDR+BY2PG-n), n);
	((User *)upa)->p = p;
	kunmap(k);

	/*
	 * User stack
	 */
	p->seg[SSEG] = u->p->seg[SSEG];
	s = &p->seg[SSEG];
	s->proc = p;
	on = (s->maxva-s->minva)>>PGSHIFT;
	usp = ((Ureg*)UREGADDR)->usp;
	if(usp >= USTKTOP)
		panic("fork bad usp %lux", usp);
	if(usp < u->p->seg[SSEG].minva)
		s->minva = u->p->seg[SSEG].minva;
	else
		s->minva = usp & ~(BY2PG-1);
	usp = s->minva & (BY2PG-1);	/* just low bits */
	s->maxva = USTKTOP;
	n = (s->maxva-s->minva)>>PGSHIFT;
	s->o = neworig(s->minva, n, OWRPERM|OISMEM, 0);
	lock(s->o);
	/*
	 * Only part of last stack page
	 */
	for(i=0; i<n; i++){
		op = u->p->seg[SSEG].o->pte[i+(on-n)].page;
		if(op){
			np = newpage(1, s->o, op->va);
			k = kmap(np);
			p->seg[SSEG].o->pte[i].page = np;
			if(i == 0){	/* only part of last stack page */
				memset((void*)VA(k), 0, usp);
				memmove((void*)(VA(k)+usp),
					(void*)(op->va+usp), BY2PG-usp);
			}else		/* all of higher pages */
				memmove((void*)VA(k), (void*)op->va, BY2PG);
			kunmap(k);
		}
	}
	unlock(s->o);
	/*
	 * Duplicate segments
	 */
	for(s=&u->p->seg[0], n=0; n<NSEG; n++, s++){
		if(n == SSEG)		/* already done */
			continue;
		if(s->o == 0)
			continue;
		p->seg[n] = *s;
		p->seg[n].proc = p;
		o = s->o;
		lock(o);
		o->nproc++;
		if(s->mod)
			forkmod(s, &p->seg[n], p);
		unlock(o);
	}
	/*
	 * Refs
	 */
	incref(u->dot);
	for(n=0; n<=u->maxfd; n++)
		if(c = u->fd[n])	/* assign = */
			incref(c);
	/*
	 * Sched
	 */
	if(setlabel(&p->sched)){
		u->p = p;
		p->state = Running;
		p->mach = m;
		m->proc = p;
		spllo();
		return 0;
	}
	p->parent = u->p;
	p->parentpid = u->p->pid;
	p->pgrp = u->p->pgrp;
	p->fpstate = u->p->fpstate;
	incref(p->pgrp);
	u->p->nchild++;
	pid = p->pid;
	memset(p->time, 0, sizeof(p->time));
	p->time[TReal] = MACHP(0)->ticks;
	memmove(p->text, u->p->text, NAMELEN);
	/*
	 *  since the bss/data segments are now shareable,
	 *  any mmu info about this process is now stale
	 *  (i.e. has bad properties) and has to be discarded.
	 */
	flushmmu();
	clearmmucache();
	ready(p);
	return pid;
}

long
sysexec(ulong *arg)
{
	Proc *p;
	Seg *s;
	ulong l, t, d, b, v;
	int i;
	Chan *tc, *c;
	Orig *o;
	char **argv, **argp;
	char *a, *charp, *file;
	char *progarg[sizeof(Exec)/2+1], elem[NAMELEN];
	ulong ssize, spage, nargs, nbytes, n, bssend;
	ulong *sp;
	int indir;
	Exec exec;
	char line[sizeof(Exec)];

	p = u->p;
	validaddr(arg[0], 1, 0);
	file = (char*)arg[0];
	indir = 0;
    Header:
	tc = namec(file, Aopen, OEXEC, 0);
	if(waserror()){
		close(tc);
		nexterror();
	}
	if(!indir)
		strcpy(elem, u->elem);
	n = (*devtab[tc->type].read)(tc, &exec, sizeof(Exec), 0);
	if(n < 2)
    Err:
		error(Ebadexec);
	if(n==sizeof(Exec) && exec.magic==AOUT_MAGIC){
		if((exec.text&KZERO)
		|| (ulong)exec.entry < UTZERO+sizeof(Exec)
		|| (ulong)exec.entry >= UTZERO+sizeof(Exec)+exec.text)
			goto Err;
		goto Binary;
	}

	/*
	 * Process #! /bin/sh args ...
	 */
	memmove(line, &exec, sizeof(Exec));
	if(indir || line[0]!='#' || line[1]!='!')
		goto Err;
	n = shargs(line, n, progarg);
	if(n == 0)
		goto Err;
	indir = 1;
	/*
	 * First arg becomes complete file name
	 */
	progarg[n++] = file;
	progarg[n] = 0;
	validaddr(arg[1], BY2WD, 1);
	arg[1] += BY2WD;
	file = progarg[0];
	progarg[0] = elem;
	poperror();
	close(tc);
	goto Header;

    Binary:
	t = (UTZERO+sizeof(Exec)+exec.text+(BY2PG-1)) & ~(BY2PG-1);
	d = (t + exec.data + (BY2PG-1)) & ~(BY2PG-1);
	bssend = t + exec.data + exec.bss;
	b = (bssend + (BY2PG-1)) & ~(BY2PG-1);
	if((t|d|b) & KZERO)
		error(Ebadexec);

	/*
	 * Args: pass 1: count
	 */
	nbytes = 0;
	nargs = 0;
	if(indir){
		argp = progarg;
		while(*argp){
			a = *argp++;
			nbytes += strlen(a) + 1;
			nargs++;
		}
	}
	evenaddr(arg[1]);
	argp = (char**)arg[1];
	validaddr((ulong)argp, BY2WD, 0);
	while(*argp){
		a = *argp++;
		if(((ulong)argp&(BY2PG-1)) < BY2WD)
			validaddr((ulong)argp, BY2WD, 0);
		validaddr((ulong)a, 1, 0);
		nbytes += (vmemchr(a, 0, 0xFFFFFFFF) - a) + 1;
		nargs++;
	}
	ssize = BY2WD*(nargs+1) + ((nbytes+(BY2WD-1)) & ~(BY2WD-1));
	spage = (ssize+(BY2PG-1)) >> PGSHIFT;
	/*
	 * Build the stack segment, putting it in kernel virtual for the moment
	 */
	if(spage > TSTKSIZ)
		errors("not enough argument stack space");

	s = &p->seg[ESEG];
	s->proc = p;
	s->o = neworig(TSTKTOP-(spage<<PGSHIFT), spage, OWRPERM|OISMEM, 0);
	s->minva = s->o->va;
	s->maxva = TSTKTOP;

	/*
	 * Args: pass 2: assemble; the pages will be faulted in
	 */
	argv = (char**)(TSTKTOP - ssize);
	charp = (char*)(TSTKTOP - nbytes);
	if(indir)
		argp = progarg;
	else
		argp = (char**)arg[1];
	for(i=0; i<nargs; i++){
		if(indir && *argp==0){
			indir = 0;
			argp = (char**)arg[1];
		}
		*argv++ = charp + (USTKTOP-TSTKTOP);
		n = strlen(*argp) + 1;
		memmove(charp, *argp++, n);
		charp += n;
	}

	memmove(p->text, elem, NAMELEN);

	/*
	 * Committed.  Free old memory
	 */
	freesegs(ESEG);

	/*
	 * Close on exec
	 */
	for(i=0; i<=u->maxfd; i++)
		if((c=u->fd[i]) && c->flag&CCEXEC){
			close(c);
			fdclose(i);
		}

	/*
	 * Text.  Shared.
	 */
	s = &p->seg[TSEG];
	s->proc = p;
	o = lookorig(UTZERO, (t-UTZERO)>>PGSHIFT, OCACHED|OISMEM, tc);
	if(o == 0){
		o = neworig(UTZERO, (t-UTZERO)>>PGSHIFT, OCACHED|OISMEM, tc);
		o->minca = 0;
		o->maxca = sizeof(Exec)+exec.text;
	}
	s->o = o;
	s->minva = UTZERO;
	s->maxva = t;
	s->mod = 0;

	/*
	 * Data.  Shared.
	 */
	s = &p->seg[DSEG];
	s->proc = p;
	o = lookorig(t, (d-t)>>PGSHIFT, OWRPERM|OPURE|OCACHED|OISMEM, tc);
	if(o == 0){
		o = neworig(t, (d-t)>>PGSHIFT, OWRPERM|OPURE|OCACHED|OISMEM, tc);
		o->minca = p->seg[TSEG].o->maxca;
		o->maxca = o->minca + exec.data;
	}
	s->o = o;
	s->minva = t;
	s->maxva = d;
	s->mod = 0;

	/*
	 * BSS.  Created afresh.
	 */
	s = &p->seg[BSEG];
	s->proc = p;
	o = neworig(d, (b-d)>>PGSHIFT, OWRPERM|OISMEM, 0);
	o->minca = 0;
	o->maxca = 0;
	s->o = o;
	s->minva = d;
	s->maxva = b;
	s->mod = 0;

	poperror();
	close(tc);

	p->seg[BSEG].endseg = bssend;

	/*
	 * Move the stack
	 */
	s = &p->seg[SSEG];
	*s = p->seg[ESEG];
	p->seg[ESEG].o = 0;
	o = s->o;
	o->va += (USTKTOP-TSTKTOP);
	s->minva = o->va;
	s->maxva = USTKTOP;
	lock(o);
	for(i=0; i<o->npte; i++)
		o->pte[i].page->va += (USTKTOP-TSTKTOP);
	unlock(o);

	/*
	 *  at this point, the mmu contains info about the old address
	 *  space and needs to be flushed
	 */
	flushmmu();
	clearmmucache();
	execpc(exec.entry);
	sp = (ulong*)(USTKTOP - ssize);
	*--sp = nargs;
	((Ureg*)UREGADDR)->usp = (ulong)sp;
	lock(&p->debug);
	u->nnote = 0;
	u->notify = 0;
	u->notified = 0;
	procsetup(p);
	unlock(&p->debug);
	return 0;
}

int
shargs(char *s, int n, char **ap)
{
	int i;

	s += 2, n -= 2;		/* skip #! */
	for(i=0; s[i]!='\n'; i++)
		if(i == n-1)
			return 0;
	s[i] = 0;
	*ap = 0;
	i = 0;
	for(;;){
		while(*s==' ' || *s=='\t')
			s++;
		if(*s == 0)
			break;
		i++;
		*ap++ = s;
		*ap = 0;
		while(*s && *s!=' ' && *s!='\t')
			s++;
		if(*s == 0)
			break;
		else
			*s++ = 0;
	}
	return i;
}

int
return0(void *a)
{
	return 0;
}

long
syssleep(ulong *arg)
{
	tsleep(&u->p->sleep, return0, 0, arg[0]);
	return 0;
}

long
sysalarm(ulong *arg)
{
	return procalarm(arg[0]);		
}

long
sysexits(ulong *arg)
{
	char *status;

	status = (char*)arg[0];
	if(status){
		if(waserror())
			status = "invalid exit string";
		else{
			validaddr((ulong)status, 1, 0);
			vmemchr(status, 0, ERRLEN);
		}
		poperror();
	}
	pexit(status, 1);
}

long
syswait(ulong *arg)
{
	if(arg[0]){
		validaddr(arg[0], sizeof(Waitmsg), 1);
		evenaddr(arg[0]);
	}
	return pwait((Waitmsg*)arg[0]);
}

long
sysdeath(ulong *arg)
{
	pprint("deprecated system call");
	pexit("Suicide", 0);
}

long
syserrstr(ulong *arg)
{
	char buf[ERRLEN];

	validaddr(arg[0], ERRLEN, 1);
	memmove((char*)arg[0], u->error, ERRLEN);
	return 0;
}

long
sysforkpgrp(ulong *arg)
{
	int mask;
	Pgrp *pg;

	pg = newpgrp();
	if(waserror()){
		closepgrp(pg);
		nexterror();
	}

	mask = arg[0];
	if(mask == FPall)
		mask = FPnote|FPenv|FPnamespc;

	memmove(pg->user, u->p->pgrp->user, NAMELEN);

	if(mask & FPnamespc)
		pgrpcpy(pg, u->p->pgrp);

	if(mask & FPenv)
		envcpy(pg, u->p->pgrp);

	if((mask & FPnote) == 0) {
		u->nnote = 0;
		u->notified = 0;
		memset(u->note, 0, sizeof(u->note));
	}

	poperror();
	closepgrp(u->p->pgrp);
	u->p->pgrp = pg;
	return pg->pgrpid;
}

long
sysnotify(ulong *arg)
{
	if(arg[0] != 0)
		validaddr(arg[0], sizeof(ulong), 0);
	u->notify = (int(*)(void*, char*))(arg[0]);
	return 0;
}

long
sysnoted(ulong *arg)
{
	if(u->notified == 0)
		error(Egreg);
	return 0;
}

/*
 * Temporary; should be replaced by a generalized segment control operator
 */
long
sysbrk_(ulong *arg)
{
	return ibrk(arg[0], BSEG);
}

long
syslkbrk_(ulong *arg)
{
	return ibrk(arg[0], LSEG);
}
