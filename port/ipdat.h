typedef struct Ipconv	Ipconv;
typedef struct Ipifc	Ipifc;
typedef struct Fragq	Fragq;
typedef struct Ipfrag	Ipfrag;
typedef ulong		Ipaddr;
typedef ushort		Port;
typedef struct Udphdr	Udphdr;
typedef struct Etherhdr	Etherhdr;
typedef struct Reseq	Reseq;
typedef struct Tcp	Tcp;
typedef struct Tcpctl	Tcpctl;
typedef struct Tcphdr	Tcphdr;
typedef struct Timer	Timer;
typedef struct Ilhdr	Ilhdr;
typedef struct Ilcb	Ilcb;

struct Etherhdr
{
#define ETHER_HDR	14
	uchar	d[6];
	uchar	s[6];
	uchar	type[2];

	/* Now we have the ip fields */
#define ETHER_IPHDR	20
	uchar	vihl;		/* Version and header length */
	uchar	tos;		/* Type of service */
	uchar	length[2];	/* packet length */
	uchar	id[2];		/* Identification */
	uchar	frag[2];	/* Fragment information */
	uchar	ttl;		/* Time to live */
	uchar	proto;		/* Protocol */
	uchar	cksum[2];	/* Header checksum */
	uchar	src[4];		/* Ip source */
	uchar	dst[4];		/* Ip destination */
};

/* Ethernet packet types */
#define ET_IP	0x0800

/* A userlevel data gram */
struct Udphdr
{
#define UDP_EHSIZE	22
	uchar	d[6];		/* Ethernet destination */
	uchar	s[6];		/* Ethernet source */
	uchar	type[2];	/* Ethernet packet type */

	uchar	vihl;		/* Version and header length */
	uchar	tos;		/* Type of service */
	uchar	length[2];	/* packet length */
	uchar	id[2];		/* Identification */
	uchar	frag[2];	/* Fragment information */

	/* Udp pseudo ip really starts here */
#define UDP_PHDRSIZE	12
#define UDP_HDRSIZE	20
	uchar	Unused;	
	uchar	udpproto;	/* Protocol */
	uchar	udpplen[2];	/* Header plus data length */
	uchar	udpsrc[4];	/* Ip source */
	uchar	udpdst[4];	/* Ip destination */
	uchar	udpsport[2];	/* Source port */
	uchar	udpdport[2];	/* Destination port */
	uchar	udplen[2];	/* data length */
	uchar	udpcksum[2];	/* Checksum */
};

struct Ilhdr
{
#define IL_EHSIZE	34
	uchar	d[6];		/* Ethernet destination */
	uchar	s[6];		/* Ethernet source */
	uchar	type[2];	/* Ethernet packet type */

	uchar	vihl;		/* Version and header length */
	uchar	tos;		/* Type of service */
	uchar	length[2];	/* packet length */
	uchar	id[2];		/* Identification */
	uchar	frag[2];	/* Fragment information */
	uchar	ttl;		/* Time to live */
	uchar	proto;		/* Protocol */
	uchar	cksum[2];	/* Header checksum */
	uchar	src[4];		/* Ip source */
	uchar	dst[4];		/* Ip destination */
#define IL_HDRSIZE	18	
	uchar	ilsum[2];	/* Checksum including header */
	uchar	illen[2];	/* Packet length */
	uchar	iltype;		/* Packet type */
	uchar	ilspec;		/* Special */
	uchar	ilsrc[2];	/* Src port */
	uchar	ildst[2];	/* Dst port */
	uchar	ilid[4];	/* Sequence id */
	uchar	ilack[4];	/* Acked sequence */
};

struct Ilcb				/* Control block */
{
	int	state;			/* Connection state */

	Rendez	syncer;			/* where syncer waits for a connect */

	QLock	ackq;			/* Unacknowledged queue */
	Block	*unacked;
	Block	*unackedtail;

	QLock	outo;			/* Out of order packet queue */
	Block	*outoforder;

	Lock	nxl;
	ulong	next;			/* Id of next to send */
	ulong	recvd;			/* Last packet received */
	ulong	start;			/* Local start id */
	ulong	rstart;			/* Remote start id */

	int	timeout;		/* Time out counter */
	int	slowtime;		/* Slow time counter */
	int	fasttime;		/* Retransmission timer */
	int	acktime;		/* Acknowledge timer */
	int	querytime;		/* Query timer */
	int	deathtime;		/* Time to kill connection */

