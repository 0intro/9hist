#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

typedef struct Crypt	Crypt;
struct Crypt
{
	Crypt		*next;
	Ticket		t;
	Authenticator	a;
	char		tbuf[TICKETLEN];	/* remote ticket */
};

typedef struct Session	Session;
struct Session
{
	Lock;
	Crypt	*cache;			/* cache of tickets */
	char	cchal[CHALLEN];		/* client challenge */
	char	schal[CHALLEN];		/* server challenge */
	char	authid[NAMELEN];	/* server encryption uid */
	char	authdom[DOMLEN];	/* server encryption domain */
	ulong	cid;			/* challenge id */
};

struct
{
	Lock;
	Crypt		*free;
} cryptalloc;

char	eve[NAMELEN] = "bootes";
char	evekey[DESKEYLEN];
char	hostdomain[DOMLEN];

/*
 *  return true if current user is eve
 */
int
iseve(void)
{
	return strcmp(eve, u->p->user) == 0;
}

/*
 * crypt entries are allocated from a pool rather than allocated using malloc so
 * the memory can be protected from reading by devproc. The base and top of the
 * crypt arena is stored in palloc for devproc.
 */
Crypt*
newcrypt(void)
{
	Crypt *c;

	lock(&cryptalloc);
	if(cryptalloc.free) {
		c = cryptalloc.free;
		cryptalloc.free = c->next;
		unlock(&cryptalloc);
		memset(c, 0, sizeof(Crypt));
		return c;
	}

	cryptalloc.free = xalloc(sizeof(Crypt)*conf.nproc);
	if(cryptalloc.free == 0)
		panic("newcrypt");

	for(c = cryptalloc.free; c < cryptalloc.free+conf.nproc-1; c++)
		c->next = c+1;

	palloc.cmembase = (ulong)cryptalloc.free;
	palloc.cmemtop = palloc.cmembase+(sizeof(Crypt)*conf.nproc);
	unlock(&cryptalloc);
	return newcrypt();
}

void
freecrypt(Crypt *c)
{
	lock(&cryptalloc);
	c->next = cryptalloc.free;
	cryptalloc.free = c;
	unlock(&cryptalloc);
}

/*
 *  return the info received in the session message on this channel.
 *  if no session message has been exchanged, do it.
 */
long
sysfsession(ulong *arg)
{
	int i, n;
	Chan *c;
	Fcall *f;
	char *buf;
	Crypt *cp;
	Session *s;
	Ticketreq tr;

	validaddr(arg[1], TICKREQLEN, 1);
	f = malloc(sizeof(Fcall));
	if(f == 0)
		error(Enomem);
	buf = malloc(MAXMSG);
	if(buf == 0){
		free(f);
		error(Enomem);
	}
	c = fdtochan(arg[0], OWRITE, 0, 1);
	s = 0;
	if(waserror()) {
		if(s)
			free(s);
		close(c);
		free(buf);
		free(f);
		nexterror();
	}
	s = c->session;
	if(s == 0){
		/*  exchange a session message with the server */
		s = malloc(sizeof(Session));
		if(s == 0)
			error(Enomem);
		memset(s, 0, sizeof(Session));
		for(i = 0; i < CHALLEN; i++)
			s->cchal[i] = nrand(256);
		f->tag = NOTAG;
		f->type = Tsession;
		memmove(f->chal, s->cchal, CHALLEN);
		n = convS2M(f, buf);
		if((*devtab[c->type].write)(c, buf, n, 0) != n)
			error(Emountrpc);
	dkhack:
		n = (*devtab[c->type].read)(c, buf, MAXMSG, 0);
		if(n == 2 && buf[0] == 'O' && buf[1] == 'K')
			goto dkhack;
		if(convM2S(buf, f, n) == 0)
			error(Emountrpc);
		if(f->type == Rsession){
			memmove(s->schal, f->chal, CHALLEN);
			memmove(s->authid, f->authid, NAMELEN);
			memmove(s->authdom, f->authdom, DOMLEN);
		}
		s->cid = 0;
		c->session = s;
	}

	/* 
	 *  If server requires no ticket, or user is "none", or a ticket
	 *  is already cached, zero the request type
	 */
	tr.type = AuthTreq;
	if(strcmp(u->p->user, "none") == 0 || c->session->authid[0] == 0)
		tr.type = 0;
	else for(cp = s->cache; cp; cp = cp->next)
		if(strcmp(cp->t.cuid, u->p->user) == 0){
			tr.type = 0;
			break;
		}

	/*  create ticket request */
	memmove(tr.chal, c->session->schal, CHALLEN);
	memmove(tr.authid, c->session->authid, NAMELEN);
	memmove(tr.authdom, c->session->authdom, DOMLEN);
	memmove(tr.uid, u->p->user, NAMELEN);
	memmove(tr.hostid, eve, NAMELEN);
	convTR2M(&tr, (char*)arg[1]);

	poperror();
	close(c);
	free(buf);
	free(f);
	return 0;
}

