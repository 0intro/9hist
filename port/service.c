#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"fcall.h"

#define	BUFSIZE	(MAXFDATA+500) 	/* BUG */
typedef struct Buffer Buffer;
struct Buffer
{
	Buffer	*next;
	char	buf[BUFSIZE];
};

struct Servalloc {
	Lock;
	Service *all;
	Service *freel;
	Buffer	*ball;
	Buffer	*bfreel;
} servalloc;


/*
 *  allocate the taks force structures and put them
 *  on the free list
 */
void
serviceinit(void)
{
	Service *s;
	Buffer *b;

	servalloc.all = ialloc(conf.nservice * sizeof(Service), 0);
	servalloc.freel = servalloc.all;
	for(s = servalloc.all; s < &servalloc.all[conf.nservice-1]; s++)
		s->next = s+1;

	servalloc.ball = ialloc(2*conf.nservice * sizeof(Service), 0);
	servalloc.bfreel = servalloc.ball;
	for(b = servalloc.ball; b < &servalloc.ball[2*conf.nservice-1]; b++)
		b->next = b+1;
}

/*
 *  Add a process to a service
 */
static Proc *
reinforce(Service *s)
{
	int i;
	Proc *p;

	/*
	 *  don't start one up if we already have too many,
	 *  return our own process number
	 */
	lock(s);
	if(s->ref == NSTUB){
		unlock(s);
		return u->p;
	}
	s->ref++;
	unlock(s);

	/*
	 *  create it
	 */
	return kproc(s->name, 0, 0);
}

/*
 *  resign from a service
 */
static void
resign(Service *s)
{
	int i;

	print("%lux resigning\n", u->p);
	if(decref(s) == 0){
		print("freeing service\n");
		close(s->inc);
		close(s->outc);
		lock(&servalloc);
		s->next = servalloc.freel;
		servalloc.freel = s;
		unlock(&servalloc);
	}
	pexit(0, 1);
}

/*
 *  Create a service of kernel processes to handle file system requests.
 *  (*doit)() is a function that handles that event.  Service
 *  dynamicly creates new processes (up to MAXTASK) to ensure that 1 process
 *  is always awaiting while others are doing.
 */
void
service(char *name, Chan *cin, Chan *cout, void (*doit)(Chan*, char*, long))
{
	void *a;
	Service *s;
	Buffer *b;
	long n;

	b = 0;

	/*
	 *  allocate a service
	 */
	lock(&servalloc);
	s = servalloc.freel;
	if(s == 0)
		panic("no more services");
	servalloc.freel = s->next;
	unlock(&servalloc);
	memset(s, 0, sizeof(Service));
	if(name==0 || *name==0)
		strcpy(name, "filsys");
	else
		strncpy(s->name, name, sizeof(s->name));
	s->inc = cin;
	s->outc = cout;
	s->die = 0;
	incref(cin);
	incref(cout);

	if(waserror()){
		/*
		 *  tell everyone else to die
		 */
		s->die = 1;

		/*
		 *  wake up the next guy
		 */
		qunlock(&s->alock);

		/*
		 *  free the buffer if we have one
		 */
		if(b){
			lock(&servalloc);
			b->next = servalloc.bfreel;
			servalloc.bfreel = b;
			unlock(&servalloc);
		}
		resign(s);
	}

	/*
	 *  Create the first process in the service.  The
	 *  caller returns.
	 */
	if(reinforce(s)){
		poperror();
		return;
	}

	/*
	 *  first process enters the infinite loop
	 */
	for(;;){
		b = 0;
		qlock(&s->alock);

		/*
		 *  if there's more than 1 process waiting, we're
		 *  superfluous
		 */
		if(s->die || s->alock.head && s->alock.head!=s->alock.tail){
			qunlock(&s->alock);
			resign(s);
		}

		/*
		 *  get a buffer
		 */
		lock(&servalloc);
		b = servalloc.bfreel;
		if(b == 0){
			print("no serv buffers\n");
			unlock(&servalloc);
			continue;
		}
		servalloc.bfreel = b->next;
		unlock(&servalloc);

		/*
		 *  wait for something to happen
		 */
		n = (*devtab[cin->type].read)(cin, b->buf, sizeof(b->buf));
		if(n <= 0)
			error(Ehungup);

		/*
		 *  create a new process if none are waiting to
		 *  take over
		 */
		if(s->alock.head == 0)
			if(reinforce(s) == 0)
				continue;
		qunlock(&s->alock);

		/*
		 *  process an event
		 */
		(*doit)(cout, b->buf, n);

		/*
		 *  release the buffer
		 */
		lock(&servalloc);
		b->next = servalloc.bfreel;
		servalloc.bfreel = b;
		unlock(&servalloc);
	}
}
