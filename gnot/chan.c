#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"


struct{
	Lock;
	Chan	*free;
}chanalloc;

int
incref(Ref *r)
{
	int x;

	lock(r);
	x = ++r->ref;
	unlock(r);
	return x;
}

int
decref(Ref *r)
{
	int x;

	lock(r);
	x = --r->ref;
	unlock(r);
	return x;
}

void
chaninit(void)
{
	int i;
	Chan *c;

	chanalloc.free = ialloc(conf.nchan*sizeof(Chan), 0);

	c = chanalloc.free;
	for(i=0; i<conf.nchan-1; i++,c++){
		c->fid = i;
		c->next = c+1;
	}
	c->next = 0;
}

void
chandevreset(void)
{
	int i;

	for(i=0; i<strlen(devchar); i++)
		(*devtab[i].reset)();
}

void
chandevinit(void)
{
	int i;

	for(i=0; i<strlen(devchar); i++)
		(*devtab[i].init)();
}

Chan*
newchan(void)
{
	Chan *c;

loop:
	lock(&chanalloc);
	if(c = chanalloc.free){		/* assign = */
		chanalloc.free = c->next;
		c->type = 0;	/* if closed before changed, this calls rooterror, a nop */
		c->flag = 0;
		c->ref = 1;
		unlock(&chanalloc);
		c->dev = 0;
		c->offset = 0;
		c->mnt = 0;
		c->stream = 0;
		c->mchan = 0;
		c->mqid = 0;
		return c;
	}
	unlock(&chanalloc);
	print("no chans\n");
	if(u == 0)
		panic("newchan");
	u->p->state = Wakeme;
	alarm(1000, wakeme, u->p);
	sched();
	goto loop;
}

void
close(Chan *c)
{
	if(c->flag & CFREE)
		panic("close");
	if(decref(c) == 0){
		if(!waserror()){
			(*devtab[c->type].close)(c);
			poperror();
		}
		c->flag = CFREE;
		lock(&chanalloc);
		c->next = chanalloc.free;
		chanalloc.free = c;
		unlock(&chanalloc);
	}
}

int
eqchan(Chan *a, Chan *b, long qmask)
{
	if((a->qid^b->qid) & qmask)
		return 0;
	if(a->type != b->type)
		return 0;
	if(a->dev != b->dev)
		return 0;
	return 1;
}

/*
 * omnt is locked.  return with nmnt locked.
 */
Mount*
mountsplit(Mount *omnt)
{
	Mount *nmnt;

	nmnt = newmount();
	lock(nmnt);
	nmnt->term = omnt->term;
	nmnt->mountid = omnt->mountid;
	nmnt->next = omnt->next;
	if(nmnt->next)
		incref(nmnt->next);
	nmnt->c = omnt->c;
	incref(nmnt->c);
	omnt->ref--;
	unlock(omnt);
	return nmnt;
}

int
mount(Chan *new, Chan *old, int flag)
{
	int i;
	Mtab *mt, *mz;
	Mount *mnt, *omnt, *nmnt, *pmnt;
	Pgrp *pg;
	int islast;

	if(CHDIR & (old->qid^new->qid))
		error(0, Emount);
	if((old->qid&CHDIR)==0 && (flag&MORDER)!=MREPL)
		error(0, Emount);

	mz = 0;
	islast = 0;
	mnt = 0;
	pg = u->p->pgrp;
	lock(pg);
	if(waserror()){
		if(mnt){
			mnt->c = 0;	/* caller will close new */
			closemount(mnt);
		}
		unlock(pg);
		nexterror();
	}
	/*
	 * Is old already in mount table?
	 */
	mt = pg->mtab;
	for(i=0; i<pg->nmtab; i++,mt++){
		if(mt->c==0 && mz==0)
			mz = mt;
		else if(eqchan(mt->c, old, CHDIR|QPATH)){
			mz = 0;
			goto Found;
		}
	}
	if(mz == 0){
		if(i == conf.nmtab)
			error(0, Enomount);
		mz = &pg->mtab[i];
		islast++;
	}
	mz->mnt = 0;
	mt = mz;

    Found:
	new->flag = CMOUNT;
	if(flag & MCREATE)
		new->flag |= CCREATE;
	mnt = newmount();
	mnt->c = new;

	switch(flag & MORDER){
	/*
	 * These two always go at head of list
	 */
	case MBEFORE:
		if(mt->mnt == 0)
			error(0, Enotunion);
		/* fall through */

	case MREPL:
		mnt->next = mt->mnt;
		mt->mnt = mnt;
		if((flag&MORDER) == MBEFORE)
			mnt->term = 0;
		else
			mnt->term = 1;
		break;

	/*
	 * This one never goes at head of list
	 */
	case MAFTER:
		if(mt->mnt == 0)
			error(0, Enotunion);
		omnt = mt->mnt;
		pmnt = 0;
		while(!omnt->term){
			lock(omnt);
			if(omnt->ref > 1){
				omnt = mountsplit(omnt);
				if(pmnt)
					pmnt->next = omnt;
				else
					mt->mnt = omnt;
			}
			unlock(omnt);
			nmnt = omnt->next;
			if(nmnt == 0)
				panic("MAFTER term");
			pmnt = omnt;
			omnt = nmnt;
		}
		mnt->next = omnt->next;
		omnt->next = mnt;
		mnt->term = 1;
		omnt->term = 0;
		break;
	}

	incref(new);
	if(mz){
		mz->c = old;
		incref(old);
	}
	if(islast)
		pg->nmtab++;
	unlock(pg);
	poperror();
	return mnt->mountid;
}