/*
 *  attach tickets to a session
 */
long
sysfauth(ulong *arg)
{
	Chan *c;
	char *ta;
	Session *s;
	Crypt *cp, *ncp, **l;
	char tbuf[2*TICKETLEN];

	validaddr(arg[1], 2*TICKETLEN, 0);
	c = fdtochan(arg[0], OWRITE, 0, 1);
	s = c->session;
	if(s == 0)
		error("fauth must follow fsession");
	cp = newcrypt();
	if(waserror()){
		freecrypt(cp);
		nexterror();
	}

	/*  ticket supplied, use it */
	ta = (char*)arg[1];
	memmove(tbuf, ta, 2*TICKETLEN);
	convM2T(tbuf, &cp->t, evekey);
	if(cp->t.num != AuthTc)
		error("bad AuthTc in ticket");
	if(strncmp(u->p->user, cp->t.cuid, NAMELEN) != 0)
		error("bad uid in ticket");
	if(memcmp(cp->t.chal, s->schal, CHALLEN) != 0)
		error("bad chal in ticket");
	memmove(cp->tbuf, tbuf+TICKETLEN, TICKETLEN);

	/* string onto local list, replace old version */
	lock(s);
	l = &s->cache;
	for(ncp = s->cache; ncp; ncp = *l){
		if(strcmp(ncp->t.cuid, u->p->user) == 0){
			*l = ncp->next;
			freecrypt(ncp);
			break;
		}
		l = &ncp->next;
	}
	cp->next = s->cache;
	s->cache = cp;
	unlock(s);
	poperror();
	return 0;
}

/*
 *  free a session created by fsession
 */
void
freesession(Session *s)
{
	Crypt *cp, *next;

	for(cp = s->cache; cp; cp = next) {
		next = cp->next;
		freecrypt(cp);
	}
	free(s);
}

/*
 *  called by mattach() to fill in the Tattach message
 */
ulong
authrequest(Session *s, Fcall *f)
{
	Crypt *cp;
	ulong id, dofree;

	/* no authentication if user is "none" or if no ticket required by remote */
	if(s == 0 || s->authid[0] == 0 || strcmp(u->p->user, "none") == 0){
		memset(f->ticket, 0, TICKETLEN);
		memset(f->auth, 0, AUTHENTLEN);
		return 0;
	}

	/* look for ticket in cache */
	dofree = 0;
	lock(s);
	for(cp = s->cache; cp; cp = cp->next)
		if(strcmp(cp->t.cuid, u->p->user) == 0)
			break;

	id = s->cid++;
	unlock(s);

	if(cp == 0){
		/*
		 *  create a ticket using hostkey, this solves the
		 *  chicken and egg problem
		 */
		cp = newcrypt();
		cp->t.num = AuthTs;
		memmove(cp->t.chal, s->schal, CHALLEN);
		memmove(cp->t.cuid, u->p->user, NAMELEN);
		memmove(cp->t.suid, u->p->user, NAMELEN);
		memmove(cp->t.key, evekey, DESKEYLEN);
		convT2M(&cp->t, f->ticket, evekey);
		dofree = 1;
	} else
		memmove(f->ticket, cp->tbuf, TICKETLEN);

	/* create an authenticator */
	memmove(cp->a.chal, s->schal, CHALLEN);
	cp->a.num = AuthAc;
	cp->a.id = id;
	convA2M(&cp->a, f->auth, cp->t.key);
	if(dofree)
		freecrypt(cp);
	return id;
}

/*
 *  called by mattach() to check the Rattach message
 */
void
authreply(Session *s, ulong id, Fcall *f)
{
	Crypt *cp;

	if(s == 0)
		return;

	lock(s);
	for(cp = s->cache; cp; cp = cp->next)
		if(strcmp(cp->t.cuid, u->p->user) == 0)
			break;
	unlock(s);

	/* we're getting around authentication */
	if(s == 0 || cp == 0 || s->authid[0] == 0 || strcmp(u->p->user, "none") == 0)
		return;

	convM2A(f->rauth, &cp->a, cp->t.key);
	if(cp->a.num != AuthAs){
		print("bad encryption type\n");
		error("server lies");
	}
	if(memcmp(cp->a.chal, s->cchal, sizeof(cp->a.chal))){
		print("bad returned challenge\n");
		error("server lies");
	}	
	if(cp->a.id != id){
		print("bad returned id\n");
		error("server lies");
	}
}

