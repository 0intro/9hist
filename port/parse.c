#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * Generous estimate of number of fields, including terminal nil pointer
 */
static int
ncmdfield(char *p, int n)
{
	int white, nwhite;
	char *ep;
	int nf;

	if(p == nil)
		return 1;

	nf = 0;
	ep = p+n;
	white = 1;	/* first text will start field */
	while(p < ep){
		nwhite = (strchr(" \t\r\n", *p++ & 0xFF) != 0);	/* UTF is irrelevant */
		if(white && !nwhite)	/* beginning of field */
			nf++;
		white = nwhite;
	}
	return nf+1;	/* +1 for nil */
}

/*
 *  parse a command written to a device
 */
Cmdbuf*
parsecmd(char *p, int n)
{
	Cmdbuf *volatile cb;
	int nf;
	char *sp;

	nf = ncmdfield(p, n);

	/* allocate Cmdbuf plus string pointers plus copy of string including \0 */
	sp = smalloc(sizeof(*cb) + nf * sizeof(char*) + n + 1);
	cb = (Cmdbuf*)sp;
	cb->f = (char**)(&cb[1]);
	cb->buf = (char*)(&cb->f[nf]);

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

	cb->nf = tokenize(cb->buf, cb->f, nf-1);
	cb->f[cb->nf] = nil;

	return cb;
}

