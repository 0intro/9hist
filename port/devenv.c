#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"


enum
{
	Maxenvsize = 16300,
};

void
envreset(void)
{
}

void
envinit(void)
{
}

int
envgen(Chan *c, Dirtab*, int, int s, Dir *dp)
{
	Egrp *eg;
	Evalue *e;

	eg = up->egrp;
	qlock(eg);

	for(e = eg->entries; e && s; e = e->link)
		s--;

	if(e == 0) {
		qunlock(eg);
		return -1;
	}

	devdir(c, (Qid){e->path, 0}, e->name, e->len, eve, 0666, dp);
	qunlock(eg);
	return 1;
}

Chan*
envattach(char *spec)
{
	return devattach('e', spec);
}

Chan*
envclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
envwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, envgen);
}

void
envstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, envgen);
}

Chan *
envopen(Chan *c, int omode)
{
	Egrp *eg;
	Evalue *e;
	
	eg = up->egrp;
	if(c->qid.path & CHDIR) {
		if(omode != OREAD)
			error(Eperm);
	}
	else {
		qlock(eg);
		for(e = eg->entries; e; e = e->link)
			if(e->path == c->qid.path)
				break;

		if(e == 0) {
			qunlock(eg);
			error(Enonexist);
		}
		if(omode == (OWRITE|OTRUNC) && e->value) {
			free(e->value);
			e->value = 0;
			e->len = 0;
		}
		qunlock(eg);
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
envcreate(Chan *c, char *name, int omode, ulong)
{
	Egrp *eg;
	Evalue *e;

	if(c->qid.path != CHDIR)
		error(Eperm);

	omode = openmode(omode);
	eg = up->egrp;

	qlock(eg);
	if(waserror()) {
		qunlock(eg);
		nexterror();
	}

	for(e = eg->entries; e; e = e->link)
		if(strcmp(e->name, name) == 0)
			error(Einuse);

	e = smalloc(sizeof(Evalue));
	e->name = smalloc(strlen(name)+1);
	strcpy(e->name, name);

	e->path = ++eg->path;
	e->link = eg->entries;
	eg->entries = e;
	c->qid = (Qid){e->path, 0};
	
	qunlock(eg);
	poperror();

	c->offset = 0;
	c->mode = omode;
	c->flag |= COPEN;
}

void
envremove(Chan *c)
{
	Egrp *eg;
	Evalue *e, **l;

	if(c->qid.path & CHDIR)
		error(Eperm);

	eg = up->egrp;
	qlock(eg);

	l = &eg->entries;
	for(e = *l; e; e = e->link) {
		if(e->path == c->qid.path)
			break;
		l = &e->link;
	}

	if(e == 0) {
		qunlock(eg);
		error(Enonexist);
	}

	*l = e->link;
	qunlock(eg);
	free(e->name);
	if(e->value)
		free(e->value);
	free(e);
}

void
envwstat(Chan*, char*)
{
	error(Eperm);
}

void
envclose(Chan*)
{
}

long
envread(Chan *c, void *a, long n, ulong offset)
{
	Egrp *eg;
	Evalue *e;

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, envgen);

	eg = up->egrp;
	qlock(eg);
	for(e = eg->entries; e; e = e->link)
		if(e->path == c->qid.path)
			break;

	if(e == 0) {
		qunlock(eg);
		error(Enonexist);
	}

	if(offset + n > e->len)
		n = e->len - offset;
	if(n <= 0)
		n = 0;
	else
		memmove(a, e->value+offset, n);
	qunlock(eg);
	return n;
}

Block*
envbread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

long
envwrite(Chan *c, void *a, long n, ulong offset)
{
	char *s;
	int vend;
	Egrp *eg;
	Evalue *e;

	if(n <= 0)
		return 0;

	vend = offset+n;
	if(vend > Maxenvsize)
		error(Etoobig);

	eg = up->egrp;
	qlock(eg);
	for(e = eg->entries; e; e = e->link)
		if(e->path == c->qid.path)
			break;

	if(e == 0) {
		qunlock(eg);
		error(Enonexist);
	}

	if(vend > e->len) {
		s = smalloc(offset+n);
		memmove(s, e->value, e->len);
		if(e->value)
			free(e->value);
		e->value = s;
		e->len = vend;
	}
	memmove(e->value+offset, a, n);
	qunlock(eg);
	return n;
}

long
envbwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}

void
envcpy(Egrp *to, Egrp *from)
{
	Evalue **l, *ne, *e;

	l = &to->entries;
	qlock(from);
	for(e = from->entries; e; e = e->link) {
		ne = smalloc(sizeof(Evalue));
		ne->name = smalloc(strlen(e->name)+1);
		strcpy(ne->name, e->name);
		if(e->value) {
			ne->value = smalloc(e->len);
			memmove(ne->value, e->value, e->len);
			ne->len = e->len;
		}
		ne->path = ++to->path;
		*l = ne;
		l = &ne->link;
	}
	qunlock(from);
}

void
closeegrp(Egrp *eg)
{
	Evalue *e, *next;

	if(decref(eg) == 0) {
		for(e = eg->entries; e; e = next) {
			next = e->link;
			free(e->name);
			if(e->value)
				free(e->value);
			free(e);
		}
		free(eg);
	}
}

/*
 *  to let the kernel set environment variables
 */
void
ksetenv(char *ename, char *eval)
{
	Chan *c;
	char buf[2*NAMELEN];

	sprint(buf, "#e/%s", ename);
	c = namec(buf, Acreate, OWRITE, 0600);
	(*devtab[c->type].write)(c, eval, strlen(eval), 0);
	close(c);
}
