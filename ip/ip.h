typedef	ulong	Ipaddr;
typedef uchar	byte;
typedef struct	Conv	Conv;
typedef struct	Fs	Fs;
typedef union	Hwaddr	Hwaddr;
typedef struct	Ifcconv	Ifcconv;
typedef struct	Iproute	Iproute;
typedef struct	Media	Media;
typedef struct	Multicast	Multicast;
typedef struct	Proto	Proto;
typedef struct	Pstate	Pstate;
typedef struct	Tcpc	Tcpc;

enum
{
	Addrlen=	64,
	Maxproto=	20,
	Nhash=		64,
	Maxincall=	5,
	Nchans=		256,

	MAXTTL=		255,

	IPaddrlen=	4,
	Ipbcast=	0xffffffff,	/* ip broadcast address */
	Ipbcastobs=	0,		/* obsolete (but still used) ip broadcast addr */
	Ipallsys=	0xe0000001,	/* multicast for all systems */
	Ipallrouter=	0xe0000002,	/* multicast for all routers */
};

enum
{
	Announcing=	1,
	Announced=	2,
	Connecting=	3,
	Connected=	4,
};

/*
 *  contained in each conversation
 */
struct Conv
{
	Lock;

	int	x;			/* conversation index */
	Proto*	p;

	Ipaddr	laddr;			/* local IP address */
	Ipaddr	raddr;			/* remote IP address */
	int	restricted;		/* remote port is restricted */
	ushort	lport;			/* local port number */
	ushort	rport;			/* remote port number */
	uint	ttl;			/* max time to live */

	char	owner[NAMELEN];		/* protections */
	int	perm;
	int	inuse;			/* opens of listen/data/ctl */
	int	length;
	int	state;

	/* udp specific */
	int	headers;		/* data src/dst headers in udp */
	int	reliable;		/* true if reliable udp */

	Conv*	incall;			/* calls waiting to be listened for */
	Conv*	next;

	Queue*	rq;			/* queued data waiting to be read */
	Queue*	wq;			/* queued data waiting to be written */
	Queue*	eq;			/* returned error packets */

	QLock	car;
	Rendez	cr;
	char	cerr[ERRLEN];

	QLock	listenq;
	Rendez	listenr;

	void*	ptcl;			/* protocol specific stuff */
};

union Hwaddr
{
	byte	ether[6];
};

enum
{
	METHER,			/* Media types */
	MFDDI,
	MPACKET,
};

/* one per multicast address per medium */
struct Multicast
{
	Ipaddr	addr;
	int	ref;
	int	timeout;
	Multicast *next;
};

struct Media
{
	int	type;		/* Media type */
	Chan*	mchan;		/* Data channel */
	Chan*	achan;		/* Arp channel */
	Chan*	cchan;		/* Control channel */
	char*	dev;		/* device mfd points to */
	Ipaddr	myip[5];
	Ipaddr	mymask;
	Ipaddr	mynetmask;
	Ipaddr	remip;		/* Address of remote side */
	byte	netmyip[4];	/* In Network byte order */
	int	arping;		/* true if we mus arp */
	int	maxmtu;		/* Maximum transfer unit */
	int	minmtu;		/* Minumum tranfer unit */
	int	hsize;		/* Media header size */
	Hwaddr;
	ulong	in, out;	/* message statistics */
	ulong	inerr, outerr;	/* ... */
	int	inuse;
	Conv	*c;		/* for packet interface */

	QLock	mlock;		/* lock for changing *multi */
	Multicast *multi;	/* list of multicast addresses we're listening to */
	int	mactive;	/* number of active multicast addresses */

	Media*	link;
};
int	Mediaforme(byte*);
int	Mediaforpt2pt(byte*);
Ipaddr	Mediagetsrc(byte*);
void	Mediaclose(Media*);
char*	Mediaopen(int, char*, Conv*, Ipaddr, Ipaddr, Ipaddr, int, Media**);
Media*	Mediaroute(byte*, byte*);
void	Mediasetaddr(Media*, Ipaddr, Ipaddr);
void	Mediasetraddr(Media*, Ipaddr);
Ipaddr	Mediagetaddr(Media*);
Ipaddr	Mediagetraddr(Media*);
void	Mediawrite(Media*, Block*, byte*);
int	Mediaifcread(char*, ulong, int);
char*	Mediaifcwrite(Ifcconv*, char*, int);
void	Mediaresolver(Media*);
void	Mediaread(Media*);
int	Mediaarp(Media*, Block*, byte*, Hwaddr*);
Media*	Mediafind(Iproute*);
Multicast*	Mediacopymulti(Media*);
void	Mediamulticastadd(Media*, Ifcconv*, Ipaddr);
void	Mediamulticastrem(Media*, Ipaddr);