/*
 *  called by devcons() for #c/authenticate
 *
 *  The protocol is
 *	1) read ticket request from #c/authenticate
 *	2) write ticket to #c/authenticate. if it matchs the challenge the
 *	  user is changed to the suid field of the ticket
 *	3) read authenticator (to confirm this is the server advertised)
 */
long
authread(Chan *c, char *a, int n)
{
	Crypt *cp;
	int i;
	Ticketreq tr;

	if(c->aux == 0){
		/*
		 *  first read returns a ticket request
		 */
		if(n != TICKREQLEN)
			error(Ebadarg);
		c->aux = newcrypt();
		cp = c->aux;
		memset(&tr, 0, sizeof(tr));
		tr.type = AuthTreq;
		strcpy(tr.hostid, eve);
		strcpy(tr.authid, eve);
		strcpy(tr.authdom, hostdomain);
		strcpy(tr.uid, u->p->user);
		for(i = 0; i < CHALLEN; i++)
			tr.chal[i] = nrand(256);
		memmove(cp->a.chal, tr.chal, CHALLEN);
		convTR2M(&tr, a);
	} else {
		/*
		 *  subsequent read returns an authenticator
		 */
		if(n != AUTHENTLEN)
			error(Ebadarg);
		cp = c->aux;
		cp->a.num = AuthAs;
		memmove(cp->a.chal, cp->t.chal, CHALLEN);
		cp->a.id = 0;
		convA2M(&cp->a, a, cp->t.key);
		freecrypt(cp);
		c->aux = 0;
	}
	return n;
}

long
authwrite(Chan *c, char *a, int n)
{
	Crypt *cp;

	if(n != TICKETLEN)
		error(Ebadarg);
	if(c->aux == 0)
		error(Ebadarg);
	cp = c->aux;
	convM2T(a, &cp->t, evekey);
	if(cp->t.num != AuthTs || memcmp(cp->a.chal, cp->t.chal, CHALLEN))
		error(Eperm);
	memmove(u->p->user, cp->t.suid, NAMELEN);
	return n;
}

/*
 *  called by devcons() for #c/authcheck
 *
 *  a write of a ticket+authenticator succeeds if they match
 */
long
authcheck(Chan *c, char *a, int n)
{
	Crypt *cp;

	if(n != TICKETLEN+AUTHENTLEN)
		error(Ebadarg);
	if(c->aux == 0)
		c->aux = newcrypt();
	cp = c->aux;
	convM2T(a, &cp->t, evekey);
	if(cp->t.num != AuthTc)
		error(Ebadarg);
	if(strcmp(u->p->user, cp->t.cuid))
		error(cp->t.cuid);
	convM2A(a+TICKETLEN, &cp->a, cp->t.key);
	if(cp->a.num != AuthAs || memcmp(cp->t.chal, cp->a.chal, CHALLEN))
		error(Eperm);
	return n;
}

void
authclose(Chan *c)
{
	if(c->aux)
		freecrypt(c->aux);
	c->aux = 0;
}

/*
 *  called by devcons() for key device
 */
long
keyread(char *a, int n, long offset)
{
	if(n<DESKEYLEN || offset != 0)
		error(Ebadarg);
	if(!iseve())
		error(Eperm);
	memmove(a, evekey, DESKEYLEN);
	return DESKEYLEN;
}

long
keywrite(char *a, int n)
{
	if(n != DESKEYLEN)
		error(Ebadarg);
	if(!iseve())
		error(Eperm);
	memmove(evekey, a, DESKEYLEN);
	return DESKEYLEN;
}

/*
 *  called by devcons() for user device
 *
 *  anyone can become none
 */
long
userwrite(char *a, int n)
{
	if(n >= NAMELEN)
		error(Ebadarg);
	if(strcmp(a, "none") != 0)
		error(Eperm);
	memset(u->p->user, 0, NAMELEN);
	strcpy(u->p->user, "none");
	return n;
}

/*
 *  called by devcons() for host owner/domain
 *
 *  writing hostowner also sets user
 */
long
hostownerwrite(char *a, int n)
{
	char buf[NAMELEN];

	if(!iseve())
		error(Eperm);
	if(n >= NAMELEN)
		error(Ebadarg);
	memset(buf, 0, NAMELEN);
	strncpy(buf, a, n);
	if(buf[0] == 0)
		error(Ebadarg);
	memmove(eve, buf, NAMELEN);
	memmove(u->p->user, buf, NAMELEN);
	return n;
}

long
hostdomainwrite(char *a, int n)
{
	char buf[DOMLEN];

	if(!iseve())
		error(Eperm);
	if(n >= DOMLEN)
		error(Ebadarg);
	memset(buf, 0, DOMLEN);
	strncpy(buf, a, n);
	if(buf[0] == 0)
		error(Ebadarg);
	memmove(hostdomain, buf, DOMLEN);
	return n;
}