	int	rtt;			/* Average round trip time */
	ulong	rttack;			/* The ack we are waiting for */
	ulong	ackms;			/* Time we issued */

	int	window;			/* Maximum receive window */
};

enum					/* Packet types */
{
	Ilsync,
	Ildata,
	Ildataquery,
	Ilack,
	Ilquerey,
	Ilstate,
	Ilclose,
};

enum					/* Connection state */
{
	Ilclosed,
	Ilsyncer,
	Ilsyncee,
	Ilestablished,
	Illistening,
	Ilclosing,
};

#define TCP_PKT	(TCP_EHSIZE+TCP_IPLEN+TCP_PHDRSIZE)

struct Tcphdr
{
#define TCP_EHSIZE	14
	uchar	d[6];		/* Ethernet destination */
	uchar	s[6];		/* Ethernet source */
	uchar	type[2];	/* Ethernet packet type */
#define TCP_IPLEN	8
	uchar	vihl;		/* Version and header length */
	uchar	tos;		/* Type of service */
	uchar	length[2];	/* packet length */
	uchar	id[2];		/* Identification */
	uchar	frag[2];	/* Fragment information */

#define TCP_PHDRSIZE	12	
	uchar	Unused;
	uchar	proto;
	uchar	tcplen[2];
	uchar	tcpsrc[4];
	uchar	tcpdst[4];

#define TCP_HDRSIZE	20
	uchar	tcpsport[2];
	uchar	tcpdport[2];
	uchar	tcpseq[4];
	uchar	tcpack[4];
	uchar	tcpflag[2];
	uchar	tcpwin[2];
	uchar	tcpcksum[2];
	uchar	tcpurg[2];

	/* Options segment */
	uchar	tcpopt[2];
	uchar	tcpmss[2];
};



struct Timer
{
	Timer	*next;
	Timer	*prev;
	int	state;
	int	start;
	int	count;
	void	(*func)(void*);
	void	*arg;
};

struct Tctl
{
	uchar	state;		/* Connection state */
	uchar	type;		/* Listening or active connection */
	uchar	code;		/* Icmp code */		
	struct {
		int una;	/* Unacked data pointer */
		int nxt;	/* Next sequence expected */
		int ptr;	/* Data pointer */
		ushort wnd;	/* Tcp send window */
		int up;		/* Urgent data pointer */
		int wl1;
		int wl2;
	} snd;
	int	iss;
	ushort	cwind;
	ushort	ssthresh;
	int	resent;
	struct {
		int nxt;
		ushort wnd;
		int up;
	} rcv;
	int	irs;
	ushort	mss;
	int	rerecv;
	ushort	window;
	int	max_snd;
	int	last_ack;
	char	backoff;
	char	flags;
	char	tos;

	Block	*rcvq;
	ushort	rcvcnt;

	Block	*sndq;			/* List of data going out */
	ushort	sndcnt;			/* Amount of data in send queue */

	Reseq	*reseq;			/* Resequencing queue */
	Timer	timer;			 
	Timer	acktimer;		/* Acknoledge timer */
	Timer	rtt_timer;		/* Round trip timer */
	int	rttseq;			/* Round trip sequence */
	int	srtt;			/* Shortened round trip */
	int	mdev;			/* Mean deviation of round trip */
};

struct Tcpctl
{
	QLock;
	struct Tctl;
	Rendez syner;
};

struct	Tcp
{
	Port	source;
	Port	dest;
	int	seq;
	int	ack;
	char	flags;
	ushort	wnd;
	ushort	up;
	ushort	mss;
};

struct Reseq
{
	Reseq 	*next;
	Tcp	seg;
	Block	*bp;
	ushort	length;
	char	tos;
};

/* An ip interface used for UDP/TCP/IL */
struct Ipconv
{
	QLock;				/* Ref count lock */
	int 	ref;
	int	index;
	Qinfo	*stproto;		/* Stream protocol for this device */
	Network	*net;			/* user level network interface */
	Ipaddr	dst;			/* Destination from connect */
	Port	psrc;			/* Source port */
	Port	pdst;			/* Destination port */

	Ipifc	*ipinterface;		/* Ip protocol interface */
	Queue	*readq;			/* Pointer to upstream read q */
	QLock	listenq;		/* List of people waiting incoming cons */
	Rendez	listenr;		/* Some where to sleep while waiting */
		
