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

/*
 *  used by namec, domount, and walk to point
 *  to the current mount entry during name resolution
 */
typedef struct Finger	Finger;
struct Finger {
	Chan	*c;		/* channel we're walking */
	int	needcl;		/* true if we need to clone c before using it */
	Mount	*mnt;		/* last mount point we ran traversed */
	ulong	mountid;	/* id of that mount point */
};

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
		c->aux = 0;
		c->mchan = 0;
		c->mqid = (Qid){0, 0};
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
freechan(Chan *c)
{
	if(decref(c) == 0){
		c->flag = CFREE;
		lock(&chanalloc);
		c->next = chanalloc.free;
		chanalloc.free = c;
		unlock(&chanalloc);
	}
}

void
close(Chan *c)
{
	if(c->flag & CFREE)
		panic("close");
	if(decref(c) == 0){
		if(!waserror()) {
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
eqqid(Qid a, Qid b)
{
	return a.path==b.path && a.vers==b.vers;
}

int
eqchan(Chan *a, Chan *b, int pathonly)
{
	if(a->qid.path != b->qid.path)
		return 0;
	if(!pathonly && a->qid.vers!=b->qid.vers)
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

	if(CHDIR & (old->qid.path^new->qid.path))
		error(Emount);
	if((old->qid.path&CHDIR)==0 && (flag&MORDER)!=MREPL)
		error(Emount);

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
		else if(eqchan(mt->c, old, 1)){
			mz = 0;
			goto Found;
		}
	}
	if(mz == 0){
		if(i == conf.nmtab)
			error(Enomount);
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
			error(Enotunion);
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
			error(Enotunion);
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

void
domount(Finger *f)
{
	int i;
	Mtab *mt;
	Pgrp *pg;
	Chan *mc;

	pg = u->p->pgrp;
	/*
	 * Is c in in mount table?
	 */
	mt = pg->mtab;
	for(i=0; i<pg->nmtab; i++,mt++)
		if(mt->c && eqchan(mt->c, f->c, 1))
			goto Found;
	/*
	 * No; c is unaffected
	 */
	return;

	/*
	 * Yes; move c through table
	 */
    Found:
	lock(pg);
	if(!eqchan(mt->c, f->c, 1)){	/* table changed underfoot */
		pprint("domount: changed underfoot?\n");
		unlock(pg);
		return;
	}
	f->mnt = mt->mnt;
	f->mountid = f->mnt->mountid;
	mc = mt->mnt->c;
	incref(mc);
	unlock(pg);
	close(f->c);
	f->c = mc;
	f->needcl = 1;
}

int
walk(Finger *f, char *name, int domnt)
{
	int first = 1;
	Mount *mnt = f->mnt;
	ulong mountid = f->mountid;
	Chan *c = f->c;
	Chan *mc;
	Pgrp *pg = u->p->pgrp;

	/*
	 * name may be empty if the file name is "/", "#c" etc.
	 */
	if(name[0]){
		for(;;){
			if(!first || f->needcl){
				mc = (*devtab[c->type].clwalk)(c, name);
				if(mc){
					close(c);
					c = mc;
					break;
				}
			} else {
				if((*devtab[c->type].walk)(c, name) != 0){
					break;
				}
			}
			if(!(c->flag&CMOUNT))
				goto Notfound;
			if(mnt == 0)
				panic("walk");	/* ??? is this safe ??? */
			lock(pg);
			if(mnt->term){
				unlock(pg);
				goto Notfound;
			}
			if(mountid != mnt->mountid){
				pprint("walk: changed underfoot? '%s'\n", name);
				unlock(pg);
				goto Notfound;
			}
			mnt = mnt->next;
			mountid = mnt->mountid;
			mc = mnt->c;
			incref(mc);
			unlock(pg);
			if(!first)
				close(c);
			c = mc;
			first = 0;
		}

		/*
		 *  we get here only if we have a cloned and walked
		 *  channel
		 */
		if(!first)
			close(f->c);
		f->c = c;
		f->needcl = 0;
		f->mnt = 0;
		c->flag &= ~CMOUNT;
	}

	if(domnt)
		domount(f);

	return 1;

    Notfound:
	if(!first)
		close(c);
	return 0;
}

/*
 * f->c is a mounted non-creatable directory.  find a creatable one.
 */
void
createdir(Finger *f)
{
	Mount *mnt;
	Pgrp *pg = u->p->pgrp;
	Chan *mc;

	lock(pg);
	if(waserror()){
		unlock(pg);
		nexterror();
	}
	mnt = f->mnt;
	if(f->mountid != mnt->mountid){
		pprint("createdir: changed underfoot?\n");
		error(Enocreate);
	}
	do{
		if(mnt->term)
			error(Enocreate);
		mnt = mnt->next;
	}while(!(mnt->c->flag&CCREATE));
	mc = mnt->c;
	incref(mc);
	unlock(pg);
	poperror();
	close(f->c);
	f->needcl = 1;
	f->c = mc;
}

void
saveregisters(void)
{
}

void
doclone(Finger *f)
{
	Chan *nc;

	if(f->needcl == 0)
		return;
	nc = clone(f->c, 0);
	close(f->c);
	f->c = nc;
	f->needcl = 0;
}

/*
 * Turn a name into a channel.
 * &name[0] is known to be a valid address.  It may be a kernel address.
 */
Chan*
namec(char *name, int amode, int omode, ulong perm)
{
	int t;
	int mntok, isdot;
	char *p;
	char *elem;
	Finger f;

	if(name[0] == 0)
		error(Enonexist);

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
	f.mnt = 0;
	if(name[0] == '/'){
		f.c = u->slash;
		incref(f.c);
		f.needcl = 1;
		/*
		 * Skip leading slashes.
		 */
		name = skipslash(name);
	}else if(name[0] == '#'){
		mntok = 0;
		if(name[1]=='M')
			error(Enonexist);
		t = devno(name[1], 1);
		if(t == -1)
			error(Ebadsharp);
		name += 2;
		if(*name == '/'){
			name = skipslash(name);
			elem[0]=0;
		}else
			name = nextelem(name, elem);
		f.c = (*devtab[t].attach)(elem);
		f.needcl = 0;
	}else{
		f.c = u->dot;
		incref(f.c);
		f.needcl = 1;
		name = skipslash(name);	/* eat leading ./ */
		if(*name == 0)
			isdot = 1;
	}

	if(waserror()){
		close(f.c);
		nexterror();
	}

	name = nextelem(name, elem);

	/*
	 *  If mounting, don't follow the mount entry for root or the
	 *  current directory.
	 */
	if(mntok && !isdot && !(amode==Amount && elem[0]==0))
		domount(&f);		/* see case Atodir below */

	/*
	 * How to treat the last element of the name depends on the operation.
	 * Therefore do all but the last element by the easy algorithm.
	 */
	while(*name){
		if(walk(&f, elem, mntok) == 0)
			error(Enonexist);
		name = nextelem(name, elem);
	}

	/*
	 * Last element; act according to type of access.
	 */
	switch(amode){
	case Aaccess:
		if(isdot)
			domount(&f);
		else{
			if(walk(&f, elem, mntok) == 0)
				error(Enonexist);
		}
		break;

	case Atodir:
		/*
		 * Directories (e.g. for cd) are left before the mount point,
		 * so one may mount on / or . and see the effect.
		 */
		if(walk(&f, elem, 0) == 0)
			error(Enonexist);
		if(!(f.c->qid.path & CHDIR))
			error(Enotdir);
		break;

	case Aopen:
		if(isdot)
			domount(&f);
		else{
			if(walk(&f, elem, mntok) == 0)
				error(Enonexist);
		}
	Open:
		saveregisters(); /* else error() in open has wrong value of c saved */
		if(f.needcl)
			doclone(&f);
		f.c = (*devtab[f.c->type].open)(f.c, omode);
		if(omode & OCEXEC)
			f.c->flag |= CCEXEC;
		break;

	case Amount:
		/*
		 * When mounting on an already mounted upon directory, one wants
		 * the second mount to be attached to the original directory, not
		 * the replacement.
		 */
		if(walk(&f, elem, 0) == 0)
			error(Enonexist);
		break;

	case Acreate:
		if(isdot)
			error(Eisdir);
		if(walk(&f, elem, 1) != 0){
			omode |= OTRUNC;
			goto Open;
		}
		if((f.c->flag&(CMOUNT|CCREATE)) == CMOUNT)
			createdir(&f);
		if(f.needcl)
			doclone(&f);
		(*devtab[f.c->type].create)(f.c, elem, omode, perm);
		if(omode & OCEXEC)
			f.c->flag |= CCEXEC;
		break;

	default:
		panic("unknown namec access %d\n", amode);
	}
	if(f.needcl)
		doclone(&f);
	poperror();
	if(f.c->flag & CMOUNT){
		f.c->mnt = f.mnt;
		f.c->mountid = f.mountid;
	}
	return f.c;
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
		error(Efilename);
	end = memchr(name, 0, NAMELEN);
	if(end == 0){
		end = memchr(name, '/', NAMELEN);
		if(end == 0)
			error(Efilename);
	}else{
		e = memchr(name, '/', end-name);
		if(e)
			end = e;
	}
	while(name < end){
		c = *name++;
		if((c&0x80) || isfrog[c])
			error(Ebadchar);
		*elem++ = c;
	}
	*elem = 0;
	return skipslash(name);
}

void
isdir(Chan *c)
{
	if(c->qid.path & CHDIR)
		return;
	error(Enotdir);
}
