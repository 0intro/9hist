#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"fcall.h"

/*
 * The sys*() routines needn't poperror() as they return directly to syscall().
 */

int
newfd(void)
{
	int i;

	for(i=0; i<NFD; i++)
		if(u->fd[i] == 0){
			if(i > u->maxfd)
				u->maxfd = i;
			return i;
		}
	error(Enofd);
}

Chan*
fdtochan(int fd, int mode)
{
	Chan *c;

	c = 0;		/* set */
	if(fd<0 || NFD<=fd || (c=u->fd[fd])==0)
		error(Ebadfd);
	if(mode<0 || c->mode==ORDWR)
		return c;
	if((mode&OTRUNC) && c->mode==OREAD)
    err:
		error(Ebadusefd);
	if((mode&~OTRUNC) != c->mode)
		goto err;
	return c;
}

int
openmode(ulong o)
{
	if(o >= (OTRUNC|OCEXEC|ORCLOSE|OEXEC))
    Err:
		error(Ebadarg);
	o &= ~(OTRUNC|OCEXEC|ORCLOSE);
	if(o > OEXEC)
		goto Err;
	if(o == OEXEC)
		return OREAD;
	return o;
}

long
syspipe(ulong *arg)
{
	int fd[2];
	Chan *c[2];
	Dev *d;

	validaddr(arg[0], 2*BY2WD, 1);
	evenaddr(arg[0]);
	d = &devtab[devno('|', 0)];
	c[0] = (*d->attach)(0);
	c[1] = 0;
	fd[0] = -1;
	fd[1] = -1;
	if(waserror()){
		close(c[0]);
		if(c[1])
			close(c[1]);
		if(fd[0] >= 0)
			u->fd[fd[0]]=0;
		if(fd[1] >= 0)
			u->fd[fd[1]]=0;
		nexterror();
	}
	c[1] = (*d->clone)(c[0], 0);
	(*d->walk)(c[0], "data");
	(*d->walk)(c[1], "data1");
	c[0] = (*d->open)(c[0], ORDWR);
	c[1] = (*d->open)(c[1], ORDWR);
	fd[0] = newfd();
	u->fd[fd[0]] = c[0];
	fd[1] = newfd();
	u->fd[fd[1]] = c[1];
	((long*)arg[0])[0] = fd[0];
	((long*)arg[0])[1] = fd[1];
	poperror();
	return 0;
}

long
sysdup(ulong *arg)
{
	int fd;
	Chan *c, *oc;

	/*
	 * Close after dup'ing, so date > #d/1 works
	 */
	c = fdtochan(arg[0], -1);
	fd = arg[1];
	if(fd != -1){
		if(fd<0 || NFD<=fd)
			error(Ebadfd);
		if(fd > u->maxfd)
			u->maxfd = fd;
		oc = u->fd[fd];
	}else{
		oc = 0;
		fd = newfd();
	}
	u->fd[fd] = c;
	incref(c);
	if(oc)
		close(oc);
	return fd;
}

long
sysopen(ulong *arg)
{
	int fd;
	Chan *c;

	openmode(arg[1]);	/* error check only */
	fd = newfd();
	validaddr(arg[0], 1, 0);
	c = namec((char*)arg[0], Aopen, arg[1], 0);
	u->fd[fd] = c;
	return fd;
}

void
fdclose(int fd)
{
	int i;

	u->fd[fd] = 0;
	if(fd == u->maxfd)
		for(i=fd; --i>=0 && u->fd[i]==0; )
			u->maxfd = i;
}

long
sysclose(ulong *arg)
{
	Chan *c;

	c = fdtochan(arg[0], -1);
	close(c);
	fdclose(arg[0]);
	return 0;
}