	char	*err;			/* Async protocol error */
	int	backlog;		/* Maximum number of waiting connections */
	int	curlog;			/* Number of waiting connections */
	int 	newcon;			/* Flags that this is the start of a connection */

	union {
		Tcpctl	tcpctl;			/* Tcp control block */
		Ilcb	ilctl;			/* Il control block */
	};
};

#define	MAX_TIME	100000000	/* Forever */
#define TCP_ACK		200		/* Timed ack sequence every 200ms */

#define URG	0x20
#define ACK	0x10
#define PSH	0x08
#define RST	0x04
#define SYN	0x02
#define FIN	0x01

#define EOL_KIND	0
#define NOOP_KIND	1
#define MSS_KIND	2

#define MSS_LENGTH	4
#define MSL2		10
#define MSPTICK		100
#define DEF_MSS		1024
#define DEF_RTT		1000

#define TCP_PASSIVE	0
#define TCP_ACTIVE	1
#define IL_PASSIVE	0
#define IL_ACTIVE	1

#define MAXBACKOFF	5
#define FORCE		1
#define	CLONE		2
#define RETRAN		4
#define ACTIVE		8
#define SYNACK		16
#define AGAIN		8
#define DGAIN		4

#define TIMER_STOP	0
#define TIMER_RUN	1
#define TIMER_EXPIRE	2

#define Nreseq		64

#define	set_timer(t,x)	(((t)->start) = (x)/MSPTICK)
#define	run_timer(t)	((t)->state == TIMER_RUN)

enum					/* Tcp connection states */
{
	Closed = 0,
	Listen,
	Syn_sent,
	Syn_received,
	Established,
	Finwait1,
	Finwait2,
	Close_wait,
	Closing,
	Last_ack,
	Time_wait
};

/*
 * Ip interface structure. We have one for each active protocol driver
 */
struct Ipifc 
{
	QLock;
	int 		ref;
	uchar		protocol;		/* Ip header protocol number */
	char		name[NAMELEN];		/* Protocol name */
	void (*iprcv)	(Ipconv *, Block *);	/* Receive demultiplexor */
	Ipconv		*connections;		/* Connection list */
	int		maxmtu;			/* Maximum transfer unit */
	int		minmtu;			/* Minumum tranfer unit */
	int		hsize;			/* Media header size */	
	ulong		chkerrs;		/* checksum errors */
	Lock;	
};

struct Fragq
{
	QLock;
	Block  *blist;
	Fragq  *next;
	Ipaddr src;
	Ipaddr dst;
	ushort id;
	ulong  age;
};

struct Ipfrag
{
	ushort	foff;
	ushort	flen;
};

#define IP_VER	0x40			/* Using IP version 4 */
#define IP_HLEN 0x05			/* Header length in characters */
#define IP_DF	0x4000			/* Don't fragment */
#define IP_MF	0x2000			/* More fragments */

#define	ICMP_ECHOREPLY		0	/* Echo Reply */
#define	ICMP_UNREACH		3	/* Destination Unreachable */
#define	ICMP_SOURCEQUENCH	4	/* Source Quench */
#define	ICMP_REDIRECT		5	/* Redirect */
#define	ICMP_ECHO		8	/* Echo Request */
#define	ICMP_TIMXCEED		11	/* Time-to-live Exceeded */
#define	ICMP_PARAMPROB		12	/* Parameter Problem */
#define	ICMP_TSTAMP		13	/* Timestamp */
#define	ICMP_TSTAMPREPLY	14	/* Timestamp Reply */
#define	ICMP_IREQ		15	/* Information Request */
#define	ICMP_IREQREPLY		16	/* Information Reply */

/* Sizes */
#define IP_MAX		(32*1024)		/* Maximum Internet packet size */
#define UDP_MAX		(IP_MAX-ETHER_IPHDR)	/* Maximum UDP datagram size */
#define UDP_DATMAX	(UDP_MAX-UDP_HDRSIZE)	/* Maximum amount of udp data */
#define IL_DATMAX	(IP_MAX-IL_HDRSIZE)	/* Maximum IL data in one ip packet */

/* Protocol numbers */
#define IP_UDPPROTO	17
#define IP_TCPPROTO	6
#define	IP_ILPROTO	190		/* I have no idea */

/* Protocol port numbers */
#define PORTALLOC	5000		/* First automatic allocated port */
#define PRIVPORTALLOC	600		/* First priveleged port allocated */
#define PORTMAX		30000		/* Last port to allocte */

