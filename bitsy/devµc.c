#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

enum{
	Qbacklight = 1,
	Qbattery,
	Qbuttons,
	Qled,
	Qversion,

	/* command types */
	BLversion=	0,
	BLbuttons=	2,	/* button events */
	BLtouch=	3,	/* read touch screen events */
	BLled=		8,	/* turn LED on/off */
	BLbattery=	9,	/* read battery status */
	BLbacklight=	0xd,	/* backlight control */

	SOF=	0x2,		/* start of frame */

	/* key definitions */
	Up=		0xFF0E,
	Left=		0xFF11,
	Right=		0xFF12,
	Down=		0x8000,
};

Dirtab µcdir[]={
	"backlight",	{ Qbacklight, 0 },	0,	0664,
	"battery",	{ Qbattery, 0 },	0,	0664,
	"buttons",	{ Qbuttons, 0 },	0,	0664,
	"led",		{ Qled, 0 },		0,	0664,
	"version",	{ Qversion, 0 },	0,	0664,
};

static struct µcontroller
{
	/* message being rcvd */
	int	state;
	uchar	buf[16+4];
	uchar	n;

	/* for messages that require acks */
	QLock;
	Rendez	r;

	/* battery */
	uchar	acstatus;
	uchar	voltage;
	ushort	batstatus;
	uchar	batchem;

	/* version string */
	char	version[16+2];
} ctlr;

/* button map */
Rune bmap[4] = 
{
	Up, Right, Down, Left
};

int
µcputc(Queue*, int ch)
{
	int i, len, b, up;
	uchar cksum;
	uchar *p;

	if(ctlr.n > sizeof(ctlr.buf))
		panic("µcputc");

	ctlr.buf[ctlr.n++] = (uchar)ch;
		
	for(;;){
		/* message hasn't started yet? */
		if(ctlr.buf[0] != SOF){
			p = memchr(ctlr.buf, SOF, ctlr.n);
			if(p == nil){
				ctlr.n = 0;
				break;
			} else {
				ctlr.n -= p-ctlr.buf;
				memmove(ctlr.buf, p, ctlr.n);
			}
		}
	
		/* whole msg? */
		len = ctlr.buf[1] & 0xf;
		if(ctlr.n < 3 || ctlr.n < len+3)
			break;
	
		/* check the sum */
		ctlr.buf[0] = ~SOF;	/* make sure we process this msg exactly once */
		cksum = 0;
		for(i = 1; i < len+2; i++)
			cksum += ctlr.buf[i];
		if(ctlr.buf[len+2] != cksum)
			continue;
	
		/* parse resulting message */
		p = ctlr.buf+2;
		switch(ctlr.buf[1] >> 4){
		case BLversion:
			strncpy(ctlr.version, (char*)p, len);
			ctlr.version[len] = '0';
			strcat(ctlr.version, "\n");
			wakeup(&ctlr.r);
			break;
		case BLbuttons:
			if(len < 1)
				break;
			b = p[0] & 0x7f;
			up = p[0] & 80;
	
			if(b > 5){
				/* rocker panel acts like arrow keys */
				if(b < 10)
					kbdputc(kbdq, bmap[b-6]);
			} else {
				/* the rest like mouse buttons */
				if(--b == 0)
					b = 5;
				penbutton(up, 1<<b);
			}
			break;
		case BLtouch:
			if(len == 4)
				pentrackxy((p[0]<<8)|p[1], (p[2]<<8)|p[3]);
			else
				pentrackxy(-1, -1);
			break;
		case BLled:
			wakeup(&ctlr.r);
			break;
		case BLbattery:
			if(len >= 5){
				ctlr.acstatus = p[0];
				ctlr.voltage = (p[3]<<8)|p[2];
				ctlr.batstatus = p[4];
				ctlr.batchem = p[1];
			}
			wakeup(&ctlr.r);
			break;
		case BLbacklight:
			wakeup(&ctlr.r);
			break;
		}
	
		/* remove the message */
		ctlr.n -= len+3;
		memmove(ctlr.buf, &ctlr.buf[len+3], ctlr.n);
	}
	return 0;
}

