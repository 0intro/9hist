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

	if(up != nil)
		cb = smalloc(sizeof(*cb));
	else{
		cb = malloc(sizeof(*cb));
		if(cb == nil)
			return nil;
	}
	
	if(n > sizeof(cb->buf)-1)
		n = sizeof(cb->buf)-1;

	if(up != nil && waserror()){
		free(cb);
		nexterror();
	}
	memmove(cb->buf, p, n);
	if(up != nil)
		poperror();

	if(n > 0 && cb->buf[n-1] == '\n')
		n--;
	cb->buf[n] = '\0';
	cb->nf = tokenize(cb->buf, cb->f, nelem(cb->f));
	return cb;
}

