#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	"devtab.h"
#include	"../port/netif.h"

/*
 *  Stargate's Avanstar serial board.  There are ISA, EISA, microchannel versions.
 *  At the moment we only handle the ISA one.
 */
typedef struct Astar Astar;
typedef struct Astarport Astarchan;

enum
{
	/* ISA control ports */
	ISAid=		0,		/* Id port and its values */
	 ISAid0=	 0xEC,
	 ISAid1=	 0x13,
	ISActl1=	1,		/* board control */
	 ISAien=	 1<<7,		/*  interrupt enable */
	 ISAirqm=	 4,		/*  shift for 3 bit irq code */
	 ISAdl=		 1<<1,		/*  download bit (1 == download) */
	 ISApr=		 1<<0,		/*  program ready */
	ISActl2=	2,		/* board control */
	 ISA186ien=	 1<<7,		/*  I186 irq enable bit state */
	 ISA186idata=	 1<<6,		/*  I186 irq data bit state */
	 ISAmemen=	 1<<4,		/*  enable memory to respond to ISA cycles */
	 ISAmembank=	 0,		/*  shift for 4 bit memory bank */
	ISAmemaddr=	3,		/* bits 14-19 of the boards mem address */
	ISAstat1=	4,		/* board status (1 bit per channel) */
	ISAstat2=	5,		/* board status (1 bit per channel) */
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

/* control program channel control block */
typedef struct CCB CCB;
struct CCB
{
	ushort	baud;		/* baud rate */
	ushort	format;		/* data format */
	ushort	line;		/* line protocol */
	ushort	insize;		/* input buffer size */
	ushort	outsize;	/* output buffer size */
	ushort 	intrigger;	/* input buffer trigger rate */
	ushort	outlow;		/* output buffer low water mark */
	ushort	xon;		/* xon characters */
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
	ushort	xoff;		/* xoff characters */
	ushort	status2;
	uchort	strip;		/* strip/error characters */
};

/* host per controller info */
struct Astar
{
	int		port;		/* number of first port */
	GCB		*gbc;		/* board comm area */
	Astarchan	*c;		/* channels */
};

/* host per channel info */
struct Astarchan
{
	QLock;

	/* buffers */
	int	(*putc)(Queue*, int);
	Queue	*iq;
	Queue	*oq;


	/* staging areas to avoid some of the per character costs */
	uchar	istage[Stagesize];
	uchar	*ip;
	uchar	*ie;

	uchar	ostage[Stagesize];
	uchar	*op;
	uchar	*oe;
};

/*
 *  Stargate's Avanstar serial port cards.  Currently only the ISA version
 *  is supported.
 */
void
avanstarreset(void)
{
	int i;
	Dirtab *dp;

	for(i = 0; i < Maxcard; i++){
		sc = scard[i] = xalloc(sizeof(Scard));
		if(isaconfig("serial", i, sc) == 0){
			xfree(sc);
			break;
		}

	ndir = 3*nuart;
	ns16552dir = xalloc(ndir * sizeof(Dirtab));
	dp = ns16552dir;
	for(i = 0; i < nuart; i++){
		/* 3 directory entries per port */
		sprint(dp->name, "eia%d", i);
		dp->qid.path = NETQID(i, Ndataqid);
		dp->perm = 0660;
		dp++;
		sprint(dp->name, "eia%dctl", i);
		dp->qid.path = NETQID(i, Nctlqid);
		dp->perm = 0660;
		dp++;
		sprint(dp->name, "eia%dstat", i);
		dp->qid.path = NETQID(i, Nstatqid);
		dp->perm = 0444;
		dp++;
	}
}
