#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	"devtab.h"

/*
 *  Stargate's Avanstar serial board.  There are ISA, EISA, microchannel
 *  versions.  We only handle the ISA one.
 */
typedef struct Astar Astar;
typedef struct Astarchan Astarchan;

enum
{
	/* ISA control ports */
	ISAid=		0,		/* Id port and its values */
	 ISAid0=	 0xEC,
	 ISAid1=	 0x13,
	ISActl1=	1,		/* board control */
	 ISAien=	 1<<7,		/*  interrupt enable */
	 ISAirq=	 7<<4,		/*  mask for irq code */
	 ISAdl=		 1<<1,		/*  download bit (1 == download) */
	 ISApr=		 1<<0,		/*  program ready */
	ISActl2=	2,		/* board control */
	 ISA186ien=	 1<<7,		/*  I186 irq enable bit state */
	 ISA186idata=	 1<<6,		/*  I186 irq data bit state */
	 ISAmen=	 1<<4,		/*  enable memory to respond to ISA cycles */
	 ISAmbank=	 0,		/*  shift for 4 bit memory bank */
	ISAmaddr=	3,		/* bits 14-19 of the boards mem address */
	ISAstat1=	4,		/* board status (1 bit per channel) */
	ISAstat2=	5,		/* board status (1 bit per channel) */

	Maxcard=	8,
	Pramsize=	64*1024,	/* size of program ram */
	Footshift=	14,		/* footprint of card mem in ISA space */
	Footprint=	1<<Footshift,
};

/* IRQ codes */
static int isairqcode[16] =
{
	-1,	-1,	-1,	0<<4,
	1<<4,	2<<4,	-1,	-1,
	-1,	3<<4,	4<<4,	5<<4,
	6<<4,	-1,	-1,	7<<4,
};

/* control program global control block */
typedef struct GCB GCB;
struct GCB
{
	ushort	cmd;		/* command word */
	ushort	status;		/* status word */
	ushort	serv;		/* service request, must be accessed via exchange 'X' */
	ushort	avail;		/* available buffer space */
	ushort	type;		/* board type */
	ushort	cpvers;		/* control program version */
	ushort	ccbc;		/* control channel block count */
	ushort	ccbo;		/* control channel block offset */
	ushort	ccbc;		/* control channel block size */
	ushort	cmd2;		/* command word 2 */
	ushort	status2;	/* status word 2 */
	ushort	errserv;	/* comm error service request 'X' */
	ushort	inserv;		/* input buffer service request 'X' */
	ushort	outserv;	/* output buffer service request 'X' */
	ushort	modemserv;	/* modem change service request 'X' */
	ushort	cmdserv;	/* channel command service request 'X' */
};

enum
{
	/* GCB.cmd commands/codes */
	Greadycmd=	0,
	Gdiagcmd=	1,
	Gresetcmd=	2,

	/* GCB.status values */
	Gready=		0,
	Gstopped=	1,
	Gramerr=	2,
	Gbadcmd=	3,
	Gbusy=		4,

	/* GCB.type values */
	Gx00m=		0x6,
	G100e=		0xA,
	Gx00i=		0xC,

	/* GCB.status2 bits */
	Ghas232=	(1<<0),
	Ghas422=	(1<<1),
	Ghasmodems=	(1<<2),
	Ghasrj11s=	(1<<7),
	Ghasring=	(1<<8),
	Ghasdcd=	(1<<9),
	Ghasdtr=	(1<<10),
	Ghasdsr=	(1<<11),
	Ghascts=	(1<<12),
	Ghasrts=	(1<<13),
};

/* control program channel control block */
typedef struct CCB CCB;
struct CCB
{
	ushort	baud;		/* baud rate */
	ushort	format;		/* data format */
	ushort	proto;		/* line protocol */
	ushort	insize;		/* input buffer size */
	ushort	outsize;	/* output buffer size */
	ushort 	intrigger;	/* input buffer trigger rate */
	ushort	outlow;		/* output buffer low water mark */
	char	xon[2];		/* xon characters */
	ushort	inhigh;		/* input buffer high water mark */
	ushort	inlow;		/* input buffer low water mark */
	ushort	cmd;		/* channel command */
	ushort	status;		/* channel status */
	ushort	inbase;		/* input buffer start addr */
	ushort 	inlim;		/* input buffer ending addr */
	ushort	outbase;	/* output buffer start addr */
	ushort 	outlim;		/* output buffer ending addr */
	ushort	inwp;		/* input read and write pointers */
	ushort	inrp;
	ushort	outwp;		/* output read and write pointers */
	ushort	outrp;
	ushort	errstat;	/* error status */
	ushort	badp;		/* bad character pointer */
	ushort	mctl;		/* modem control */
	ushort	mstat;		/* modem status */
	ushort	bstat;		/* blocking status */
	ushort	rflag;		/* character received flag */
	char	xoff[2];	/* xoff characters */
	ushort	status2;
	char	strip[2];	/* strip/error characters */
};

