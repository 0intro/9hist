#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 *  parse a command written to a device
 */
Cmdbuf*
parsecmd(char *p, int n)
{
	Cmdbuf *volatile cb;
	int nf;
	char *sp;

	/* count fields and allocate a big enough cmdbuf */
	for(nf = 1, sp = p; sp != nil && *sp; nf++, sp = strchr(sp+1, ' '))
		;
	sp = smalloc(sizeof(*cb) + n + 1 + nf*sizeof(char*));
	cb = (Cmdbuf*)sp;
	cb->buf = sp+sizeof(*cb);
	cb->f = (char**)(cb->buf + n + 1);

	if(up!=nil && waserror()){
		free(cb);
		nexterror();
	}
	memmove(cb->buf, p, n);
	if(up != nil)
		poperror();

	/* dump new line and null terminate */
	if(n > 0 && cb->buf[n-1] == '\n')
		n--;
	cb->buf[n] = '\0';

	cb->nf = getfields(cb->buf, cb->f, nf, 1, " ");
	return cb;
}

