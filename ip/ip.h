typedef struct	Conv	Conv;
typedef struct	Fs	Fs;
typedef union	Hwaddr	Hwaddr;
typedef struct	Ifcconv	Ifcconv;
typedef struct	Ipself	Ipself;
typedef struct	Iplink	Iplink;
typedef struct	Iplifc	Iplifc;
typedef struct	Ipmulti	Ipmulti;
typedef struct	Iproute	Iproute;
typedef struct	Ipifc	Ipifc;
typedef struct	Medium	Medium;
typedef struct	Proto	Proto;
typedef struct	Pstate	Pstate;
typedef struct	Tcpc	Tcpc;
typedef struct	Arpent	Arpent;
typedef struct	Route	Route;

enum
{
	Addrlen=	64,
	Maxproto=	20,
	Nhash=		64,
	Maxincall=	5,
	Nchans=		256,
	MAClen=		16,		/* longest mac address */

	MAXTTL=		255,

	IPaddrlen=	16,
	IPv4addrlen=	4,
	IPv4off=	12,
	IPllen=		4,

	/* ip versions */
	V4=		4,
	V6=		6,
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

	uchar	laddr[IPaddrlen];	/* local IP address */
	uchar	raddr[IPaddrlen];	/* remote IP address */
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

	Ipmulti	*multi;			/* multicast bindings for this interface */

	void*	ptcl;			/* protocol specific stuff */
};

struct Medium
{
	char	*name;
	int	hsize;		/* medium header size */
	int	minmtu;		/* default min mtu */
	int	maxmtu;		/* default max mtu */
	int	maclen;		/* mac address length  */
	void	(*bind)(Ipifc*, int, char**);
	void	(*unbind)(Ipifc*);
	void	(*bwrite)(Ipifc *ifc, Block *b, int version, uchar *ip);

	/* for arming interfaces to receive multicast */
	void	(*addmulti)(Ipifc *ifc, uchar *a, uchar *ia);
	void	(*remmulti)(Ipifc *ifc, uchar *a, uchar *ia);

	/* process packets written to 'data' */
	void	(*pktin)(Ipifc *ifc, Block *bp);

	/* routes for router boards */
	void	(*addroute)(Ipifc *ifc, int, uchar*, uchar*, uchar*, int);
	void	(*remroute)(Ipifc *ifc, int, uchar*, uchar*);
	void	(*flushroutes)(Ipifc *ifc);

	/* for routing multicast groups */
	void	(*joinmulti)(Ipifc *ifc, uchar *a, uchar *ia, uchar **iap);
	void	(*leavemulti)(Ipifc *ifc, uchar *a, uchar *ia);

	int	unbindonclose;	/* if non-zero, unbind on last close */
};

/* logical interface associated with a physical one */
struct Iplifc
{
	uchar	local[IPaddrlen];
	uchar	mask[IPaddrlen];
	uchar	remote[IPaddrlen];
	uchar	net[IPaddrlen];
	Iplink	*link;		/* addresses linked to this lifc */
	Iplifc	*next;
};

/* binding twixt Ipself and Ipifc */
struct Iplink
{
	Ipself	*self;
	Iplifc	*lifc;
	Iplink	*selflink;	/* next link for this local address */
	Iplink	*lifclink;	/* next link for this ifc */
	ulong	expire;
	Iplink	*next;		/* free list */
	int	ref;
};

struct Ipifc
{
	RWlock;
	
	Conv	*conv;		/* link to its conversation structure */
	char	dev[64];	/* device we're attached to */
	Medium	*m;		/* Media pointer */
	int	maxmtu;		/* Maximum transfer unit */
	int	minmtu;		/* Minumum tranfer unit */
	void	*arg;		/* medium specific */

	/* these are used so that we can unbind on the fly */
	Lock	idlock;
	uchar	ifcid;		/* incremented each 'bind/unbind/add/remove' */
	int	ref;		/* number of proc's using this ipifc */
	Rendez	wait;		/* where unbinder waits for ref == 0 */
	int	unbinding;

	uchar	mac[MAClen];	/* MAC address */

	Iplifc	*lifc;		/* logical interfaces on this physical one */

	ulong	in, out;	/* message statistics */
	ulong	inerr, outerr;	/* ... */
};

/*
 *  one per multicast-lifc pair used by a Conv
 */