enum
{
	/* special baud rate codes for CCB.baud */
	Cb76800=	0xff00,
	Cb115200=	0xff01,

	/* CCB.format fields */
	C5bit=		0<<0,	/* data bits */
	C6bit=		0<<1,
	C7bit=		0<<2,
	C8bit=		0<<3,
	C1stop=		0<<2,	/* stop bits */
	C2stop=		1<<2,
	Cnopar=		0<<3,	/* parity */
	Coddpar=	1<<3,
	Cevenpar=	3<<3,
	Cmarkpar=	5<<3,
	Cspacepar=	7<<3,
	Cnormal=	0<<6,	/* normal mode */
	Cecho=		1<<6,	/* echo mode */
	Clloop=		2<<6,	/* local loopback */
	Crloop=		3<<6,	/* remote loopback */

	/* CCB.proto fields */
	Cobeyxon=	1<<0,	/* obey received xoff/xon controls */
	Canyxon=	1<<1,	/* any rcvd character restarts xmit */
	Cgenxon=	1<<2,	/* generate xoff/xon controls */
	Cobeycts=	1<<3,	/* obey hardware flow ctl */
	Cgendtr=	1<<4,	/* dtr off when uart rcvr full */
	C½duplex=	1<<5,	/* rts off while xmitting */
	Cgenrts=	1<<6,	/* generate hardware flow ctl */
	Cmctl=		1<<7,	/* direct modem control via CCB.mctl */
	Cstrip=		1<<12,	/* to strip out characters */
	Ceia422=	1<<13,	/* to select eia 422 lines */

	/* CCB.cmd fields */
	Cconfall=	1<<0,	/* configure channel and UART */
	Cconfchan=	1<<1,	/* configure just channel */
	Cflushin=	1<<2,	/* flush input buffer */
	Cflushout=	1<<3,	/* flush output buffer */
	Crcvena=	1<<4,	/* enable receiver */
	Crcvdis=	1<<5,	/* disable receiver */
	Cxmtena=	1<<6,	/* enable transmitter */
	Cxmtdis=	1<<7,	/* disable transmitter */
	Cmreset=	1<<9,	/* reset modem */

	/* CCB.errstat fields */
	Coverrun=	1<<0,
	Cparity=	1<<1,
	Cframing=	1<<2,
	Cbreak=		1<<3,

	/* CCB.mctl fields */
	Cdtrctl=	1<<0,
	Crtsctl=	1<<1,
	Cbreakctl=	1<<4,


	/* CCB.mstat fields */
	Cctsstat=	1<<0,
	Cdsrstat=	1<<1,
	Cristat=	1<<2,
	Cdcdstat=	1<<3,

	/* CCB.bstat fields */
	Cbrcvoff=	1<<0,
	Cbxmtoff=	1<<1,
	Clbxoff=	1<<2,	/* transmitter blocked by XOFF */
	Clbcts=		1<<3,	/* transmitter blocked by CTS */
	Crbxoff=	1<<4,	/* remote blocked by xoff */
	Crbrts=		1<<4,	/* remote blocked by rts */
};

/* host per controller info */
struct Astar
{
	QLock;

	ISAConf;
	int		id;		/* from plan9.ini */
	int		nchan;		/* number of channels */
	Rendez		r;
	Astarchan	*c;		/* channels */
	int		ramsize;	/* 16k or 256k */
	GCB		*gbc;		/* board comm area */
	char		*addr;		/* memory area */
};

/* host per channel info */
struct Astarchan
{
	QLock;

	Astar	*a;	/* controller */
	CCB	*ccb;	/* control block */
	int	perm;
	int	opens;

	/* buffers */
	Queue	*iq;
	Queue	*oq;
};

Astar *astar[Maxcard];
static int nastar;

enum
{
	Qmem=	0,
	Qbctl,
	Qdata,
	Qctl,
	Qstat,
};
#define TYPE(x)		((x)&0xff)
#define BOARD(x)	((((x)&~CHDIR)>>16)&0xff)
#define CHAN(x)		((((x)&~CHDIR)>>8)&0xff)
#define QID(b,c,t)	(((b)<<16)|((c)<<8)|(t))

static int astarsetup(Astar*);