Chan*
clone(Chan *c, Chan *nc)
{
	return (*devtab[c->type].clone)(c, nc);
}

Chan*
domount(Chan *c)
{
	int i;
	ulong mntid;
	Mtab *mt;
	Mount *mnt;
	Pgrp *pg;
	Chan *nc, *mc;

	pg = u->p->pgrp;
	/*
	 * Is c in in mount table?
	 */
	mt = pg->mtab;
	for(i=0; i<pg->nmtab; i++,mt++)
		if(mt->c && eqchan(mt->c, c, CHDIR|QPATH))
			goto Found;
	/*
	 * No; c is unaffected
	 */
	return c;

	/*
	 * Yes; move c through table
	 */
    Found:
	lock(pg);
	if(!eqchan(mt->c, c, CHDIR|QPATH)){	/* table changed underfoot */
		pprint("domount: changed underfoot?\n");
		unlock(pg);
		return c;
	}
	mnt = mt->mnt;
	mntid = mnt->mountid;
	mc = mnt->c;
	incref(mc);
	unlock(pg);
	if(waserror()){
		close(mc);
		nexterror();
	}
	nc = clone(mc, 0);
	close(mc);
	poperror();
	close(c);
	nc->mnt = mnt;
	nc->mountid = mntid;
	return nc;
}

Chan*
walk(Chan *ac, char *name, int domnt)
{
	Mount *mnt;
	int first = 1;
	Chan *c = ac;
	Chan *nc, *mc;
	Pgrp *pg = u->p->pgrp;

	/*
	 * name may be empty if the file name is "/", "#c" etc.
	 */
    Again:
	if(name[0] && (*devtab[c->type].walk)(c, name)==0){
		if(!(c->flag&CMOUNT))
			goto Notfound;
		mnt = c->mnt;
		if(mnt == 0)
			panic("walk");
		lock(pg);
		if(mnt->term){
			unlock(pg);
			goto Notfound;
		}
		if(c->mountid != mnt->mountid){
			pprint("walk: changed underfoot? '%s'\n", name);
			unlock(pg);
			goto Notfound;
		}
		mnt = mnt->next;
		mc = mnt->c;
		incref(mc);
		unlock(pg);
		if(waserror()){
			close(mc);
			nexterror();
		}
		if(mnt == 0)
			panic("walk 1");
		nc = clone(mc, 0);
		close(mc);
		poperror();
		if(!first)
			close(c);
		nc->mnt = mnt;
		nc->mountid = mnt->mountid;
		c = nc;
		first = 0;
		goto Again;
	}

	if(name[0])			/* walk succeeded */
		c->flag &= ~CMOUNT;

	if(domnt){
		if(waserror()){
			if(!first)
				close(c);
			return 0;
		}
		c = domount(c);
		poperror();
	}

	if(!first)
		close(ac);


	return c;

    Notfound:
	if(!first)
		close(c);
	return 0;
}

/*
 * c is a mounted non-creatable directory.  find a creatable one.
 */
Chan*
createdir(Chan *c)
{
	Mount *mnt;
	Pgrp *pg = u->p->pgrp;
	Chan *mc, *nc;

	lock(pg);
	if(waserror()){
		unlock(pg);
		nexterror();
	}
	mnt = c->mnt;
	if(c->mountid != mnt->mountid){
		pprint("createdir: changed underfoot?\n");
		error(0, Enocreate);
	}
	do{
		if(mnt->term)
			error(0, Enocreate);
		mnt = mnt->next;
	}while(!(mnt->c->flag&CCREATE));
	mc = mnt->c;
	incref(mc);
	unlock(pg);
	if(waserror()){
		close(mc);
		nexterror();
	}
	nc = clone(mc, 0);
	poperror();
	close(c);
	close(mc);
	nc->mnt = mnt;
	return nc;
}

void
saveregisters(void)
{
}

/*
 * Turn a name into a channel.
 * &name[0] is known to be a valid address.  It may be a kernel address.
 */
