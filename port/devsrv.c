#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"
#include	"fcall.h"

void *calloc(unsigned, unsigned);
void free(void *);

/* This structure holds the contents of a directory entry.  Entries are kept
 * in a linked list.
 */
typedef struct Entry Entry;
struct Entry {
	Lock;
	Entry *next;		/* next entry */
	Entry **back;		/* entry pointer */
	Entry *parent;		/* parent directory */
	Dir dir;		/* dir structure */
	union{
		Chan *chan;	/* if not a subdirectory */
		Entry *entries;	/* directory entries */
	};
};

void
srvinit(void)
{
}

void
srvreset(void)
{
}

Entry *
srvalloc(int mode){
	Entry *e;
	static Lock qidlock;
	static nextqid;

	e = calloc(1, sizeof(Entry));
	e->dir.atime = e->dir.mtime = seconds();
	lock(&qidlock);	/* for qid allocation */
	e->dir.qid = mode | nextqid++;
	unlock(&qidlock);
	return e;
}

Chan *
srvattach(char *spec)
{
	Chan *c;
	static Lock rootlock;
	static Entry *root;

	lock(&rootlock);
	if(root==0){
		root = srvalloc(CHDIR);
		root->dir.mode = CHDIR | 0777;
	}
	unlock(&rootlock);
	c = devattach('s', spec);
	c->qid = root->dir.qid;
	c->aux = root;
	return c;
}

Chan *
srvclone(Chan *c, Chan *nc)
{
	nc = devclone(c, nc);
	nc->aux = c->aux;
	return nc;
}

int
srvwalk(Chan *c, char *name)
{
	Entry *dir, *e;

	isdir(c);
	if(strcmp(name, ".") == 0)
		return 1;
	if((dir=c->aux) == 0)
		panic("bad aux pointer in srvwalk");
	if(strcmp(name, "..") == 0)
		e = dir->parent;
	else{
		lock(dir);
		for(e=dir->entries; e; e=e->next)
			if (strcmp(name, e->dir.name) == 0)
				break;
		unlock(dir);
	}
	if(e==0){
		u->error.code = Enonexist;
		u->error.type = 0;
		u->error.dev = 0;
		return 0;
	}
	c->qid = e->dir.qid;
	c->aux = e;
	return 1;
}

void
srvstat(Chan *c, char *db)
{
	Entry *e;

	e = c->aux;
	convD2M(&e->dir, db);
}

Chan *
srvopen(Chan *c, int omode)
{
	Entry *e;
	Chan *f;

	if(c->qid & CHDIR){
		if(omode != OREAD)
			error(0, Eisdir);
		c->mode = omode;
		c->flag |= COPEN;
		c->offset = 0;
		return c;
	}
	if((e=c->aux) == 0)
		error(0, Egreg);
	if((f=e->chan) == 0)
		error(0, Eshutdown);
	if(omode & OTRUNC)
		error(0, Eperm);
	if(omode!=f->mode && f->mode!=ORDWR)
		error(0, Eperm);
	close(c);
	incref(f);
	return f;
}

void
srvcreate(Chan *c, char *name, int omode, ulong perm)
{
	Entry *parent, *e;

	parent = c->aux;
	isdir(c);
	lock(parent);
	if (waserror()){
		unlock(parent);
		nexterror();
	}
	for(e=parent->entries; e; e=e->next)
		if(strcmp(name, e->dir.name) == 0)
			error(0, Einuse);
	e = srvalloc(perm & CHDIR);
	e->parent = parent;
	strcpy(e->dir.name, name);
	e->dir.mode = perm & parent->dir.mode;
	e->dir.gid = parent->dir.gid;
	if(e->next = parent->entries)	/* assign = */
		e->next->back = &e->next;
	e->back = &parent->entries;
	*e->back = e;
	parent->dir.mtime = e->dir.mtime;
	unlock(parent);
	poperror();
	c->qid = e->dir.qid;
	c->aux = e;
	c->flag |= COPEN;
	c->mode = omode;
}

void
srvremove(Chan *c)
{
	Entry *e;

	e = c->aux;
	if(e->parent == 0)
		error(0, Eperm);
	lock(e->parent);
	if(waserror()){
		unlock(e->parent);
		nexterror();
	}
	if(e->dir.mode & CHDIR){
		if (e->entries != 0)
			error(0, Eperm);
	}else{
		if(e->chan == 0)
			error(0, Eshutdown);
		close(e->chan);
	}
	if(*e->back = e->next)	/* assign = */
		e->next->back = e->back;
	unlock(e->parent);
	poperror();
	free(e);
}

void
srvwstat(Chan *c, char *dp)
{
	error(0, Egreg);
}

void
srvclose(Chan *c)
{
}

/* A directory is being read.  The entries must be synthesized.  e points
 * to a list of entries in this directory.  Count is the size to be
 * read.
 */
int
srvdirentry(Entry *e, char *a, long count){
	Dir dir;
	int n;

	n = 0;
	while(n!=count && e!=0){
		n += convD2M(&e->dir, a + n);
		e = e->next;
	}
	return n;
}

long
srvread(Chan *c, void *va, long n)
{
	Entry *dir, *e;
	int offset;

	dir = c->aux;
	offset = c->offset;
	isdir(c);
	if(n <= 0)
		return 0;
	if(offset%DIRLEN || n%DIRLEN)
		error(0, Ebaddirread);
	lock(dir);
	for(e=dir->entries; e; e=e->next)
		if(offset <= 0){
			n = srvdirentry(e, va, n);
			unlock(dir);
			c->offset += n;
			return n;
		}else
			offset -= DIRLEN;
	unlock(dir);
	return 0;
}

long
srvwrite(Chan *c, void *va, long n)
{
	Entry *e;
	int i, fd;
	char buf[32];

	e = c->aux;
	if(e->dir.mode & CHDIR)
		panic("write to directory");
	lock(e);
	if(waserror()){
		unlock(e);
		nexterror();
	}
	if(e->chan)
		error(0, Egreg);
	if(n >= sizeof buf)
		error(0, Egreg);
	memcpy(buf, va, n);	/* so we can NUL-terminate */
	buf[n] = 0;
	fd = strtoul(buf, 0, 0);
	fdtochan(fd, -1);	/* error check only */
	e->chan = u->fd[fd];
	incref(e->chan);
	unlock(e);
	poperror();
	return n;
}

void
srverrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

void
srvuserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}