int
astargen(Chan *c, Dirtab *tab, int ntab, int i, Dir *db)
{
	int dev, sofar, ch, t;
	extern ulong kerndate;

	USED(tab, ntab);
	sofar = 0;

	for(dev = 0; dev < nastar; dev++){
		if(sofar == i){
			sprint(db->name, "atar%dmem", astar[dev]->id);
			db->qid.path = QID(dev, 0, Qmem);
			db->mode = 0660;
			break;
		}
		sofar++;

		if(sofar == i){
			sprint(db->name, "atar%dctl", astar[dev]->id);
			db->qid.path = QID(dev, 0, Qbctl);
			db->mode = 0660;
			break;
		}
		sofar++;

		if(i - sofar < 3*astar[dev]->nchan){
			i -= sofar;
			ch = i/3;
			t = i%3;
			switch(t){
			case 0:
				sprint(db->name, "eia%d%2.2d", dev, ch);
				db->mode = astar[dev]->c[ch].perm;
				db->qid.path = QID(dev, 0, Qdata);
				break;
			case 1:
				sprint(db->name, "eia%d%2.2dctl", dev, ch);
				db->mode = astar[dev]->c[ch].perm;
				db->qid.path = QID(dev, 0, Qctl);
				break;
			case 2:
				sprint(db->name, "eia%d%2.2dstat", dev, ch);
				db->mode = 0444;
				db->qid.path = QID(dev, 0, Qstat);
				break;
			}
			break;
		}
		sofar += 3*astar[dev]->nchan;
	}

	if(dev == nastar)
		return -1;

	db->atime = seconds();
	db->mtime = kerndate;
	db->hlength = 0;
	db->length = 0;				/* update ???? */
	memmove(db->uid, eve, NAMELEN);
	memmove(db->gid, eve, NAMELEN);
	db->type = devchar[c->type];
	db->dev = c->dev;
	if(c->flag&CMSG)
		db->mode |= CHMOUNT;

	return 1;
}

void
astarreset(void)
{
	int i;
	Astar *a;

	for(i = 0; i < Maxcard; i++){
		a = astar[nastar] = xalloc(sizeof(Astar));
		if(isaconfig("serial", i, a) == 0){
			xfree(a);
			astar[nastar] = 0;
			break;
		}

		if(strcmp(a->type, "a100i") == 0 || strcmp(a->type,"A100I") == 0)
			a->ramsize = 16*1024;
		else if(strcmp(a->type, "a200i") == 0 || strcmp(a->type,"A200I") == 0)
			a->ramsize = 256*1024;
		else
			continue;

		if(a->mem == 0)
			a->mem = 0xD4000;
		if(a->irq == 0)
			a->irq = 15;
		a->id = i;

		if(astarsetup(a) < 0){
			xfree(a);
			astar[nastar] = 0;
			continue;
		}
		print("serial%d avanstar addr %lux irq %d\n", i, a->addr, a->irq);
		nastar++;
	}
}

/* isa ports an ax00i can appear at */
int isaport[] = { 0x200, 0x208, 0x300, 0x308, 0x600, 0x608, 0x700, 0x708, 0 };

static int
astarprobe(int port)
{
	uchar c, c1;

	if(port < 0)
		return 0;

	c = inb(port + ISAid);
	c1 = inb(port + ISAid);
	return (c == ISAid0 && c1 == ISAid1)
		|| (c == ISAid1 && c1 == ISAid0);
}

static int
astarsetup(Astar *a)
{
	int i, found;

	/* see if the card exists */
	found = 0;
	if(a->port == 0)
		for(i = 0; isaport[i]; i++){
			a->port = isaport[i];
			found = astarprobe(isaport[i]);
			if(found){
				isaport[i] = -1;
				break;
			}
		}
	else
		found = astarprobe(a->port);
	if(!found){
		print("avanstar %d not found\n", a->id);
		return -1;
	}

	/* set memory address */
	outb(a->port + ISAmaddr, (a->mem>>12) & 0xfc);
	a->gbc = (GCB*)(KZERO | a->mem);
	a->addr = (char*)(KZERO | a->mem);

	/* set interrupt level */
	if(isairqcode[a->irq] == -1){
		print("Avanstar %d bad irq %d\n", a->id, a->irq);
		return -1;
	}

	return 0;
}

void
astarinit(void)
{
}

Chan*
astarattach(char *spec)
{
	return devattach('g', spec);
}

Chan*
astarclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
astarwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, astargen);
}

void
astarstat(Chan *c, char *dp)
{
	devstat(c, dp, 0, 0, astargen);
}

Chan*
astaropen(Chan *c, int omode)
{
	Astar *a;

	c = devopen(c, omode, 0, 0, astargen);

	switch(TYPE(c->qid.path)){
	case Qmem:
	case Qbctl:
		if(!iseve())
			error(Eperm);
		break;
	}

	return c;
}