Chan*
namec(char *name, int amode, int omode, ulong perm)
{
	Chan *c, *nc;
	int t;
	int mntok, isdot;
	char *p;
	char *elem;

	if(name[0] == 0)
		error(0, Enonexist);

	/*
	 * Make sure all of name is o.k.  first byte is validated
	 * externally so if it's a kernel address we know it's o.k.
	 */
	if(!((ulong)name & KZERO)){
		p = name;
		t = BY2PG-((ulong)p&(BY2PG-1));
		while(vmemchr(p, 0, t) == 0){
			p += t;
			t = BY2PG;
		}
	}

	elem = u->elem;
	mntok = 1;
	isdot = 0;
	if(name[0] == '/'){
		c = clone(u->slash, 0);
		/*
		 * Skip leading slashes.
		 */
		name = skipslash(name);
	}else if(name[0] == '#'){
		mntok = 0;
		if(name[1]=='M')
			error(0, Enonexist);
		t = devno(name[1], 1);
		if(t == -1)
			error(0, Ebadsharp);
		name += 2;
		if(*name == '/'){
			name = skipslash(name);
			elem[0]=0;
		}else
			name = nextelem(name, elem);
		c = (*devtab[t].attach)(elem);
	}else{
		c = clone(u->dot, 0);
		name = skipslash(name);	/* eat leading ./ */
		if(*name == 0)
			isdot = 1;
	}

	if(waserror()){
		close(c);
		nexterror();
	}

	name = nextelem(name, elem);

	if(mntok && !isdot)
	if(!(amode==Amount && elem[0]==0))	/* don't domount on slash */
		c = domount(c);			/* see case Atodir below */

	/*
	 * How to treat the last element of the name depends on the operation.
	 * Therefore do all but the last element by the easy algorithm.
	 */
	while(*name){
		if((nc=walk(c, elem, mntok)) == 0)
			error(0, Enonexist);
		c = nc;
		name = nextelem(name, elem);
	}

	/*
	 * Last element; act according to type of access.
	 */

	switch(amode){
	case Aaccess:
		if(isdot)
			c = domount(c);
		else{
			if((nc=walk(c, elem, mntok)) == 0)
				error(0, Enonexist);
			c = nc;
		}
		break;

	case Atodir:
		/*
		 * Directories (e.g. for cd) are left before the mount point,
		 * so one may mount on / or . and see the effect.
		 */
		if((nc=walk(c, elem, 0)) == 0)
			error(0, Enonexist);
		c = nc;
		if(!(c->qid & CHDIR))
			error(0, Enotdir);
		break;

	case Aopen:
		if(isdot)
			c = domount(c);
		else{
			if((nc=walk(c, elem, mntok)) == 0)
				error(0, Enonexist);
			c = nc;
		}
	Open:
		saveregisters();	/* else error() in open has wrong value of c saved */
		c = (*devtab[c->type].open)(c, omode);
		if(omode & OCEXEC)
			c->flag |= CCEXEC;
		break;

	case Amount:
		/*
		 * When mounting on an already mounted upon directory, one wants
		 * the second mount to be attached to the original directory, not
		 * the replacement.
		 */
		if((nc=walk(c, elem, 0)) == 0)
			error(0, Enonexist);
		c = nc;
		break;

	case Acreate:
		if(isdot)
			error(0, Eisdir);
		if((nc=walk(c, elem, 1)) != 0){
			c = nc;
			omode |= OTRUNC;
			goto Open;
		}
		if((c->flag&(CMOUNT|CCREATE)) == CMOUNT)
			c = createdir(c);
		(*devtab[c->type].create)(c, elem, omode, perm);
		if(omode & OCEXEC)
			c->flag |= CCEXEC;
		break;

	default:
		panic("unknown namec access %d\n", amode);
	}
	poperror();
	return c;
}

/*
 * name[0] is addressable.
 */
char*
skipslash(char *name)
{
    Again:
	while(*name == '/')
		name++;
	if(*name=='.' && (name[1]==0 || name[1]=='/')){
		name++;
		goto Again;
	}
	return name;
}

char isfrog[]={
	/*NUL*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*BKS*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*DLE*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*CAN*/	1, 1, 1, 1, 1, 1, 1, 1,
	[' ']	1,
	['/']	1,
	[0x7f]	1,
};

/*
 * name[0] should not be a slash.
 * Advance name to next element in path, copying current element into elem.
 * Return pointer to next element, skipping slashes.
 */
char*
nextelem(char *name, char *elem)
{
	int i, user, c;
	char *end, *e;

	if(*name == '/')
		error(0, Efilename);
	end = memchr(name, 0, NAMELEN);
	if(end == 0){
		end = memchr(name, '/', NAMELEN);
		if(end == 0)
			error(0, Efilename);
	}else{
		e = memchr(name, '/', end-name);
		if(e)
			end = e;
	}
	while(name < end){
		c = *name++;
		if((c&0x80) || isfrog[c])
			error(0, Ebadchar);
		*elem++ = c;
	}
	*elem = 0;
	return skipslash(name);
}

void
isdir(Chan *c)
{
	if(c->qid & CHDIR)
		return;
	error(0, Enotdir);
}