/* Stuff to go in funs.h someday */
Ipifc   *newipifc(uchar, void (*)(Ipconv *, Block*), Ipconv *, int, int, int, char*);
void	closeipifc(Ipifc*);
ushort	ip_csum(uchar*);
int	arp_lookup(uchar*, uchar*);
Ipaddr	ipparse(char*);
void	hnputs(uchar*, ushort);
void	hnputl(uchar*, ulong);
ulong	nhgetl(uchar*);
ushort	nhgets(uchar*);
ushort	ptcl_csum(Block*bp, int, int);
void	ppkt(Block*);
void	udprcvmsg(Ipconv *, Block*);
Block	*btrim(Block*, int, int);
Block	*ip_reassemble(int, Block*, Etherhdr*);
Ipconv	*portused(Ipconv *, Port);
Port	nextport(Ipconv *, int);
Fragq   *ipfragallo(void);
void	ipfragfree(Fragq*, int);
void	iproute(uchar*, uchar*);
void	initfrag(int);
int	ntohtcp(Tcp*, Block**);
void	reset(Ipaddr, Ipaddr, char, ushort, Tcp*);
void	proc_syn(Ipconv*, char, Tcp*);
void	send_syn(Tcpctl*);
void	tcp_output(Ipconv*);
int	seq_within(int, int, int);
void	update(Ipconv *, Tcp *);
int	trim(Tcpctl *, Tcp *, Block **, ushort *);
void	add_reseq(Tcpctl *, char, Tcp *, Block *, ushort);
void	close_self(Ipconv *, char []);
int	seq_gt(int, int);
Ipconv	*ip_conn(Ipconv *, Port, Port, Ipaddr dest, char proto);
void	ipmkdir(Qinfo *, Dirtab *, Ipconv *);
Ipconv	*ipincoming(Ipconv*, Ipconv*);
int	inb_window(Tcpctl *, int);
Block	*htontcp(Tcp *, Block *, Tcphdr *);
void	start_timer(Timer *);
void	stop_timer(Timer *);
int	copyupb(Block **, uchar *, int);
void	init_tcpctl(Ipconv *);
int	iss(void);
int	seq_within(int, int, int);
int	seq_lt(int, int);
int	seq_le(int, int);
int	seq_gt(int, int);
int	seq_ge(int, int);
void	setstate(Ipconv *, char);
void	tcpackproc(void*);
Block 	*htontcp(Tcp *, Block *, Tcphdr *);
int	ntohtcp(Tcp *, Block **);
void	extract_oob(Block **, Block **, Tcp *);
void	get_reseq(Tcpctl *, char *, Tcp *, Block **, ushort *);
void	state_upcall(Ipconv*, char oldstate, char newstate);
int	backoff(int);
int	dupb(Block **, Block *, int, int);
void	tcp_input(Ipconv *, Block *);
void 	tcprcvwin(Ipconv *);
void	tcpstart(Ipconv *, int, ushort, char);
void	ilstart(Ipconv *, int, int);
void	tcpflow(void*);
void 	tcp_timeout(void *);
void	tcp_acktimer(void *);
int	ipclonecon(Chan *);
int	iplisten(Chan *);
void	iloutoforder(Ipconv*, Ilhdr*, Block*);
void	iplocalfill(Chan*, char*, int);
void	ipremotefill(Chan*, char*, int);
void	ipstatusfill(Chan*, char*, int);
int	ipforme(uchar*);
void	ipsetaddrs(void);
int	ipconbusy(Ipconv*);

#define	fmtaddr(xx)	(xx>>24)&0xff,(xx>>16)&0xff,(xx>>8)&0xff,xx&0xff
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define BLKIP(xp)	((Etherhdr *)((xp)->rptr))
#define BLKFRAG(xp)	((Ipfrag *)((xp)->base))
#define PREC(x)		((x)>>5 & 7)

#define WORKBUF		64

extern Ipaddr Myip[7];
extern Ipaddr Mymask;
extern Ipaddr Mynetmask;
extern Ipaddr classmask[4];
extern Ipconv *ipconv[];
extern char *tcpstate[];
extern char *ilstate[];
extern Rendez tcpflowr;
extern Qinfo tcpinfo;
extern Qinfo ipinfo;
extern Qinfo udpinfo;
extern Qinfo ilinfo;
extern Qinfo arpinfo;
extern Queue *Ipoutput;

/* offsets into Myip */
enum
{
	Myself=		0,
	Mybcast=	1,
	Mynet=		3,
	Mysubnet=	5,
};