struct Ipmulti
{
	uchar	ma[IPaddrlen];
	uchar	ia[IPaddrlen];
	Ipmulti	*next;
};

/*
 *  one per multiplexed protocol
 */
struct Proto
{
	QLock;
	char*		name;		/* protocol name */
	int		x;		/* protocol index */
	int		ipproto;	/* ip protocol type */

	void		(*kick)(Conv*, int);
	char*		(*connect)(Conv*, char**, int);
	char*		(*announce)(Conv*, char**, int);
	char*		(*bind)(Conv*, char**, int);
	int		(*state)(Conv*, char*, int);
	void		(*create)(Conv*);
	void		(*close)(Conv*);
	void		(*rcv)(uchar*, Block*);
	char*		(*ctl)(Conv*, char**, int);
	void		(*advise)(Block*, char*);
	int		(*stats)(char*, int);
	int		(*local)(Conv*, char*, int);
	int		(*inuse)(Conv*);

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
};

struct Fs
{
	Lock;

	int	np;
	Proto*	p[Maxproto+1];		/* list of supported protocols */
	Proto*	t2p[256];		/* vector of all ip protocol handlers */
};
int	Fsconnected(Fs*, Conv*, char*);
Conv*	Fsnewcall(Fs*, Conv*, uchar*, ushort, uchar*, ushort);
int	Fspcolstats(char*, int);
int	Fsproto(Fs*, Proto*);
int	Fsbuiltinproto(Fs*, uchar);
Conv*	Fsprotoclone(Proto*, char*);
Proto*	Fsrcvpcol(Fs*, uchar);
char*	Fsstdconnect(Conv*, char**, int);
char*	Fsstdannounce(Conv*, char**, int);
char*	Fsstdbind(Conv*, char**, int);

/* 
 *  logging
 */
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
extern uchar	iponly[IPaddrlen];		/* ip address to print debugging for */
extern int	iponlyset;

void netlogopen(void);
void netlogclose(void);
char* netlogctl(char*, int);
long netlogread(void*, ulong, long);
void netlog(int, char*, ...);

/*
 *  iproute.c
 */
typedef	struct RouteTree RouteTree;
typedef struct Routewalk Routewalk;
typedef struct V4route V4route;
typedef struct V6route V6route;

enum
{

	/* type bits */
	Rv4=		(1<<0),		/* this is a version 4 route */
	Rifc=		(1<<1),		/* this route is a directly connected interface */
	Rptpt=		(1<<2),		/* this route is a pt to pt interface */
	Runi=		(1<<3),		/* a unicast self address */
	Rbcast=		(1<<4),		/* a broadcast self address */
	Rmulti=		(1<<5),		/* a multicast self address */
};

struct Routewalk {
	int	n;
	int	o;
	int	h;
	char*	p;
	void*	state;
	void	(*walk)(Route*, Routewalk*);
};

struct	RouteTree
{
	Route*	right;
	Route*	left;
	Route*	mid;
	uchar	depth;
	uchar	type;
	uchar	ifcid;		/* must match ifc->id */
	Ipifc	*ifc;
	char	tag[4];
};

struct V4route
{
	ulong	address;
	ulong	endaddress;
	uchar	gate[IPv4addrlen];
};

struct V6route
{
	ulong	address[IPllen];
	ulong	endaddress[IPllen];
	uchar	gate[IPaddrlen];
};

struct Route
{
	RouteTree;

	union {
		V6route	v6;
		V4route v4;
	};
};
extern void	v4addroute(char *tag, uchar *a, uchar *mask, uchar *gate, int type);
extern void	v6addroute(char *tag, uchar *a, uchar *mask, uchar *gate, int type);
extern void	v4delroute(uchar *a, uchar *mask);
extern void	v6delroute(uchar *a, uchar *mask);
extern Route*	v4lookup(uchar *a);
extern Route*	v6lookup(uchar *a);
extern long	routeread(char*, ulong, int);
extern long	routewrite(Chan*, char*, int);
extern void	routetype(int, char*);

/*
 *  arp.c
 */
struct Arpent
{
	uchar	ip[IPaddrlen];
	uchar	mac[MAClen];
	Medium	*type;		/* media type */
	Arpent*	hash;
	Block*	hold;
	Block*	last;
	uint	time;
	uint	used;
	uchar	state;
};

