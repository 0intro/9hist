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
	Segment *s;
	Page *np, *op;
	ulong usp, upa, pid;
	Chan *c;
	KMap *k;
	int n, on, i;
	int lastvar;	/* used to compute stack address */

	p = newproc();

	/* Page va of upage used as check in mapstack */
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

	/* Make a new set of memory segments */
	for(i = 0; i < NSEG; i++)
		if(u->p->seg[i])
			p->seg[i] = dupseg(u->p->seg[i]);

	/*
	 * Refs
	 */
	incref(u->dot);				/* File descriptors etc. */
	p->fgrp = dupfgrp(u->p->fgrp);

	p->pgrp = u->p->pgrp;			/* Process groups */
	incref(p->pgrp);

	p->egrp = u->p->egrp;			/* Environment group */
	incref(p->egrp);

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

	p->fpstate = u->p->fpstate;
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

static ulong
l2be(long l)
{
	uchar *cp;

	cp = (uchar*)&l;
	return (cp[0]<<24) | (cp[1]<<16) | (cp[2]<<8) | cp[3];
}

long
sysexec(ulong *arg)
{
	Proc *p;
	Segment *s, *ts;
	ulong l, t, d, b, v;
	int i;
	Chan *tc, *c;
	char **argv, **argp;
	char *a, *charp, *file;
	char *progarg[sizeof(Exec)/2+1], elem[NAMELEN];
	ulong ssize, spage, nargs, nbytes, n, bssend;
	ulong *sp;
	int indir;
	Exec exec;
	char line[sizeof(Exec)];
	Fgrp *f;
	Image *img;
	ulong magic, text, entry, data, bss;

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
	magic = l2be(exec.magic);
	text = l2be(exec.text);
	entry = l2be(exec.entry);
	if(n==sizeof(Exec) && magic==AOUT_MAGIC){
		if((text&KZERO)
		|| entry < UTZERO+sizeof(Exec)
		|| entry >= UTZERO+sizeof(Exec)+text)
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
	poperror();
	data = l2be(exec.data);
	bss = l2be(exec.bss);
	t = (UTZERO+sizeof(Exec)+text+(BY2PG-1)) & ~(BY2PG-1);
	d = (t + data + (BY2PG-1)) & ~(BY2PG-1);
	bssend = t + data + bss;
	b = (bssend + (BY2PG-1)) & ~(BY2PG-1);
	if((t|d|b) & KZERO)
		error(Ebadexec);

	/*
	 * Args: pass 1: count
	 */
	nbytes = BY2WD;		/* hole for profiling clock at top of stack */
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
		errors("too many arguments");

	 
	p->seg[ESEG] = newseg(SG_STACK, TSTKTOP-USTKSIZE, USTKSIZE/BY2PG);

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
		if(indir && *argp==0) {
			indir = 0;
			argp = (char**)arg[1];
		}
		*argv++ = charp + (USTKTOP-TSTKTOP);
		n = strlen(*argp) + 1;
		memmove(charp, *argp++, n);
		charp += n;
	}

	memmove(p->text, elem, NAMELEN);

	if(waserror())
		pexit("fatal exec error", 0);

	/*
	 * Committed.  Free old memory. Special segments are maintained accross exec
	 */
	for(i = SSEG; i <= BSEG; i++) {
		putseg(p->seg[i]);
		p->seg[i] = 0;	    /* prevent a second free if we have an error */
	}

	/*
	 * Close on exec
	 */
	f = u->p->fgrp;
	for(i=0; i<=f->maxfd; i++)
		fdclose(i, CCEXEC);

	/* Text.  Shared. Attaches to cache image if possible */
	/* attachimage returns a locked cache image */
	img = attachimage(SG_TEXT|SG_RONLY, tc, UTZERO, (t-UTZERO)>>PGSHIFT);
	ts = img->s;
	p->seg[TSEG] = ts;
	ts->flushme = 1;
	ts->fstart = 0;
	ts->flen = sizeof(Exec)+text;
	unlock(img);

	/* Data. Shared. */
	s = newseg(SG_DATA, t, (d-t)>>PGSHIFT);
	p->seg[DSEG] = s;

	/* Attached by hand */
	incref(img);
	s->image = img;
	s->fstart = ts->fstart+ts->flen;
	s->flen = data;

	/* BSS. Zero fill on demand */
	p->seg[BSEG] = newseg(SG_BSS, d, (b-d)>>PGSHIFT);

	/*
	 * Move the stack
	 */
	s = p->seg[ESEG];
	p->seg[ESEG] = 0;
	p->seg[SSEG] = s;
	s->base = USTKTOP-USTKSIZE;
	s->top = USTKTOP;
	relocateseg(s, TSTKTOP-USTKTOP);

	close(tc);
	poperror();

	/*
	 *  At this point, the mmu contains info about the old address
	 *  space and needs to be flushed
	 */
	flushmmu();
	clearmmucache();
	execpc(entry);
	sp = (ulong*)(USTKTOP - ssize);
	*--sp = nargs;
	((Ureg*)UREGADDR)->usp = (ulong)sp;
	lock(&p->debug);
	u->nnote = 0;
	u->notify = 0;
	u->notified = 0;
	procsetup(p);
	unlock(&p->debug);
	return (USTKTOP-BY2WD);	/* address of user-level clock */
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
	strncpy(u->error, errstrtab[0], ERRLEN);
	return 0;
}