static void
_sendmsg(uchar id, uchar *data, int len)
{
	uchar buf[20];
	uchar cksum;
	uchar c;
	uchar *p = buf;
	int i;

	/* create the message */
	if(sizeof(buf) < len+4)
		return;
	cksum = (id<<4) | len;
	*p++ = SOF;
	*p++ = cksum;
	for(i = 0; i < len; i++){
		c = data[i];
		cksum += c;
		*p++ = c;
	}
	*p++ = cksum;

	/* send the message - there should be a more generic way to do this */
	serialµcputs(buf, p-buf);
}

/* the tsleep takes care of lost acks */
static void
sendmsgwithack(uchar id, uchar *data, int len)
{
	if(waserror()){
		qunlock(&ctlr);
		nexterror();
	}
	qlock(&ctlr);
	_sendmsg(id, data, len);
	tsleep(&ctlr.r, return0, 0, 100);
	qunlock(&ctlr);
	poperror();
}

static void
sendmsg(uchar id, uchar *data, int len)
{
	if(waserror()){
		qunlock(&ctlr);
		nexterror();
	}
	qlock(&ctlr);
	_sendmsg(id, data, len);
	qunlock(&ctlr);
	poperror();
}

void
µcinit(void)
{
}

static Chan*
µcattach(char* spec)
{
	return devattach('r', spec);
}

static int	 
µcwalk(Chan* c, char* name)
{
	return devwalk(c, name, µcdir, nelem(µcdir), devgen);
}

static void	 
µcstat(Chan* c, char* dp)
{
	devstat(c, dp, µcdir, nelem(µcdir), devgen);
}

static Chan*
µcopen(Chan* c, int omode)
{
	omode = openmode(omode);
	if(strcmp(up->user, eve)!=0)
		error(Eperm);
	return devopen(c, omode, µcdir, nelem(µcdir), devgen);
}

static void	 
µcclose(Chan*)
{
}

char*
acstatus(int x)
{
	if(x)
		return "attached";
	else
		return "detached";
}

char*
batstatus(int x)
{
	switch(x){
	case 1:		return "high";
	case 2:		return "low";
	case 4:		return "critical";
	case 8:		return "charging";
	case 0x80:	return "none";
	}
	return "ok";
}

static long	 
µcread(Chan* c, void* a, long n, vlong off)
{
	char buf[64];

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, µcdir, nelem(µcdir), devgen);

	switch(c->qid.path){
	case Qbattery:
		sendmsgwithack(BLbattery, nil, 0);		/* send a battery request */
		sprint(buf, "voltage: %d\nac: %s\nstatus: %s\n", ctlr.voltage,
			acstatus(ctlr.acstatus),
			batstatus(ctlr.batstatus));
		return readstr(off, a, n, buf);
	case Qversion:
		sendmsgwithack(BLversion, nil, 0);		/* send a battery request */
		return readstr(off, a, n, ctlr.version);
	}
	error(Ebadarg);
	return 0;
}

#define PUTBCD(n,o) bcdclock[o] = (n % 10) | (((n / 10) % 10)<<4)

static long	 
µcwrite(Chan* c, void* a, long n, vlong off)
{
	Cmdbuf *cmd;
	uchar data[16];
	int i;

	if(off != 0)
		error(Ebadarg);

	cmd = parsecmd(a, n);
	if(cmd->nf > 15)
		error(Ebadarg);
	for(i = 0; i < cmd->nf; i++)
		data[i] = atoi(cmd->f[i]);

	switch(c->qid.path){
	case Qled:
		sendmsgwithack(BLled, data, cmd->nf);
		break;
	case Qbacklight:
		sendmsgwithack(BLbacklight, data, cmd->nf);
		break;
	default:
		error(Ebadarg);
	}
	return n;
}

Dev µcdevtab = {
	'r',
	"µc",

	devreset,
	µcinit,
	µcattach,
	devclone,
	µcwalk,
	µcstat,
	µcopen,
	devcreate,
	µcclose,
	µcread,
	devbread,
	µcwrite,
	devbwrite,
	devremove,
	devwstat,
};