long
unionread(Chan *c, void *va, long n)
{
	Mount *mnt;
	Chan *mc, *nc;
	Pgrp *pg = u->p->pgrp;
	long nr;

	mnt = c->mnt;
	lock(pg);
	if(c->mountid != mnt->mountid){
		pprint("unionread: changed underfoot?\n");
		unlock(pg);
		return 0;
	}
    Again:
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
	if(waserror()){
		close(nc);
		nexterror();
	}
	nc = (*devtab[nc->type].open)(nc, OREAD);
	nc->offset = c->offset;
	nr = (*devtab[nc->type].read)(nc, va, n);
	c->offset = nc->offset;		/* devdirread e.g. changes it */
	close(nc);
	poperror();
	if(nr > 0)
		return nr;
	/*
	 * Advance to next element
	 */
	lock(pg);
	mnt = c->mnt;
	if(c->mountid != mnt->mountid){
		print("unionread: changed underfoot?\n");
		unlock(pg);
		return 0;
	}
	if(mnt->term){
		unlock(pg);
		return 0;
	}
	mnt = mnt->next;
	c->mnt = mnt;
	c->mountid = mnt->mountid;
	c->offset = 0;
	goto Again;
}

long
sysread(ulong *arg)
{
	Chan *c;
	long n;

	c = fdtochan(arg[0], OREAD);
	validaddr(arg[1], arg[2], 1);
	qlock(c);
	if(waserror()){
		qunlock(c);
		nexterror();
	}
	n = arg[2];
	if(c->qid.path & CHDIR){
		n -= n%DIRLEN;
		if(c->offset%DIRLEN || n==0)
			error(Ebaddirread);
	}
	if((c->qid.path&CHDIR) && (c->flag&CMOUNT))
		n = unionread(c, (void*)arg[1], n);
	else
		n = (*devtab[c->type].read)(c, (void*)arg[1], n);
	c->offset += n;
	qunlock(c);
	return n;
}

long
syswrite(ulong *arg)
{
	Chan *c;
	long n;

	c = fdtochan(arg[0], OWRITE);
	validaddr(arg[1], arg[2], 0);
	qlock(c);
	if(waserror()){
		qunlock(c);
		nexterror();
	}
	if(c->qid.path & CHDIR)
		error(Eisdir);
	n = (*devtab[c->type].write)(c, (void*)arg[1], arg[2]);
	c->offset += n;
	qunlock(c);
	return n;
}

long
sysseek(ulong *arg)
{
	Chan *c;
	char buf[DIRLEN];
	Dir dir;
	long off;

	c = fdtochan(arg[0], -1);
	if(c->qid.path & CHDIR)
		error(Eisdir);
	if(devchar[c->type] == '|')
		error(Eisstream);
	qlock(c);
	if(waserror()){
		qunlock(c);
		nexterror();
	}
	switch(arg[2]){
	case 0:
		c->offset = arg[1];
		break;

	case 1:
		c->offset += (long)arg[1];
		break;

	case 2:
		(*devtab[c->type].stat)(c, buf);
		convM2D(buf, &dir);
		c->offset = dir.length + (long)arg[1];
		break;
	}
	off = c->offset;
	qunlock(c);
	poperror();
	return off;
}

long
sysfstat(ulong *arg)
{
	Chan *c;
	long n;

	validaddr(arg[1], DIRLEN, 1);
	evenaddr(arg[1]);
	c = fdtochan(arg[0], -1);
	(*devtab[c->type].stat)(c, (char*)arg[1]);
	return 0;
}

long
sysstat(ulong *arg)
{
	Chan *c;
	long n;

	validaddr(arg[1], DIRLEN, 1);
	evenaddr(arg[1]);
	validaddr(arg[0], 1, 0);
	c = namec((char*)arg[0], Aaccess, 0, 0);
	if(waserror()){
		close(c);
		nexterror();
	}
	(*devtab[c->type].stat)(c, (char*)arg[1]);
	poperror();
	close(c);
	return 0;
}

long
syschdir(ulong *arg)
{
	Chan *c;

	validaddr(arg[0], 1, 0);
	c = namec((char*)arg[0], Atodir, 0, 0);
	close(u->dot);
	u->dot = c;
	return 0;
}

