#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * The sys*() routines needn't poperror() as they return directly to syscall().
 */

int
newfd(Chan *c)
{
	int i;
	Fgrp *f = up->fgrp;

	lock(f);
	for(i=0; i<NFD; i++)
		if(f->fd[i] == 0){
			if(i > f->maxfd)
				f->maxfd = i;
			f->fd[i] = c;
			unlock(f);
			return i;
		}
	unlock(f);
	exhausted("file descriptors");
	return 0;
}

Chan*
fdtochan(int fd, int mode, int chkmnt, int iref)
{
	Chan *c;
	Fgrp *f;

	c = 0;
	f = up->fgrp;

	lock(f);
	if(fd<0 || NFD<=fd || (c = f->fd[fd])==0) {
		unlock(f);
		error(Ebadfd);
	}
	if(iref)
		incref(c);
	unlock(f);

	if(chkmnt && (c->flag&CMSG)) {
		if(iref)
			cclose(c);
		error(Ebadusefd);
	}

	if(mode<0 || c->mode==ORDWR)
		return c;

	if((mode&OTRUNC) && c->mode==OREAD) {
		if(iref)
			cclose(c);
		error(Ebadusefd);
	}

	if((mode&~OTRUNC) != c->mode) {
		if(iref)
			cclose(c);
		error(Ebadusefd);
	}

	return c;
}

int
openmode(ulong o)
{
	if(o >= (OTRUNC|OCEXEC|ORCLOSE|OEXEC))
		error(Ebadarg);
	o &= ~(OTRUNC|OCEXEC|ORCLOSE);
	if(o > OEXEC)
		error(Ebadarg);
	if(o == OEXEC)
		return OREAD;
	return o;
}

long
sysfd2path(ulong *arg)
{
	Chan *c;

	validaddr(arg[1], 1, 0);
	if(vmemchr((char*)arg[1], '\0', arg[2]) == 0)
		error(Ebadarg);

	c = fdtochan(arg[0], -1, 0, 0);
	/* If we used open the chan will be at the first element
	 * of a union rather than the mhead of the union. undomount
	 * will make it look like we used Atodir rather than Aopen.
	 */
	if(c->qid.path & CHDIR)
		c = undomount(c);
	ptpath(c->path, (char*)arg[1], arg[2]);
	return 0;
}

long
syspipe(ulong *arg)
{
	int fd[2];
	Chan *c[2];
	Dev *d;
	Fgrp *f = up->fgrp;

	validaddr(arg[0], 2*BY2WD, 1);
	evenaddr(arg[0]);
	d = devtab[devno('|', 0)];
	c[0] = namec("#|", Atodir, 0, 0);
	c[1] = 0;
	fd[0] = -1;
	fd[1] = -1;
	if(waserror()){
		cclose(c[0]);
		if(c[1])
			cclose(c[1]);
		if(fd[0] >= 0)
			f->fd[fd[0]]=0;
		if(fd[1] >= 0)
			f->fd[fd[1]]=0;
		nexterror();
	}
	c[1] = cclone(c[0], 0);
	walk(c[0], "data", 1);
	walk(c[1], "data1", 1);
	c[0] = d->open(c[0], ORDWR);
	c[1] = d->open(c[1], ORDWR);
	fd[0] = newfd(c[0]);
	fd[1] = newfd(c[1]);
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
	Fgrp *f = up->fgrp;

	/*
	 * Close after dup'ing, so date > #d/1 works
	 */
	c = fdtochan(arg[0], -1, 0, 1);
	fd = arg[1];
	if(fd != -1){
		if(fd<0 || NFD<=fd) {
			cclose(c);
			error(Ebadfd);
		}
		lock(f);
		if(fd > f->maxfd)
			f->maxfd = fd;

		oc = f->fd[fd];
		f->fd[fd] = c;
		unlock(f);
		if(oc)
			cclose(oc);
	}else{
		if(waserror()) {
			cclose(c);
			nexterror();
		}
		fd = newfd(c);
		poperror();
	}

	return fd;
}

long
sysopen(ulong *arg)
{
	int fd;
	Chan *c = 0;

	openmode(arg[1]);	/* error check only */
	if(waserror()){
		if(c)
			cclose(c);
		nexterror();
	}
	validaddr(arg[0], 1, 0);
	c = namec((char*)arg[0], Aopen, arg[1], 0);
	fd = newfd(c);
	poperror();
	return fd;
}

void
fdclose(int fd, int flag)
{
	int i;
	Chan *c;
	Fgrp *f = up->fgrp;

	lock(f);
	c = f->fd[fd];
	if(c == 0){
		/* can happen for users with shared fd tables */
		unlock(f);
		return;
	}
	if(flag){
		if(c==0 || !(c->flag&flag)){
			unlock(f);
			return;
		}
	}
	f->fd[fd] = 0;
	if(fd == f->maxfd)
		for(i=fd; --i>=0 && f->fd[i]==0; )
			f->maxfd = i;

	unlock(f);
	cclose(c);
}