void
astarclose(Chan *c)
{
	Astar *a;
	int i;

	switch(TYPE(c->qid.path)){
	case Qmem:
		a = astar[BOARD(c->qid.path)];
		qlock(a);
		if(--a->opens == 0){
			/* take board out of download mode and enable IRQ */
			outb(a->port + ISActl1, ISAien|isairqcode[a->irq]);

			/* enable ISA access to first 16k */
			outb(a->port + ISActl2, ISAmen|0);

			/* wait for program ready */
			for(i = 0; i < 21; i++){
				if(inb(a->port + ISActl1) & ISApr)
					break;
				tsleep(&r, return0, 0, 500);
			}
			if((inb(a->port + ISActl1) & ISApr) == 0)
				print("astar%d program not ready\n", a->id);
		}
		qunlock(a);
		break;
	}
}

long
astarread(Chan *c, void *buf, long n, ulong offset)
{
	int i;
	Astar *a;
	char *to, *from, *e;
	char status[128];

	if(c->qid.path & CHDIR)
		return devdirread(c, buf, n, 0, 0, astargen);

	switch(TYPE(c->qid.path)){
	case Qmem:
		a = astar[BOARD(c->qid.path)];
		if(offset+n > Pramsize){
			if(offset >= Pramsize)
				return 0;
			n = Pramsize - offset;
		}

		if(waserror()){
			qunlock(a);
			nexterror();
		}
		qlock(a);
		from = buf;
		while(n > 0){
			/* map in right piece of memory */
			outb(a->port + ISActl2, ISAmen|(offset>>Footshift));
			i = offset%Footprint;
			to = a->addr + i;
			i = Footprint - i;
			if(i > n)
				i = n;
			
			/* byte at a time so endian doesn't matter */
			for(e = from + i; from < e;)
				*to++ = *from++;

			n -= i;
		}
		qunlock(a);
		poperror();
		break;
	case Qbctl:
		a = astar[BOARD(c->qid.path)];
		sprint(status, "id %2.2ux%2.2ux ctl1 %2.2ux ctl2 %2.2ux maddr %2.2ux stat %2.2ux%2.2ux",
			inb(a->port+ISAid), inb(a->port+ISAid),
			inb(a->port+ISActl1), inb(a->port+ISActl2), 
			inb(a->port+ISAmaddr),
			inb(a->port+ISAstat2), inb(a->port+ISAstat1));
		n = readstr(offset, buf, n, status);
		break;
	}

	return 0;
}

long
astarwrite(Chan *c, void *buf, long n, ulong offset)
{
	Astar *a;
	char *to, *from, *e;
	int i;
	char cmsg[32];

	if(c->qid.path & CHDIR)
		error(Eperm);

	switch(TYPE(c->qid.path)){
	case Qmem:
		a = astar[BOARD(c->qid.path)];
		if(offset+n > Pramsize){
			if(offset >= Pramsize)
				return 0;
			n = Pramsize - offset;
		}

		if(waserror()){
			qunlock(a);
			nexterror();
		}
		qlock(a);
		to = buf;
		while(n > 0){
			/* map in right piece of memory */
			outb(a->port + ISActl2, ISAmen|(offset>>Footshift));
			i = offset%Footprint;
			from = a->addr + i;
			i = Footprint - i;
			if(i > n)
				i = n;
			
			/* byte at a time so endian doesn't matter */
			for(e = from + i; from < e;)
				*to++ = *from++;

			n -= i;
		}
		qunlock(a);
		poperror();
		break;
	case Qbctl:
		if(n > sizeof cmsg)
			n = sizeof(cmsg) - 1;
		memmove(cmsg, buf, n);
		cmsg[n] = 0;

		if(waserror()){
			qunlock(a);
			nexterror();
		}
		qlock(a);
		if(strncmp(cmsg, "download", 8) == 0){
			/* put board in download mode */
			outb(a->port + ISActl1, ISAdl);

			/* enable ISA access to first 16k */
			outb(a->port + ISActl2, ISAmen);

		} else if(strncmp(cmsg, "run", 3) == 0){
			/* take board out of download mode and enable IRQ */
			outb(a->port + ISActl1, ISAien|isairqcode[a->irq]);

			/* enable ISA access to first 16k */
			outb(a->port + ISActl2, ISAmen);

			/* wait for control program to signal life */
			for(i = 0; i < 21; i++){
				if(inb(a->port + ISActl1) & ISApr)
					break;
				tsleep(&a->r, return0, 0, 500);
			}
			if((inb(a->port + ISActl1) & ISApr) == 0)
				print("astar%d program not ready\n", a->id);

		} else
			error(Ebadarg);
		qunlock(a);
		poperror();
		break;
	}

	return 0;
}

void
astarcreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
astarremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
astarwstat(Chan *c, char *dp)
{
	USED(c, dp);
	error(Eperm);
}