long
bindmount(ulong *arg, int ismount)
{
	Chan *c0, *c1;
	ulong flag;
	long ret;
	char *p;
	int t;
	struct{
		Chan	*chan;
		char	*spec;
		char	*auth;
	}bogus;

	flag = arg[2];
	if(flag>MMASK || (flag&MORDER)==(MBEFORE|MAFTER))
		error(Ebadarg);
	if(ismount){
		bogus.chan = fdtochan(arg[0], 2);
		validaddr(arg[3], 1, 0);
		p = (char*)arg[3];
		t = BY2PG-((ulong)p&(BY2PG-1));
		while(vmemchr(p, 0, t) == 0){
			p += t;
			t = BY2PG;
		}
		bogus.spec = (char*)arg[3];
		validaddr(arg[4], 1, 0);
		p = (char*)arg[4];
		t = BY2PG-((ulong)p&(BY2PG-1));
		while(vmemchr(p, 0, t) == 0){
			p += t;
			t = BY2PG;
		}
		bogus.auth = (char*)arg[4];
		ret = devno('M', 0);
		c0 = (*devtab[ret].attach)((char*)&bogus);
	}else{
		validaddr(arg[0], 1, 0);
		c0 = namec((char*)arg[0], Aaccess, 0, 0);
	}
	if(waserror()){
		close(c0);
		nexterror();
	}
	validaddr(arg[1], 1, 0);
	c1 = namec((char*)arg[1], Amount, 0, 0);
	if(waserror()){
		close(c1);
		nexterror();
	}
	if((c0->qid.path^c1->qid.path) & CHDIR)
		error(Ebadmount);
	if(flag && !(c0->qid.path&CHDIR))
		error(Ebadmount);
	ret = mount(c0, c1, flag);
	close(c0);
	close(c1);
	if(ismount){
		close(bogus.chan);
		fdclose(arg[0]);
	}
	return ret;
}

long
sysbind(ulong *arg)
{
	return bindmount(arg, 0);
}

long
sysmount(ulong *arg)
{
	return bindmount(arg, 1);
}

long
syscreate(ulong *arg)
{
	int fd;
	Chan *c;

	openmode(arg[1]);	/* error check only */
	fd = newfd();
	validaddr(arg[0], 1, 0);
	c = namec((char*)arg[0], Acreate, arg[1], arg[2]);
	u->fd[fd] = c;
	return fd;
}

long
sysremove(ulong *arg)
{
	Chan *c;

	validaddr(arg[0], 1, 0);
	c = namec((char*)arg[0], Aaccess, 0, 0);
	if(waserror()){
		c->type = 0;	/* see below */
		close(c);
		nexterror();
	}
	(*devtab[c->type].remove)(c);
	/*
	 * Remove clunks the fid, but we need to recover the Chan
	 * so fake it up.  rootclose() is known to be a nop.
	 */
	c->type = 0;
	close(c);
	return 0;
}

long
syswstat(ulong *arg)
{
	Chan *c;
	long n;

	validaddr(arg[1], DIRLEN, 0);
	evenaddr(arg[1]);
	validaddr(arg[0], 1, 0);
	c = namec((char*)arg[0], Aaccess, 0, 0);
	if(waserror()){
		close(c);
		nexterror();
	}
	(*devtab[c->type].wstat)(c, (char*)arg[1]);
	poperror();
	close(c);
	return 0;
}

long
sysfwstat(ulong *arg)
{
	Chan *c;
	long n;

	validaddr(arg[1], DIRLEN, 0);
	evenaddr(arg[1]);
	c = fdtochan(arg[0], -1);
	(*devtab[c->type].wstat)(c, (char*)arg[1]);
	return 0;
}

#ifdef asdf
long
sysfilsys(ulong *arg)
{
	Chan *cin, *cout;

	cin = fdtochan(arg[0], OREAD);
	cout = fdtochan(arg[1], OWRITE);
	validaddr(arg[2], 1, 0);
	if((cin->qid.path&CHDIR) || (cout->qid.path&CHDIR))
		error(Ebadarg);
	service((char *)arg[2], cin, cout, filsys);
	return 0;
}
#endif