long
sysclose(ulong *arg)
{
	fdtochan(arg[0], -1, 0, 0);
	fdclose(arg[0], 0);

	return 0;
}

long
unionread(Chan *c, void *va, long n)
{
	long nr;
	Chan *nc;
	Pgrp *pg;

	pg = up->pgrp;
	rlock(&pg->ns);

	for(;;) {
		if(waserror()) {
			runlock(&pg->ns);
			nexterror();
		}
		nc = cclone(c->mnt->to, 0);
		poperror();

		if(c->mountid != c->mnt->mountid) {
			pprint("unionread: changed underfoot?\n");
			runlock(&pg->ns);
			cclose(nc);
			return 0;
		}

		/* Error causes component of union to be skipped */
		if(waserror()) {	
			cclose(nc);
			goto next;
		}

		nc = devtab[nc->type]->open(nc, OREAD);
		nc->offset = c->offset;
		nr = devtab[nc->type]->read(nc, va, n, nc->offset);
		/* devdirread e.g. changes it */
		c->offset = nc->offset;	
		poperror();

		cclose(nc);
		if(nr > 0) {
			runlock(&pg->ns);
			return nr;
		}
		/* Advance to next element */
	next:
		c->mnt = c->mnt->next;
		if(c->mnt == 0)
			break;
		c->mountid = c->mnt->mountid;
		c->offset = 0;
	}
	runlock(&pg->ns);
	return 0;
}

long
sysread9p(ulong *arg)
{
	int dir;
	long n;
	Chan *c;

	validaddr(arg[1], arg[2], 1);
	c = fdtochan(arg[0], OREAD, 1, 1);
	if(waserror()) {
		cclose(c);
		nexterror();
	}

	n = arg[2];
	dir = c->qid.path&CHDIR;

	if(dir) {
		n -= n%DIRLEN;
		if(c->offset%DIRLEN || n==0)
			error(Etoosmall);
	}

	if(dir && c->mnt)
		n = unionread(c, (void*)arg[1], n);
	else if(devchar[c->type] != L'M')
		n = devtab[c->type]->read(c, (void*)arg[1], n, c->offset);
	else
		n = mntread9p(c, (void*)arg[1], n, c->offset);

	lock(c);
	c->offset += n;
	unlock(c);

	poperror();
	cclose(c);

	return n;
}

long
sysread(ulong *arg)
{
	int dir;
	long n;
	Chan *c;

	validaddr(arg[1], arg[2], 1);
	c = fdtochan(arg[0], OREAD, 1, 1);
	if(waserror()) {
		cclose(c);
		nexterror();
	}

	n = arg[2];
	dir = c->qid.path&CHDIR;

	if(dir) {
		n -= n%DIRLEN;
		if(c->offset%DIRLEN || n==0)
			error(Etoosmall);
	}

	if(dir && c->mnt)
		n = unionread(c, (void*)arg[1], n);
	else
		n = devtab[c->type]->read(c, (void*)arg[1], n, c->offset);

	lock(c);
	c->offset += n;
	unlock(c);

	poperror();
	cclose(c);

	return n;
}

long
syswrite9p(ulong *arg)
{
	Chan *c;
	long n;

	validaddr(arg[1], arg[2], 0);
	c = fdtochan(arg[0], OWRITE, 1, 1);
	if(waserror()) {
		cclose(c);
		nexterror();
	}

	if(c->qid.path & CHDIR)
		error(Eisdir);

	if(devchar[c->type] != L'M')
		n = devtab[c->type]->write(c, (void*)arg[1], arg[2], c->offset);
	else
		n = mntwrite9p(c, (void*)arg[1], arg[2], c->offset);
	lock(c);
	c->offset += n;
	unlock(c);

	poperror();
	cclose(c);

	return n;
}

long
syswrite(ulong *arg)
{
	Chan *c;
	long n;

	validaddr(arg[1], arg[2], 0);
	c = fdtochan(arg[0], OWRITE, 1, 1);
	if(waserror()) {
		cclose(c);
		nexterror();
	}

	if(c->qid.path & CHDIR)
		error(Eisdir);

	n = devtab[c->type]->write(c, (void*)arg[1], arg[2], c->offset);

	lock(c);
	c->offset += n;
	unlock(c);

	poperror();
	cclose(c);

	return n;
}

long
sysseek(ulong *arg)
{
	Chan *c;
	char buf[DIRLEN];
	Dir dir;
	long off;

	c = fdtochan(arg[0], -1, 1, 0);
	if(c->qid.path & CHDIR)
		error(Eisdir);

	if(devchar[c->type] == '|')
		error(Eisstream);

	off = 0;
	switch(arg[2]){
	case 0:
		off = c->offset = arg[1];
		break;

	case 1:
		lock(c);	/* lock for read/write update */
		c->offset += (long)arg[1];
		off = c->offset;
		unlock(c);
		break;

	case 2:
		devtab[c->type]->stat(c, buf);
		convM2D(buf, &dir);
		c->offset = dir.length + (long)arg[1];
		off = c->offset;
		break;
	}
	return off;
}