/*
 *  one per multiplexed protocol
 */
struct Proto
{
	Lock;
	char*		name;		/* protocol name */
	int		x;		/* protocol index */
	int		ipproto;	/* ip protocol type */

	void		(*kick)(Conv*, int);
	char*		(*connect)(Conv*, char**, int);
	char*		(*announce)(Conv*, char**, int);
	int		(*state)(char**, Conv*);
	void		(*create)(Conv*);
	void		(*close)(Conv*);
	void		(*rcv)(Media*, Block*);
	char*		(*ctl)(Conv*, char**, int);
	void		(*advise)(Block*, char*);

	Conv		**conv;		/* array of conversations */
	int		ptclsize;	/* size of per protocol ctl block */
	int		nc;		/* number of conversations */
	int		ac;
	Qid		qid;		/* qid for protocol directory */
	ushort		nextport;
	ushort		nextrport;

	ulong		csumerr;		/* checksum errors */
	ulong		hlenerr;		/* header length error */
	ulong		lenerr;			/* short packet */
	ulong		order;			/* out of order */
	ulong		rexmit;			/* retransmissions */
	ulong		wclosed;		/* window closed */
};

struct Fs
{
	Lock;

	int	np;
	Proto*	p[Maxproto+1];		/* list of supported protocols */
	Proto*	t2p[256];		/* vector of all ip protocol handlers */
};
int	Fsconnected(Fs*, Conv*, char*);
Conv*	Fsnewcall(Fs*, Conv*, Ipaddr, ushort, Ipaddr, ushort);
int	Fspcolstats(char*, int);
int	Fsproto(Fs*, Proto*);
int	Fsbuiltinproto(Fs*, byte);
Conv*	Fsprotoclone(Proto*, char*);
Proto*	Fsrcvpcol(Fs*, byte);
char*	Fsstdconnect(Conv*, char**, int);
char*	Fsstdannounce(Conv*, char**, int);

/* log flags */
enum
{
	Logip=		1<<1,
	Logtcp=		1<<2,
	Logfs=		1<<3,
	Logil=		1<<4,
	Logicmp=	1<<5,
	Logudp=		1<<6,
	Logcompress=	1<<7,
	Logilmsg=	1<<8,
	Loggre=		1<<9,
	Logppp=		1<<10,
	Logtcpmsg=	1<<11,
	Logigmp=	1<<12,
	Logudpmsg=	1<<13,
	Logipmsg=	1<<14,
};

extern int	logmask;	/* mask of things to debug */
extern Ipaddr	iponly;		/* ip address to print debugging for */

void netlogopen(void);
void netlogclose(void);
char* netlogctl(char*, int);
long netlogread(void*, ulong, long);
void netlog(int, char*, ...);

#define	msec	TK2MS(MACHP(0)->ticks)

/* Globals */
extern int	debug;
extern Fs	fs;
extern Media*	media;
extern int	iprouting;	/* true if routing turned on */
extern void	(*igmpreportfn)(Media*, byte*);

int	arpread(byte*, ulong, int);
char*	arpwrite(char*, int);
void	closeifcconv(Ifcconv*);
Ipaddr	defmask(Ipaddr);
int	eipconv(va_list*, Fconv*);
int	equivip(byte*, byte*);
void	fatal(byte*, ...);
void	hnputl(byte*, ulong);
void	hnputs(byte*, ushort);
void	icmpnoconv(Block*);
void	initfrag(int);
ushort	ipcsum(byte*);
void	(*ipextprotoiput)(Block*);
Ipaddr	ipgetsrc(byte*);
void	ipiput(Media*, Block*);
void	ipoput(Block*, int, int);
int	ipstats(char*, int);
int	ismcast(byte*);
int	isbmcast(byte*);
byte*	logctl(byte*);
void	maskip(byte*, byte*, byte*);
Ifcconv* newifcconv(void);
ulong	nhgetl(byte*);
ushort	nhgets(byte*);
void	(*pktifcrcv)(Conv*, Block*);
ushort	ptclcsum(Block*, int, int);
int	pullblock(Block**, int);
Block*	pullupblock(Block*, int);
char*	routeadd(Ipaddr, Ipaddr, Ipaddr, Media *m);
void	routedelete(ulong, ulong, Media *m);
int	routeread(byte*, ulong, int);
char*	routewrite(char*, int);

/*
 * ipaux.c
 */
int	myetheraddr(uchar*, char*);
ulong	parseip(uchar*, char*);