extern int	arpread(char*, ulong, int);
extern int	arpwrite(char*, int);
extern Arpent*	arpget(Block *bp, int version, Medium *type, uchar *ip, uchar *h);
extern void	arprelease(Arpent *a);
extern Block*	arpresolve(Arpent *a, Medium *type, uchar *mac);
extern void	arpenter(Ipifc *ifc, int version, uchar *ip, uchar *mac, Medium *type, int norefresh);

/*
 * ipaux.c
 */

typedef struct Cmdbuf	Cmdbuf;
struct Cmdbuf
{
	char	buf[64];
	char	*f[16];
	int	nf;
};

extern int	myetheraddr(uchar*, char*);
extern ulong	parseip(uchar*, char*);
extern ulong	parseipmask(uchar*, char*);
extern void	maskip(uchar *from, uchar *mask, uchar *to);
extern int	parsemac(uchar *to, char *from, int len);
extern uchar*	defmask(uchar*);
extern int	isv4(uchar*);
extern void	v4tov6(uchar *v6, uchar *v4);
extern int	v6tov4(uchar *v4, uchar *v6);
extern Cmdbuf*	parsecmd(char *a, int n);
extern int	eipconv(va_list *arg, Fconv *f);

#define	ipcmp(x, y) memcmp(x, y, IPaddrlen)
#define	ipmove(x, y) memmove(x, y, IPaddrlen)

extern uchar IPv4bcast[IPaddrlen];
extern uchar IPv4bcastobs[IPaddrlen];
extern uchar IPv4allsys[IPaddrlen];
extern uchar IPv4allrouter[IPaddrlen];
extern uchar IPnoaddr[IPaddrlen];
extern uchar v4prefix[IPaddrlen];
extern uchar IPallbits[IPaddrlen];

#define	msec	TK2MS(MACHP(0)->ticks)

/*
 *  media
 */
extern Medium	ethermedium;
extern Medium	nullmedium;
extern Medium	pktmedium;
extern Proto	ipifc;	

/*
 *  ipifc.c
 */
extern Medium*	ipfindmedium(char *name);
extern int	ipforme(uchar *addr);
extern int	ipismulticast(uchar *);
extern Ipifc*	findipifc(uchar *remote, int type);
extern void	findlocalip(uchar *local, uchar *remote);
extern int	ipv4local(Ipifc *ifc, uchar *addr);
extern int	ipv6local(Ipifc *ifc, uchar *addr);
extern Iplifc*	iplocalonifc(Ipifc *ifc, uchar *ip);
extern int	ipproxyifc(Ipifc *ifc, uchar *ip);
extern int	ipismulticast(uchar *ip);
extern int	ipisbooting(void);
extern int	ipifccheckin(Ipifc *ifc, Medium *med);
extern void	ipifccheckout(Ipifc *ifc);
extern int	ipifcgrab(Ipifc *ifc);
extern void	ipifcaddroute(int, uchar*, uchar*, uchar*, int);
extern void	ipifcremroute(int, uchar*, uchar*);
extern void	ipifcremmulti(Conv *c, uchar *ma, uchar *ia);
extern void	ipifcaddmulti(Conv *c, uchar *ma, uchar *ia);
extern char*	ipifcrem(Ipifc *ifc, char **argv, int argc, int dolock);
extern char*	ipifcadd(Ipifc *ifc, char **argv, int argc);
extern long	ipselftabread(char *a, ulong offset, int n);

/*
 *  ip.c
 */
extern void	closeifcconv(Ifcconv*);
extern void	icmpnoconv(Block*);
extern void	initfrag(int);
extern ushort	ipcsum(uchar*);
extern void	(*ipextprotoiput)(Block*);
extern void	ipiput(uchar*, Block*);
extern void	ipoput(Block*, int, int);
extern int	ipstats(char*, int);
extern uchar*	logctl(uchar*);
extern Ifcconv* newifcconv(void);
extern void	(*pktifcrcv)(Conv*, Block*);
extern ushort	ptclbsum(uchar*, int);
extern ushort	ptclcsum(Block*, int, int);

/*
 *  iprouter.c
 */
void	useriprouter(uchar*, Block*);
void	iprouteropen(void);
void	iprouterclose(void);
long	iprouterread(void*, int);

/*
 *  global to all of the stack
 */
extern int	debug;
extern Fs	fs;
extern int	iprouting;	/* true if routing turned on */
extern void	(*igmpreportfn)(Ipifc*, uchar*);