long
sysfstat(ulong *arg)
{
	Chan *c;

	validaddr(arg[1], DIRLEN, 1);
	evenaddr(arg[1]);
	c = fdtochan(arg[0], -1, 0, 1);
	if(waserror()) {
		cclose(c);
		nexterror();
	}
	devtab[c->type]->stat(c, (char*)arg[1]);
	poperror();
	cclose(c);
	return 0;
}

long
sysstat(ulong *arg)
{
	Chan *c;

	validaddr(arg[1], DIRLEN, 1);
	evenaddr(arg[1]);
	validaddr(arg[0], 1, 0);
	c = namec((char*)arg[0], Aaccess, 0, 0);
	if(waserror()){
		cclose(c);
		nexterror();
	}
	devtab[c->type]->stat(c, (char*)arg[1]);
	poperror();
	cclose(c);
	return 0;
}

long
syschdir(ulong *arg)
{
	Chan *c;

	validaddr(arg[0], 1, 0);

	c = namec((char*)arg[0], Atodir, 0, 0);
	cclose(up->dot);
	up->dot = c;
	return 0;
}

long
bindmount(ulong *arg, int ismount)
{
	ulong flag;
	int fd, ret;
	Chan *c0, *c1, *bc;
	struct{
		Chan	*chan;
		char	*spec;
		int	flags;
	}bogus;

	flag = arg[2];
	fd = arg[0];
	if(flag>MMASK || (flag&MORDER)==(MBEFORE|MAFTER))
		error(Ebadarg);

	bogus.flags = flag & (MRECOV|MCACHE);

	if(ismount){
		bc = fdtochan(fd, ORDWR, 0, 1);
		if(waserror()) {
			cclose(bc);
			nexterror();
		}
		bogus.chan = bc;

		validaddr(arg[3], 1, 0);
		if(vmemchr((char*)arg[3], '\0', NAMELEN) == 0)
			error(Ebadarg);

		bogus.spec = (char*)arg[3];
		if(strchr(bogus.spec, ' '))
			error(Ebadspec);

		ret = devno('M', 0);
		c0 = devtab[ret]->attach((char*)&bogus);

		poperror();
		cclose(bc);
	}
	else {
		bogus.spec = 0;
		validaddr(arg[0], 1, 0);
		c0 = namec((char*)arg[0], Aaccess, 0, 0);
	}

	if(waserror()){
		cclose(c0);
		nexterror();
	}

	validaddr(arg[1], 1, 0);
	c1 = namec((char*)arg[1], Amount, 0, 0);
	if(waserror()){
		cclose(c1);
		nexterror();
	}

	ret = cmount(c0, c1, flag, bogus.spec);

	poperror();
	cclose(c1);
	poperror();
	cclose(c0);
	if(ismount)
		fdclose(fd, 0);
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
sysunmount(ulong *arg)
{
	Chan *cmount, *cmounted;

	cmounted = 0;

	validaddr(arg[1], 1, 0);
	cmount = namec((char *)arg[1], Amount, 0, 0);

	if(arg[0]) {
		if(waserror()) {
			cclose(cmount);
			nexterror();
		}
		validaddr(arg[0], 1, 0);
		cmounted = namec((char*)arg[0], Aopen, OREAD, 0);
		poperror();
	}

	if(waserror()) {
		cclose(cmount);
		if(cmounted)
			cclose(cmounted);
		nexterror();
	}

	cunmount(cmount, cmounted);
	cclose(cmount);
	if(cmounted)
		cclose(cmounted);
	poperror();	
	return 0;
}

long
syscreate(ulong *arg)
{
	int fd;
	Chan *c = 0;

	openmode(arg[1]);	/* error check only */
	if(waserror()) {
		if(c)
			cclose(c);
		nexterror();
	}
	validaddr(arg[0], 1, 0);
	c = namec((char*)arg[0], Acreate, arg[1], arg[2]);
	fd = newfd(c);
	poperror();
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
		cclose(c);
		nexterror();
	}
	devtab[c->type]->remove(c);
	/*
	 * Remove clunks the fid, but we need to recover the Chan
	 * so fake it up.  rootclose() is known to be a nop.
	 */
	c->type = 0;
	poperror();
	cclose(c);
	return 0;
}

long
syswstat(ulong *arg)
{
	Chan *c;

	validaddr(arg[1], DIRLEN, 0);
	nameok((char*)arg[1]);
	validaddr(arg[0], 1, 0);
	c = namec((char*)arg[0], Aaccess, 0, 0);
	if(waserror()){
		cclose(c);
		nexterror();
	}
	devtab[c->type]->wstat(c, (char*)arg[1]);
	poperror();
	cclose(c);
	return 0;
}

long
sysfwstat(ulong *arg)
{
	Chan *c;

	validaddr(arg[1], DIRLEN, 0);
	nameok((char*)arg[1]);
	c = fdtochan(arg[0], -1, 1, 1);
	if(waserror()) {
		cclose(c);
		nexterror();
	}
	devtab[c->type]->wstat(c, (char*)arg[1]);
	poperror();
	cclose(c);
	return 0;
}
