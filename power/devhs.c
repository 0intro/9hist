#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"

#include	"io.h"

enum {
	Maxtxburst=	1024,		/* maximum transmit burst size */
	Vmevec=		0xd0,		/* vme vector for interrupts */
	Intlevel=	5,		/* level to interrupt on */
};

struct hsvmestats {
	ulong	parity;
	ulong	rintr;
	ulong	tintr;
} hsvmestats;

#define ALIVE		0x0001
#define IENABLE		0x0004
#define EXOFLOW		0x0008
#define IRQ		0x0010
#define EMUTE		0x0020
#define EPARITY		0x0040
#define EFRAME		0x0080
#define EROFLOW		0x0100
#define REF		0x0800
#define XFF		0x4000
#define XHF		0x8000

#define FORCEW		0x0008
#define IPL(x)		((x)<<5)
#define NORESET		0xFF00
#define RESET		0x0000

#define CTL		0x0100
#define CHNO		0x0200
#define TXEOD		0x0400
#define NND		0x8000

Rendez hsvmer;

#define DELAY(n)	{ \
	register int N = 12*(n); \
	while (--N > 0); \
	}

static void hsvmeintr(int);

void
hsvmerestart(struct hsvme *addr)
{
	addr->csr = RESET;
	wbflush();
	DELAY(20000);

	/*
	 *  set interrupt vector
	 *  turn on addrice
	 *  set forcew to a known value
	 *  interrupt on level `Intlevel'
	 */
	addr->vector = 0xd0;
	addr->csr = NORESET|IPL(Intlevel)|IENABLE|ALIVE;
	wbflush();
	DELAY(500);
	addr->csr = NORESET|IPL(Intlevel)|FORCEW|IENABLE|ALIVE;
	wbflush();
	DELAY(500);
}

void
hsvmereset(void)
{
	struct hsvme *addr;

	addr = HSVME;
	addr->csr = RESET;
	wbflush();
	DELAY(20000);

	/*
	 *  routine to call on interrupt
	 */
	setvmevec(Vmevec, hsvmeintr);
}

void
hsvmeinit(void)
{
}

/*
 *  enable the device for interrupts
 */
static int
never(void *arg)
{
	return 0;
}
Chan*
hsvmeattach(char *spec)
{
	struct hsvme *addr;

	addr = HSVME;
	hsvmerestart(addr);
	print("hsvme csr %ux\n", addr->csr);

	return devattach('h', spec);
}

Chan*
hsvmeclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int	 
hsvmewalk(Chan *c, char *name)
{
	if(c->qid != CHDIR)
		return 0;
	if(strcmp(name, "hsvme") == 0){
		c->qid = 1;
		return 1;
	}
	return 0;
}

void	 
hsvmestat(Chan *c, char *dp)
{
	print("hsvmestat\n");
	error(0, Egreg);
}

Chan*
hsvmeopen(Chan *c, int omode)
{
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
hsvmecreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Eperm);
}

void	 
hsvmeclose(Chan *c)
{
}

long	 
hsvmeread(Chan *c, void *buf, long n)
{
	error(0, Egreg);
	return 0;
}

long	 
hsvmewrite(Chan *c, void *buf, long n)
{
	error(0, Egreg);
	return 0;
}

void	 
hsvmeremove(Chan *c)
{
	error(0, Eperm);
}

void	 
hsvmewstat(Chan *c, char *dp)
{
	error(0, Eperm);
}

void
hsvmeuserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}

void	 
hsvmeerrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

static void
hsvmeintr(int vec)
{
	ushort csr;
	struct hsvme *addr;

	print("hsvme intr\n");
	addr = HSVME;
	wbflush();
	csr = addr->csr;
	do {
		if (addr->csr & REF) {
			hsvmestats.rintr++;
			print("hsvme rintr\n");
		}
		if (addr->csr & XHF) {
			hsvmestats.tintr++;
			print("hsvme tintr\n");
		}
		if ((csr^XFF) & (XFF|EROFLOW|EFRAME|EPARITY|EXOFLOW)) {
			hsvmestats.parity++;
			hsvmerestart(addr);
			print("hsvme %ux: reset, csr = 0x%x\n",
				HSVME, csr);
		}
		wbflush();
	} while ((csr = addr->csr) & REF);
}