long
sysforkpgrp(ulong *arg)
{
	int mask;
	Pgrp *pg;
	Egrp *eg;

	pg = newpgrp();
	eg = newegrp();
	if(waserror()){
		closepgrp(pg);
		closeegrp(eg);
		nexterror();
	}

	mask = arg[0];
	if(mask == FPall)
		mask = FPnote|FPenv|FPnamespc;

	memmove(pg->user, u->p->pgrp->user, NAMELEN);

	if(mask & FPnamespc)
		pgrpcpy(pg, u->p->pgrp);

	if(mask & FPenv)
		envcpy(eg, u->p->egrp);

	if((mask & FPnote) == 0) {
		u->nnote = 0;
		u->notified = 0;
		memset(u->note, 0, sizeof(u->note));
	}

	poperror();
	closepgrp(u->p->pgrp);
	closeegrp(u->p->egrp);
	u->p->pgrp = pg;
	u->p->egrp = eg;
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

long
syssegbrk(ulong *arg)
{
	Segment *s;
	int i;

	for(i = 0; i < NSEG; i++)
		if(s = u->p->seg[i]) {
			if(arg[0] >= s->base && arg[0] < s->top) {
				switch(s->type&SG_TYPE) {
				case SG_TEXT:
				case SG_DATA:
					errors("bad segment type");
				default:
					return ibrk(arg[1], i);
				}
			}
		}

	errors("not in address space");
}

long
syssegattach(ulong *arg)
{
	return segattach(u->p, arg[0], (char*)arg[1], arg[2], arg[3]);
}

long
syssegdetach(ulong *arg)
{
	int i;
	Segment *s;

	s = 0;
	for(i = 0; i < NSEG; i++)
		if(s = u->p->seg[i]) {
			qlock(&s->lk);
			if((arg[0] >= s->base && arg[0] < s->top) || 
			   (s->top == s->base && arg[0] == s->base))
				goto found;
			qunlock(&s->lk);
		}

	errors("not in address space");

found:
	if((ulong)arg >= s->base && (ulong)arg < s->top) {
		qunlock(&s->lk);
		errors("illegal address");
	}
	u->p->seg[i] = 0;
	qunlock(&s->lk);
	putseg(s);

	/* Ensure we flush any entries from the lost segment */
	flushmmu();
	return 0;
}

long
syssegfree(ulong *arg)
{
	Segment *s;
	ulong from, pages;

	from = PGROUND(arg[0]);
	s = seg(u->p, from, 1);
	if(s == 0)
		errors("not in address space");

	pages = (arg[1]+BY2PG-1)/BY2PG;

	if(from+pages*BY2PG > s->top) {
		qunlock(&s->lk);
		errors("segment too short");
	}

	mfreeseg(s, from, pages);
	qunlock(&s->lk);

	return 0;
}

/* For binary compatability */
long
sysbrk_(ulong *arg)
{
	return ibrk(arg[0], BSEG);
}

long
sysrendezvous(ulong *arg)
{
	Proc *p, *d, **l;
	int s, tag;
	ulong val;

	tag = arg[0];
	l = &REND(u->p->pgrp, tag);

	lock(u->p->pgrp);
	for(p = *l; p; p = p->rendhash) {
		if(p->rendtag == tag) {
			*l = p->rendhash;
			val = p->rendval;
			p->rendval = arg[1];

			if(p->state != Rendezvous)
				panic("rendezvous");
			ready(p);
			unlock(u->p->pgrp);
			return val;	
		}
		l = &p->rendhash;
	}

	/* Going to sleep here */
	p = u->p;
	p->rendtag = tag;
	p->rendval = arg[1];
	p->rendhash = *l;
	*l = p;

	unlock(p->pgrp);

	s = splhi();
	u->p->state = Rendezvous;
	sched();
	splx(s);

	return u->p->rendval;
}
