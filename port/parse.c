#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

int
parsefields(char *lp, char **fields, int n, char *sep)
{
	int i;

	for(i=0; lp && *lp && i<n; i++){
		while(*lp && strchr(sep, *lp) != 0)
			*lp++=0;
		if(*lp == 0)
			break;
		fields[i]=lp;
		while(*lp && strchr(sep, *lp) == 0)
			lp++;
	}
	return i;
}

/*
 *  parse a command written to a device
 */
Cmdbuf*
parsecmd(char *p, int n)
{
	Cmdbuf *cb;

	cb = smalloc(sizeof(*cb));
	
	if(n > sizeof(cb->buf)-1)
		n = sizeof(cb->buf)-1;
	memmove(cb->buf, p, n);
	if(n > 0 && cb->buf[n-1] == '\n')
		n--;
	cb->buf[n] = '\0';
	cb->nf = parsefields(cb->buf, cb->f, nelem(cb->f), " ");
	return cb;
}

