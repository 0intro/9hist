/*
	Lucent Wavelan IEEE 802.11 pcmcia.
	There is almost no documentation for the card.
	the driver is done using both the freebsd and the linux
	driver as `documentation'.

	Has been used with the card plugged in during all up time.
	no cards removals/insertions yet.

	For known BUGS see the comments below. Besides,
	the driver keeps interrupts disabled for just too
	long. When it gets robust, locks should be revisited.

	BUGS: Endian, alignment and mem/io issues?
 */

#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/netif.h"
#include "etherif.h"


#define DEBUG	if(1)print

#define SEEKEYS 1

typedef struct Ctlr 	Ctlr;
typedef struct Wltv 	Wltv;
typedef struct WFrame	WFrame;
typedef struct Stats	Stats;
typedef struct WStats	WStats;
typedef struct WKey	WKey;

struct WStats 
{
	ulong	ntxuframes;		// unicast frames
	ulong	ntxmframes;		// multicast frames
	ulong	ntxfrags;		// fragments
	ulong	ntxubytes;		// unicast bytes
	ulong	ntxmbytes;		// multicast bytes
	ulong	ntxdeferred;		// deferred transmits
	ulong	ntxsretries;		// single retries
	ulong	ntxmultiretries;	// multiple retries
	ulong	ntxretrylimit;
	ulong	ntxdiscards;
	ulong	nrxuframes;		// unicast frames
	ulong	nrxmframes;		// multicast frames
	ulong	nrxfrags;		// fragments
	ulong	nrxubytes;		// unicast bytes
	ulong	nrxmbytes;		// multicast bytes
	ulong	nrxfcserr;
	ulong	nrxdropnobuf;
	ulong	nrxdropnosa;
	ulong	nrxcantdecrypt;
	ulong	nrxmsgfrag;
	ulong	nrxmsgbadfrag;
	ulong	end;
};

struct WFrame
{
	ushort	sts;
	ushort	rsvd0;
	ushort	rsvd1;
	ushort	qinfo;
	ushort	rsvd2;
	ushort	rsvd3;
	ushort	txctl;
	ushort	framectl;
	ushort	id;
	uchar	addr1[Eaddrlen];
	uchar	addr2[Eaddrlen];
	uchar	addr3[Eaddrlen];
	ushort	seqctl;
	uchar	addr4[Eaddrlen];
	ushort	dlen;
	uchar	dstaddr[Eaddrlen];
	uchar	srcaddr[Eaddrlen];
	ushort	len;
	ushort	dat[3];
	ushort	type;
};

// Lucent's Length-Type-Value records  to talk to the wavelan.
// most operational parameters are read/set using this. 
enum 
{
	WTyp_Stats	= 0xf100,
	WTyp_Ptype	= 0xfc00,
	WTyp_Mac	= 0xfc01,
	WTyp_WantName	= 0xfc02,
	WTyp_Chan	= 0xfc03,
	WTyp_NetName	= 0xfc04,
	WTyp_ApDens	= 0xfc06,
	WTyp_MaxLen	= 0xfc07,
	WTyp_PM		= 0xfc09,
	WTyp_PMWait	= 0xfc0c,
	WTyp_NodeName	= 0xfc0e,
	WTyp_Crypt	= 0xfc20,
	WTyp_XClear	= 0xfc22,
	WTyp_Tick 	= 0xfce0,
	WTyp_RtsThres	= 0xfc83,
	WTyp_TxRate	= 0xfc84,
		WTx1Mbps	= 0x0,
		WTx2Mbps	= 0x1,
		WTxAuto		= 0x3,
	WTyp_Prom	= 0xfc85,
	WTyp_Keys	= 0xfcb0,
	WTyp_TxKey	= 0xfcb1,
	WTyp_CurName	= 0xfd41,
	WTyp_HasCrypt	= 0xfd4f,
};

// Controller
enum
{
	WDfltIRQ	 = 3,		// default irq
	WDfltIOB	 = 0x100,	// default IO base

	WIOLen		 = 0x40,	// Hermes IO length

	WTmOut		 = 65536,	// Cmd time out

	WPTypeManaged	= 1,
	WPTypeWDS	= 2,
	WPTypeAdHoc	= 3,
	WDfltPType	= WPTypeManaged,

	WDfltApDens	= 1,
	WDfltRtsThres	= 2347,		// == disabled
	WDfltTxRate	= WTxAuto,	// 2Mbps

	WMaxLen		= 2304,
	WNameLen	= 32,

	WNKeys		= 4,
	WKeyLen		= 14,
	WMinKeyLen	= 5,

	// Wavelan hermes registers
	WR_Cmd		= 0x00,
		WCmdIni		= 0x0000,
		WCmdEna		= 0x0001,
		WCmdDis		= 0x0002,
		WCmdMalloc	= 0x000a,
		WCmdAskStats	= 0x0011,
		WCmdMsk		= 0x003f,
		WCmdAccRd	= 0x0021,
		WCmdAccWr	= 0x0121,
		WCmdTxFree	= 0x000b|0x0100,
	WR_Parm0	= 0x02,
	WR_Parm1	= 0x04,
	WR_Parm2	= 0x06,
	WR_Sts		= 0x08,
	WR_InfoId	= 0x10,
	WR_Sel0		= 0x18,
	WR_Sel1		= 0x1a,
	WR_Off0		= 0x1c,
	WR_Off1		= 0x1e,
		WBusyOff	= 0x8000,
		WErrOff		= 0x4000,
		WResSts		= 0x7f00,
	WR_RXId		= 0x20,
	WR_Alloc	= 0x22,
	WR_EvSts	= 0x30,
	WR_IntEna	= 0x32,
		WCmdEv		= 0x0010,
		WRXEv		= 0x0001,
		WTXEv		= 0x0002,
		WTxErrEv	= 0x0004,
		WAllocEv	= 0x0008,
		WInfoEv		= 0x0080,
		WIDropEv	= 0x2000,
		WTickEv		= 0x8000,
		WEvs		= WRXEv|WTXEv|WAllocEv|WInfoEv|WIDropEv,

	WR_EvAck	= 0x34,
	WR_Data0	= 0x36,
	WR_Data1	= 0x38,

	// Frame stuff

	WF_Err		= 0x0003,
	WF_1042		= 0x2000,
	WF_Tunnel	= 0x4000,
	WF_WMP		= 0x6000,

	WF_Data		= 0x0008,

	WSnapK1		= 0xaa,
	WSnapK2		= 0x00,
	WSnapCtlr	= 0x03,
	WSnap0		= (WSnapK1|(WSnapK1<<8)),
	WSnap1		= (WSnapK2|(WSnapCtlr<<8)),
	WSnapHdrLen	= 6,

	WF_802_11_Off	= 0x44,
	WF_802_3_Off 	= 0x2e,

};

#define csr_outs(ctlr,r,arg) 	outs((ctlr)->iob+(r),(arg))
#define csr_ins(ctlr,r)		ins((ctlr)->iob+(r))
#define csr_ack(ctlr,ev)	outs((ctlr)->iob+WR_EvAck,(ev))

struct WKey
{
	ushort	len;
	char	dat[WKeyLen];
};

struct Wltv
{
	ushort	len;
	ushort	type;
	union 
	{
		struct {
			ushort	val;
			ushort	pad;
		};
		struct {
			uchar	addr[8];
		};
		struct {
			ushort	slen;
			char	s[WNameLen];
		};
		struct {
			char	name[WNameLen];
		};
		struct {
			WKey	key[WNKeys];
		};
	};
};

// What the driver thinks. Not what the card thinks.
struct Stats
{
	ulong	nints;
	ulong	nrx;
	ulong	ntx;
	ulong	ntxrq;
	ulong	nrxerr;
	ulong	ntxerr;
	ulong	nalloc;			// allocation (reclaim) events
	ulong	ninfo;
	ulong	nidrop;
	ulong	nwatchdogs;		// transmit time outs, actually
};

struct Ctlr 
{
	Lock;
	Rendez	timer;

	int	attached;
	int	slot;
	int	iob;
	int	ptype;
	int	apdensity;
	int	rtsthres;
	int	txbusy;
	int	txrate;
	int	txdid;
	int	txmid;
	int	txtmout;
	int	maxlen;
	int	chan;
	int	pmena;
	int	pmwait;

	char	netname[WNameLen];
	char	wantname[WNameLen];
	char	nodename[WNameLen];
	WFrame	txf;
	uchar	txbuf[1536];
	int	txlen;

	int	keyid;			// 0 if not using WEP
	int	xclear;
	WKey	key[WNKeys];

	Stats;
	WStats;
};

// w_... routines do not ilock the Ctlr and should 
// be called locked.

static void 
w_intdis(Ctlr* ctlr)
{
	csr_outs(ctlr, WR_IntEna, 0); 
	csr_ack(ctlr, 0xffff);
}

static void
w_intena(Ctlr* ctlr)
{
	csr_outs(ctlr, WR_IntEna, WEvs); 
}

static int
w_cmd(Ctlr *ctlr, ushort cmd, ushort arg)
{
	int i;
	int rc;

	csr_outs(ctlr, WR_Parm0, arg);
	csr_outs(ctlr, WR_Cmd, cmd);
	for (i = 0; i<WTmOut; i++){
		rc = csr_ins(ctlr, WR_EvSts);
		if ( rc&WCmdEv ){
			rc = csr_ins(ctlr, WR_Sts);
			csr_ack(ctlr, WCmdEv);
			if ((rc&WCmdMsk) != (cmd&WCmdMsk))
				break;
			if (rc&WResSts)
				break;
			return 0;
		}
	}

	return -1;
}

static int
w_seek(Ctlr* ctlr, ushort id, ushort offset, int chan)
{
	int i, rc;
	static ushort sel[] = { WR_Sel0, WR_Sel1 };
	static ushort off[] = { WR_Off0, WR_Off1 };

	if (chan != 0 && chan != 1)
		panic("wavelan: bad chan\n");
	csr_outs(ctlr, sel[chan], id);
	csr_outs(ctlr, off[chan], offset);
	for (i=0; i<WTmOut; i++){
		rc = csr_ins(ctlr, off[chan]);
		if ((rc & (WBusyOff|WErrOff)) == 0)
			return 0;
	}
	return -1;
}

static int
w_inltv(Ctlr* ctlr, Wltv* l)
{
	int i, len;
	ushort *p,code;

	if (w_cmd(ctlr, WCmdAccRd, l->type)){
		DEBUG("wavelan: access read failed\n");
		return -1;
	}
	if (w_seek(ctlr,l->type,0,1)){
		DEBUG("wavelan: seek failed\n");
		return -1;
	}
	len = csr_ins(ctlr, WR_Data1);
	if (len > l->len)
		return -1;
	l->len = len;
	if ((code=csr_ins(ctlr, WR_Data1)) != l->type){
		DEBUG("wavelan: type %x != code %x\n",l->type,code);
		return -1;
	}
	p = &l->val;
	len--;
	for (i=0; i<len; i++)
		p[i] = csr_ins(ctlr, WR_Data1);
	return 0;
}

static void
w_outltv(Ctlr* ctlr, Wltv* l)
{
	int i,len;
	ushort *p;

	if (w_seek(ctlr,l->type,0,1))
		return;
	csr_outs(ctlr, WR_Data1, l->len);
	csr_outs(ctlr, WR_Data1, l->type);
	p = &l->val;
	len = l->len-1;
	for (i=0; i<len; i++)
		csr_outs(ctlr, WR_Data1, p[i]);
	w_cmd(ctlr,WCmdAccWr,l->type);
}

static void
ltv_outs(Ctlr* ctlr, int type, ushort val)
{
	Wltv l;

	l.type = type;
	l.val = val;
	l.len = 2;
	w_outltv(ctlr, &l);
}

static ushort
ltv_ins(Ctlr* ctlr, int type)
{
	Wltv l;

	l.type = type;
	l.len = 2;
	l.val = 0;
	w_inltv(ctlr, &l);
	return l.val;
}

static void
ltv_outstr(Ctlr* ctlr, int type, char *val)
{
	Wltv l;
	int len;

	len = strlen(val);
	if(len > sizeof(l.s))
		len = sizeof(l.s);
	memset(&l, 0, sizeof(l));
	l.type = type;
	l.len = (sizeof(l.type)+sizeof(l.slen)+sizeof(l.s))/2;
	l.slen = (len+1) & ~1;
	strncpy(l.s, val, len);
	w_outltv(ctlr, &l);
}

static char Unkname[] = "who knows";
static char Nilname[] = "card does not tell";

static char*
ltv_inname(Ctlr* ctlr, int type)
{
	static Wltv l;
	
	memset(&l,0,sizeof(l));
	l.type = type;
	l.len  = WNameLen/2+2;
	if (w_inltv(ctlr, &l))
		return Unkname;
	if (l.name[2] == 0)
		return Nilname;
	return l.name+2;
}


static char*
ltv_inkey(Ctlr* ctlr, int id)
{
	static char k[WKeyLen+1];
	Wltv l;
	int i, len;

	id--; 
	memset(&l, 0, sizeof(l));
	l.type = WTyp_Keys;
	l.len = sizeof(l.key)/2 + 2;	// BUG? 
	if (w_inltv(ctlr, &l))
		return Unkname;
	if (id <0 || id >=WNKeys){
		DEBUG("wavelan: ltv_inkey: bad key id\n");
		return Unkname;
	}
	for (i=0 ; i <WNKeys; i++){
		// BUG: despite the checks, this seems to get nothing;
		// although it could be that the card
		// does not allow reading keys for security.
		if (l.key[id].len != 0){
			DEBUG("len=%d ", l.key[id].len);
		}
		if (l.key[id].dat[1] != 0){
			DEBUG("dat1=%s ", l.key[id].dat+1);
		} 
		if (l.key[id].dat[2] != 0){
			DEBUG("dat2=%s ", l.key[id].dat+2);
		} 
		if (l.key[id].dat[0] != 0){
			DEBUG("%s ", l.key[id].dat);
		} 
//		else 
//			DEBUG("none ");
	}
	DEBUG("\n");
	if (l.key[id].dat[0] == 0)
		return Nilname;
	len = l.key[id].len;
	if (len > WKeyLen)
		len = WKeyLen;
	memset(k, 0, sizeof(k));
	memmove(k, l.key[id].dat, len);
	k[WKeyLen] = 0;
	return k;
}

static int
w_read(Ctlr* ctlr, int type, int off, void* buf, ulong len)
{
	ushort *p = (ushort*)buf;
	int i, n;

	n=0;
	if (w_seek(ctlr, type, off, 1) == 0){
		len /= 2;
		for (i = 0; i < len; i++){
			p[i] = csr_ins(ctlr, WR_Data1);
			n += 2;
		}
	} else
		DEBUG("wavelan: w_read: seek failed");

	return n;
}


static int
w_write(Ctlr* ctlr, int type, int off, void* buf, ulong len)
{
	ushort *p = (ushort*)buf;
	ulong l = len / 2;
	int i,tries;

	for (tries=0; tries < WTmOut; tries++){	
		if (w_seek(ctlr, type, off, 0)){
			DEBUG("wavelan: w_write: seek failed\n");
			return 0;
		}

		for (i = 0; i < l; i++)
			csr_outs(ctlr, WR_Data0, p[i]);
		csr_outs(ctlr, WR_Data0, 0xdead);
		csr_outs(ctlr, WR_Data0, 0xbeef);
		if (w_seek(ctlr, type, off + len, 0)){
			DEBUG("wavelan: write seek failed\n");
			return 0;
		}
		if (csr_ins(ctlr, WR_Data0) == 0xdead)
		if (csr_ins(ctlr, WR_Data0) == 0xbeef)
			return len;
		DEBUG("wavelan: Hermes bug byte.\n");
		return 0;
	}
	DEBUG("wavelan: tx timeout\n");
	return 0;
}

static int
w_alloc(Ctlr* ctlr, int len)
{
	int rc;
	int i,j;

	if (w_cmd(ctlr, WCmdMalloc, len)==0)
		for (i = 0; i<WTmOut; i++)
			if (csr_ins(ctlr, WR_EvSts) & WAllocEv){
				csr_ack(ctlr, WAllocEv);
				rc=csr_ins(ctlr, WR_Alloc);
				if (w_seek(ctlr, rc, 0, 0))
					return -1;
				len = len/2;
				for (j=0; j<len; j++)
					csr_outs(ctlr, WR_Data0, 0);
				return rc;
			}
	return -1;
}


static int 
w_enable(Ether* ether)
{
	Wltv l;
	Ctlr* ctlr = (Ctlr*) ether->ctlr;
	ushort wep;

	if (!ctlr)
		return -1;

	w_intdis(ctlr);
	w_cmd(ctlr, WCmdDis, 0);
	w_intdis(ctlr);
	if(w_cmd(ctlr, WCmdIni, 0))
		return -1;

	w_intdis(ctlr);
	ltv_outs(ctlr, WTyp_Tick, 8);
	ltv_outs(ctlr, WTyp_MaxLen, ctlr->maxlen);
	ltv_outs(ctlr, WTyp_Ptype, ctlr->ptype);
	ltv_outs(ctlr, WTyp_RtsThres, ctlr->rtsthres);
	ltv_outs(ctlr, WTyp_TxRate, ctlr->txrate);
	ltv_outs(ctlr, WTyp_ApDens, ctlr->apdensity);
	ltv_outs(ctlr, WTyp_PMWait, ctlr->pmwait);
	ltv_outs(ctlr, WTyp_PM, ctlr->pmena);
	if (*ctlr->netname)
		ltv_outstr(ctlr, WTyp_NetName, ctlr->netname);
	if (*ctlr->wantname)
		ltv_outstr(ctlr, WTyp_WantName, ctlr->wantname);
	ltv_outs(ctlr, WTyp_Chan, ctlr->chan);
	if (*ctlr->nodename)
		ltv_outstr(ctlr, WTyp_NodeName, ctlr->nodename);
	l.type = WTyp_Mac;
	l.len = 4;
	memmove(l.addr, ether->ea, Eaddrlen);
	w_outltv(ctlr, &l);

	ltv_outs(ctlr, WTyp_Prom, (ether->prom?1:0));

	wep = ltv_ins(ctlr, WTyp_HasCrypt);
	if (wep)
		if (ctlr->keyid == 0)
			ltv_outs(ctlr, WTyp_Crypt, 0);
		else {
			// BUG?
			// I think it's ok, but don't know if
			// the card needs a WEPing access point
			// just to admit the keys.
			ltv_outs(ctlr, WTyp_TxKey, ctlr->keyid);
			memset(&l, 0, sizeof(l));
			l.len = sizeof(ctlr->key[0])*WNKeys/2 + 1;
			l.type= WTyp_Keys;
			memmove(l.key, &ctlr->key[0], sizeof(l.key[0])*WNKeys);
			w_outltv(ctlr, &l);
			ltv_outs(ctlr, WTyp_Crypt, wep);
			ltv_outs(ctlr, WTyp_XClear, ctlr->xclear);
		}

	// BUG: set multicast addresses

	if (w_cmd(ctlr, WCmdEna, 0)){
		DEBUG("wavelan: Enable failed");
		return -1;
	}
	ctlr->txdid = w_alloc(ctlr, 1518 + sizeof(WFrame) + 8);
	ctlr->txmid = w_alloc(ctlr, 1518 + sizeof(WFrame) + 8);
	if (ctlr->txdid == -1 || ctlr->txmid == -1)
		DEBUG("wavelan: alloc failed");
	ctlr->txbusy= 0;
	w_intena(ctlr);
	return 0;

}


static void
w_rxdone(Ether* ether)
{
	Ctlr* ctlr = (Ctlr*) ether->ctlr;
	ushort sp;
	WFrame f;
	Block* bp=0;
	ulong l;
	Etherpkt* ep;

	sp = csr_ins(ctlr, WR_RXId);
	l = w_read(ctlr, sp, 0, &f, sizeof(f));
	if (l == 0){
		DEBUG("wavelan: read frame error\n");
		goto rxerror;
	}
	if (f.sts&WF_Err){
		goto rxerror;
	}
	switch(f.sts){
	case WF_1042:
	case WF_Tunnel:
	case WF_WMP:
		l = f.dlen + WSnapHdrLen;
		bp = iallocb(ETHERHDRSIZE + l + 2);
		if (!bp)
			goto rxerror;
		ep = (Etherpkt*) bp->wp;
		memmove(ep->d, f.addr1, Eaddrlen);
		memmove(ep->s, f.addr2, Eaddrlen);
		memmove(ep->type,&f.type,2);
		bp->wp += ETHERHDRSIZE;
		if (w_read(ctlr, sp, WF_802_11_Off, bp->wp, l+2) == 0){
			DEBUG("wavelan: read 802.11 error\n");
			goto rxerror;
		}
		bp->wp +=  l+2;
		bp = trimblock(bp, 0, f.dlen+ETHERHDRSIZE);
		break;
	default:
		l = ETHERHDRSIZE + f.dlen + 2;
		bp = iallocb(l);
		if (!bp)
			goto rxerror;
		if (w_read(ctlr, sp, WF_802_3_Off, bp->wp, l) == 0){
			DEBUG("wavelan: read 800.3 error\n");
			goto rxerror;
		}
		bp->wp += l;
	}

	ctlr->nrx++;
	etheriq(ether,bp,1);
	return;

  rxerror:
	freeb(bp);
	ctlr->nrxerr++;
}

static int
w_txstart(Ether* ether, int again)
{

	Etherpkt* ep;
	Ctlr* ctlr = (Ctlr*) ether->ctlr;
	Block* bp; 
	int		txid;
	if (ctlr == 0 || ctlr->attached == 0 )
		return -1;
	if (ctlr->txbusy && again==0)
		return -1;

	txid = ctlr->txdid;
	if (again){		
		bp = 0;		// a watchdog reenabled the card. 
		goto retry;	// must retry a previously failed tx. 
	}

	bp = qget(ether->oq);
	if (bp == 0)
		return 0;
	ep = (Etherpkt*) bp->rp;
	ctlr->txbusy = 1;
	
	// BUG: only  IP/ARP/RARP seem to be ok for 802.3
	// Other packets should be just copied to the board.
	// The driver is not doing so, though. 
	// Besides, the Block should be used instead of txbuf,
	// to save a memory copy.
	memset(ctlr->txbuf,0,sizeof(ctlr->txbuf));
	memset(&ctlr->txf,0,sizeof(ctlr->txf));
	ctlr->txf.framectl = WF_Data;
	memmove(ctlr->txf.addr1, ep->d, Eaddrlen);
	memmove(ctlr->txf.addr2, ep->s, Eaddrlen);
	memmove(ctlr->txf.dstaddr, ep->d, Eaddrlen);
	memmove(ctlr->txf.srcaddr, ep->s, Eaddrlen);
	memmove(&ctlr->txf.type,ep->type,2);
	ctlr->txlen = BLEN(bp);
	ctlr->txf.dlen = ctlr->txlen - WSnapHdrLen;
	hnputs((uchar*)&ctlr->txf.dat[0], WSnap0);
	hnputs((uchar*)&ctlr->txf.dat[1], WSnap1);
	hnputs((uchar*)&ctlr->txf.len, ctlr->txlen - WSnapHdrLen);
	if (ctlr->txlen - ETHERHDRSIZE > 1536){
		print("wavelan: txbuf overflow");
		freeb(bp);
		return -1;
	}
	memmove(ctlr->txbuf, bp->rp+sizeof(ETHERHDRSIZE)+10, 
			ctlr->txlen - ETHERHDRSIZE  );
retry:
	w_write(ctlr, txid, 0, &ctlr->txf, sizeof(ctlr->txf)); 

	w_write(ctlr, txid, WF_802_11_Off, ctlr->txbuf,
			ctlr->txlen - ETHERHDRSIZE + 2);
	if (w_cmd(ctlr, WCmdTxFree, txid)){
		DEBUG("wavelan: transmit failed\n");
		ctlr->txbusy=0;	// added 
		ctlr->ntxerr++;
		freeb(bp);
		return -1;
	}
	ctlr->txtmout = 2;
	freeb(bp);
	return 0;
}

static void
w_txdone(Ctlr* ctlr, int sts)
{
	int b = ctlr->txbusy;

	ctlr->txbusy = 0;
	ctlr->txtmout= 0;
	if (sts & WTxErrEv){
		ctlr->ntxerr++;
		if (sts&1) // it was a watchdog, stay busy to retry.
			ctlr->txbusy = b;
	} else
		ctlr->ntx++;
}

static int
w_stats(Ctlr* ctlr)
{
	int sp,i;
	ushort rc;
	Wltv l;
	ulong* p = (ulong*)&ctlr->WStats;
	ulong* pend= (ulong*)&ctlr->end;

	sp = csr_ins(ctlr, WR_InfoId);
	l.type = l.len = 0;
	w_read(ctlr, sp, 0, &l, 4);
	if (l.type == WTyp_Stats){
		l.len--;
		for (i = 0; i < l.len && p < pend ; i++){
			rc = csr_ins(ctlr, WR_Data1);
			if (rc > 0xf000)
				rc = ~rc & 0xffff;
			p[i] += rc;
		}
		return 0;
	}
	return -1;
}

static void
w_intr(Ether *ether)
{
	int  rc, txid, i;
	Ctlr* ctlr = (Ctlr*) ether->ctlr;

	if (ctlr->attached == 0){
		csr_ack(ctlr, 0xffff);
		csr_outs(ctlr, WR_IntEna, 0); 
		return;
	}
	for(i=0; i<7; i++){
		csr_outs(ctlr, WR_IntEna, 0);
		rc = csr_ins(ctlr, WR_EvSts);
		csr_ack(ctlr, ~WEvs);	// Not interested on them
	
		if (rc & WRXEv){
			w_rxdone(ether);
			csr_ack(ctlr, WRXEv);
		}
		if (rc & WTXEv){
			w_txdone(ctlr, rc);
			csr_ack(ctlr, WTXEv);
		}
		if (rc & WAllocEv){
			ctlr->nalloc++;
			txid = csr_ins(ctlr, WR_Alloc);
			csr_ack(ctlr, WAllocEv);
			if (txid == ctlr->txdid){
				if ((rc & WTXEv) == 0)
					w_txdone(ctlr, rc);
			}
		}
		if (rc & WInfoEv){
			ctlr->ninfo++;
			w_stats(ctlr);
			csr_ack(ctlr, WInfoEv);
		}
		if (rc & WTxErrEv){
			w_txdone(ctlr, rc);
			csr_ack(ctlr, WTxErrEv);
		}
		if (rc & WIDropEv){
			ctlr->nidrop++;
			csr_ack(ctlr, WIDropEv);
		}
	
		w_intena(ctlr);
		w_txstart(ether,0);
	}
 }

// Watcher to ensure that the card still works properly and 
// to request WStats updates once a minute.
// BUG: it runs much more often, see the comment below.

static void
w_timer(void* arg)
{
	Ether* ether = (Ether*) arg;
	Ctlr* ctlr = (Ctlr*)ether->ctlr;
	int tick=0;

	for(;;){
		tsleep(&ctlr->timer, return0, 0, 50);
		ctlr = (Ctlr*)ether->ctlr;
		if (ctlr == 0)
			break;
		if (ctlr->attached == 0)
			continue;
		tick++;

		ilock(&ctlr->Lock);

		// Seems that the card gets frames BUT does
		// not send the interrupt; this is a problem because
		// I suspect it runs out of receive buffers and
		// stops receiving until a transmit watchdog
		// reenables the card.
		// The problem is serious because it leads to
		// poor rtts.
		// This can be seen clearly by commenting out
		// the next if and doing a ping: it will stop
		// receiving (although the icmp replies are being
		// issued from the remote) after a few seconds. 
		// Of course this `bug' could be because I'm reading
		// the card frames in the wrong way; due to the
		// lack of documentation I cannot know.

		if (csr_ins(ctlr, WR_EvSts)&WEvs)
				w_intr(ether);

		if (tick % 10 == 0) {
			if (ctlr->txtmout && --ctlr->txtmout == 0){
				ctlr->nwatchdogs++;
				w_txdone(ctlr, WTxErrEv|1); // 1: keep it busy
				if (w_enable(ether)){
					DEBUG("wavelan: wdog enable failed\n");
				}
				if (ctlr->txbusy) 
					w_txstart(ether,1);
			}
			if (tick % 120 == 0)
			if (ctlr->txbusy == 0)
				w_cmd(ctlr, WCmdAskStats, WTyp_Stats);
		}
		iunlock(&ctlr->Lock);
	} 
	pexit("terminated",0);
}

static void*
emalloc(ulong size)
{
	void *r=malloc(size);
	if (!r)
		error(Enomem);
	memset(r,0,size);
	return r;
}



static void
multicast(void*, uchar*, int)
{
	// BUG: to be added. 
}

static void
attach(Ether* ether)
{
	Ctlr* ctlr;
	char name[NAMELEN];
	int rc;

	if (ether->ctlr == 0)
		return;

	snprint(name, NAMELEN, "#l%dtimer", ether->ctlrno);
	ctlr = (Ctlr*) ether->ctlr;
	if (ctlr->attached == 0){
		ilock(&ctlr->Lock);
		rc = w_enable(ether);
		iunlock(&ctlr->Lock);
		if(rc == 0){
			ctlr->attached = 1;
			kproc(name, w_timer, ether);
		} else
			print("#l%d: enable failed\n",ether->ctlrno);
	} 
}

#define PRINTSTAT(fmt,val)	l += snprint(p+l, READSTR-l, (fmt), (val))
#define PRINTSTR(fmt)		l += snprint(p+l, READSTR-l, (fmt))

static long
ifstat(Ether* ether, void* a, long n, ulong offset)
{
	Ctlr *ctlr = (Ctlr*) ether->ctlr;
	char *k, *p;
	int l, i, txid;

	if (n == 0 || ctlr == 0){
		return 0;
	}
	p = malloc(READSTR);
	l = 0;
	PRINTSTAT("Interrupts: %lud\n", ctlr->nints);
	PRINTSTAT("TxPackets: %lud\n", ctlr->ntx);
	PRINTSTAT("RxPackets: %lud\n", ctlr->nrx);
	PRINTSTAT("TxErrors: %lud\n", ctlr->ntxerr);
	PRINTSTAT("RxErrors: %lud\n", ctlr->nrxerr);
	PRINTSTAT("TxRequests: %lud\n", ctlr->ntxrq);
	PRINTSTAT("AllocEvs: %lud\n", ctlr->nalloc);
	PRINTSTAT("InfoEvs: %lud\n", ctlr->ninfo);
	PRINTSTAT("InfoDrop: %lud\n", ctlr->nidrop);
	PRINTSTAT("WatchDogs: %lud\n", ctlr->nwatchdogs);
	k = ((ctlr->attached) ? "attached" : "not attached");
	PRINTSTAT("Card %s", k);
	k = ((ctlr->txbusy)? ", txbusy" : "");
	PRINTSTAT("%s\n", k);

	if (ctlr->keyid){
		PRINTSTR("Keys: ");
		for (i = 0; i < WNKeys; i++)
			if (ctlr->key[i].len == 0)
				PRINTSTR("none ");
			else {
				if (SEEKEYS == 0)
					PRINTSTR("set ");
				else {
					PRINTSTAT("%s ", ctlr->key[i].dat);
				}
			}
		PRINTSTR("\n");
	}

	// real card stats
	ilock(&ctlr->Lock);
	PRINTSTR("\nCard stats: \n");
	PRINTSTAT("Status: %ux\n", csr_ins(ctlr, WR_Sts));
	PRINTSTAT("Event status: %ux\n", csr_ins(ctlr, WR_EvSts));
	i = ltv_ins(ctlr, WTyp_Ptype);
	PRINTSTAT("Port type: %d\n", i);
	PRINTSTAT("Transmit rate: %d\n", ltv_ins(ctlr, WTyp_TxRate));
	PRINTSTAT("Channel: %d\n", ltv_ins(ctlr, WTyp_Chan));
	PRINTSTAT("AP density: %d\n", ltv_ins(ctlr, WTyp_ApDens));
	PRINTSTAT("Promiscuous mode: %d\n", ltv_ins(ctlr, WTyp_Prom));
	if(i == 3)
		PRINTSTAT("SSID name: %s\n", ltv_inname(ctlr, WTyp_NetName));
	else
		PRINTSTAT("Current name: %s\n", ltv_inname(ctlr, WTyp_CurName));
	PRINTSTAT("Net name: %s\n", ltv_inname(ctlr, WTyp_WantName));
	PRINTSTAT("Node name: %s\n", ltv_inname(ctlr, WTyp_NodeName));
	if (ltv_ins(ctlr, WTyp_HasCrypt) == 0)
		PRINTSTR("WEP: not supported\n");
	else {
		if (ltv_ins(ctlr, WTyp_Crypt) == 0)
			PRINTSTR("WEP: disabled\n");
		else{
			PRINTSTR("WEP: enabled\n");
			i = ltv_ins(ctlr, WTyp_XClear);
			k = ((i) ? "excluded" : "included");
			PRINTSTAT("Clear packets: %s\n", k);
			txid = ltv_ins(ctlr, WTyp_TxKey);
			PRINTSTAT("Transmit key id: %d\n", txid);
			if (txid >= 1 && txid <= WNKeys){
				k = ltv_inkey(ctlr, txid);
				if (SEEKEYS && k != nil)
					PRINTSTAT("Transmit key: %s\n", k);
			}
		}
	}
	iunlock(&ctlr->Lock);

	PRINTSTAT("ntxuframes: %lud\n", ctlr->ntxuframes);
	PRINTSTAT("ntxmframes: %lud\n", ctlr->ntxmframes);
	PRINTSTAT("ntxfrags: %lud\n", ctlr->ntxfrags);
	PRINTSTAT("ntxubytes: %lud\n", ctlr->ntxubytes);
	PRINTSTAT("ntxmbytes: %lud\n", ctlr->ntxmbytes);
	PRINTSTAT("ntxdeferred: %lud\n", ctlr->ntxdeferred);
	PRINTSTAT("ntxsretries: %lud\n", ctlr->ntxsretries);
	PRINTSTAT("ntxmultiretries: %lud\n", ctlr->ntxmultiretries);
	PRINTSTAT("ntxretrylimit: %lud\n", ctlr->ntxretrylimit);
	PRINTSTAT("ntxdiscards: %lud\n", ctlr->ntxdiscards);
	PRINTSTAT("nrxuframes: %lud\n", ctlr->nrxuframes);
	PRINTSTAT("nrxmframes: %lud\n", ctlr->nrxmframes);
	PRINTSTAT("nrxfrags: %lud\n", ctlr->nrxfrags);
	PRINTSTAT("nrxubytes: %lud\n", ctlr->nrxubytes);
	PRINTSTAT("nrxmbytes: %lud\n", ctlr->nrxmbytes);
	PRINTSTAT("nrxfcserr: %lud\n", ctlr->nrxfcserr);
	PRINTSTAT("nrxdropnobuf: %lud\n", ctlr->nrxdropnobuf);
	PRINTSTAT("nrxdropnosa: %lud\n", ctlr->nrxdropnosa);
	PRINTSTAT("nrxcantdecrypt: %lud\n", ctlr->nrxcantdecrypt);
	PRINTSTAT("nrxmsgfrag: %lud\n", ctlr->nrxmsgfrag);
	PRINTSTAT("nrxmsgbadfrag: %lud\n", ctlr->nrxmsgbadfrag);
	USED(l);
	n = readstr(offset, a, n, p);
	free(p);
	return n;
}
#undef PRINTSTR
#undef PRINTSTAT

		
static long 
ctl(Ether* ether, void* buf, long n)
{
	int i, key;
	Ctlr *ctlr;
	Cmdbuf *cb;
	char *nessid;
	WKey *kp;
	char k[64];
	char *kn= &k[3];

	if((ctlr = ether->ctlr) == nil)
		error(Enonexist);
	if(ctlr->attached == 0)
		error(Eshutdown);

	cb = parsecmd(buf, n);

	if(cb->nf < 2)
		error(Ebadctl);

	ilock(&ctlr->Lock);
	if(waserror()){
		iunlock(&ctlr->Lock);
		free(cb);
		nexterror();
	}

	if(strcmp(cb->f[0], "essid") == 0){
		if (strcmp(cb->f[1],"default") == 0)
			nessid = "";
		else
			nessid = cb->f[1];
		if (ctlr->ptype == 3){
			memset(ctlr->netname, 0, sizeof(ctlr->netname));
			strncpy(ctlr->netname, nessid, WNameLen);
		}
		else {
			memset(ctlr->wantname, 0, sizeof(ctlr->wantname));
			strncpy(ctlr->wantname, nessid, WNameLen);
		}
	} 
	else if(strcmp(cb->f[0], "station_name") == 0){
		memset(ctlr->nodename, 0, sizeof(ctlr->nodename));
		strncpy(ctlr->nodename, cb->f[1], WNameLen);
	}
	else if(strcmp(cb->f[0], "channel") == 0){
		i = atoi(cb->f[1]);
		if(i < 1 || i > 16 )
			error("channel not in [1-16]\n");
		ctlr->chan = i;
	}
	else if(strcmp(cb->f[0], "ptype") == 0){
		i = atoi(cb->f[1]);
		if(i < 1 || i > 3 )
			error("port type not in [1-3]");
		ctlr->ptype = i;
	}
	else if(strncmp(cb->f[0], "key", 3) == 0){
		strncpy(k,cb->f[0],64);
		if (strcmp(cb->f[1], "off") == 0)
			ctlr->keyid = 0;
		else if (strcmp(cb->f[1], "exclude") == 0){
			ctlr->xclear = 1;
		}
		else if (strcmp(cb->f[1], "include") == 0){
			ctlr->xclear = 0;
		}
		else {
			key = atoi(kn);
			if (key < 1 || key > WNKeys)
				error("key not in [1-4]");
			ctlr->keyid = key;
			kp = &ctlr->key[key-1];
			kp->len = strlen(cb->f[1]);
			if (kp->len > WKeyLen)
				kp->len = WKeyLen;
			memset(kp->dat, 0, sizeof(kp->dat));
			strncpy(kp->dat, cb->f[1], kp->len);
//			if (kp->len > WMinKeyLen)
//				kp->len = WKeyLen;
//			else if (kp->len > 0)
//				kp->len = WMinKeyLen;
		}
	} 
	else if(strcmp(cb->f[0], "pm") == 0){
		if(strcmp(cb->f[1], "off") == 0)
			ctlr->pmena = 0;
		else if(strcmp(cb->f[1], "on") == 0){
			ctlr->pmena = 1;
			if(cb->nf == 3){
				i = atoi(cb->f[2]);
				// check range here? what are the units?
				ctlr->pmwait = i;
			}
		}
		else
			error(Ebadctl);
	}
	else
		error(Ebadctl);

	if(ctlr->txbusy)
		w_txdone(ctlr, WTxErrEv|1);	// retry tx later.
	w_enable(ether);

	iunlock(&ctlr->Lock);
	poperror();
	free(cb);

	return n;
}

static void
transmit(Ether* ether)
{
	Ctlr* ctlr = ether->ctlr;

	if (ctlr == 0)
		return;

	ilock(&ctlr->Lock);
	ctlr->ntxrq++;
	w_txstart(ether,0);
	iunlock(&ctlr->Lock);
}

static void
promiscuous(void* arg, int on)
{
	Ether* ether = (Ether*)arg;
	Ctlr* ctlr = ether->ctlr;

	if (ctlr == nil)
		error("card not found");
	if (ctlr->attached == 0)
		error("card not attached");
	ilock(&ctlr->Lock);
	ltv_outs(ctlr, WTyp_Prom, (on?1:0));
	iunlock(&ctlr->Lock);
}



static void 
interrupt(Ureg* ,void* arg)
{
	Ether* ether = (Ether*) arg;
	Ctlr* ctlr = (Ctlr*) ether->ctlr;

	if (ctlr == 0)
		return;
	ilock(&ctlr->Lock);
	ctlr->nints++;
	w_intr(ether);
	iunlock(&ctlr->Lock);
}

static void
setopt(Ctlr* ctlr, char* opt, int no)
{
	int i;
	char k[64], *ke, *nessid;
	char *kn = &k[3];
	WKey *kp;
	int key;

	if (strncmp(opt,"essid=",6) == 0){
		if (strcmp(opt+6, "default") == 0)
			nessid = "";
		else
			nessid = opt+6;
		if (ctlr->ptype == 3){
			memset(ctlr->netname, 0, sizeof(ctlr->netname));
			strncpy(ctlr->netname, nessid, WNameLen);
		}
		else{
			memset(ctlr->wantname, 0, sizeof(ctlr->wantname));
			strncpy(ctlr->wantname, nessid, WNameLen);
		}
	} 
	else if (strncmp(opt,"station_name=",13) == 0){
		// The max. length of an 'opt' is ISAOPTLEN in dat.h.
		// It should be > 16 to give reasonable name lengths.
		memset(ctlr->nodename, 0, sizeof(ctlr->nodename));
		strncpy(ctlr->nodename, opt+13,WNameLen);
	} 
	else if (strncmp(opt, "channel=",8) == 0){
		i = atoi(opt+8);
		if(i < 1 || i > 16 )
			print("#l%d: channel (%d) not in [1-16]\n",no,i);
		else
			ctlr->chan = i;
	} 
	else if (strncmp(opt, "ptype=",6) == 0){
		i = atoi(opt+6);
		if(i < 1 || i > 3 )
			print("#l%d: port type (%d) not in [1-3]\n",no,i);
		else
			ctlr->ptype = i;
	}
	else if (strncmp(opt, "key", 3) == 0){
		if (opt[3] == 0 || opt[4] != '='){
			print("#l%d: key option is not keyX=xxxx\n", no);
			return;
		}
		strncpy(k,opt,64);
		key = atoi(kn);
		if (key < 1 || key > WNKeys){
			print("#l%d: key number (%d) not in [1-%d]\n",
					no, key, WNKeys);
			return;
		}
		ctlr->keyid = key;
		kp = &ctlr->key[key-1];
		ke = opt+5;
		while (*ke && *ke != ' ' && *ke != '\n' && *ke != '\t')
			ke++;
		kp->len = ke - (opt+5);
		if (kp->len > WKeyLen)
			kp->len = WKeyLen;
		memset(kp->dat, 0, sizeof(kp->dat));
		strncpy(kp->dat, opt+5, kp->len);
//		if (kp->len > WMinKeyLen)
//			kp->len = WKeyLen;
//		else if (kp->len > 0)
//			kp->len = WMinKeyLen;
	}
}

static int
reset(Ether* ether)
{
	Ctlr* ctlr;
	Wltv ltv;
	int i;

	if (ether->ctlr){
		print("#l%d: only one card supported\n", ether->ctlrno);
		return -1;
	}

	ether->arg = ctlr = (Ctlr*)emalloc(sizeof(Ctlr));
	ilock(&ctlr->Lock);
	
	if (ether->port==0)
		ether->port=WDfltIOB;
	ctlr->iob = ether->port;
	if (ether->irq==0)
		ether->irq=WDfltIRQ;
	if ((ctlr->slot = pcmspecial("WaveLAN/IEEE", ether))<0){
		DEBUG("no wavelan found\n");
		goto abort;
	}
	DEBUG("#l%d: port=0x%lx irq=%ld\n", 
			ether->ctlrno, ether->port, ether->irq);
  
	ctlr->chan = 0;
	ctlr->ptype= WDfltPType;
	ctlr->keyid= 0;
	*ctlr->netname = *ctlr->wantname = 0;
	strcpy(ctlr->nodename, "wvlancard");

	for (i=0; i < ether->nopt; i++)
		setopt(ctlr, ether->opt[i], ether->ctlrno);

	ctlr->netname[WNameLen-1] = 0;
	ctlr->wantname[WNameLen-1] = 0;
	ctlr->nodename[WNameLen-1] =0;

	if (ioalloc(ether->port,WIOLen,0,"wavelan")<0){
		print("#l%d: port 0x%lx in use\n", 
				ether->ctlrno, ether->port);
		goto abort;
	}

	w_intdis(ctlr);
	if (w_cmd(ctlr,WCmdIni,0)){
		print("#l%d: init failed\n", ether->ctlrno);
		goto abort;
	}
	w_intdis(ctlr);
	ltv_outs(ctlr, WTyp_Tick, 8);

	ltv.type = WTyp_Mac;
	ltv.len	= 4;
	if (w_inltv(ctlr, &ltv)){
		print("#l%d: unable to read mac addr\n", 
			ether->ctlrno);
		goto abort;
	}
	memmove(ether->ea, ltv.addr, Eaddrlen);
	DEBUG("#l%d: %2.2uX%2.2uX%2.2uX%2.2uX%2.2uX%2.2uX\n", 
			ether->ctlrno,
			ether->ea[0], ether->ea[1], ether->ea[2],
			ether->ea[3], ether->ea[4], ether->ea[5]);

	if (ctlr->chan == 0)
		ctlr->chan = ltv_ins(ctlr, WTyp_Chan);
	ctlr->apdensity = WDfltApDens;
	ctlr->rtsthres = WDfltRtsThres;
	ctlr->txrate = WDfltTxRate;
	ctlr->maxlen = WMaxLen;
	ctlr->pmena = 0;
	ctlr->pmwait= 100;
	// link to ether
	ether->ctlr = ctlr;
	ether->mbps = 10;	
	ether->attach = attach;
	ether->interrupt = interrupt;
	ether->transmit = transmit;
	ether->ifstat = ifstat;
	ether->ctl = ctl;
	ether->promiscuous = promiscuous;
	ether->multicast = multicast;
	ether->arg = ether;

	DEBUG("#l%d: irq %ld port %lx type %s",
		ether->ctlrno, ether->irq, ether->port,	ether->type);
	DEBUG(" %2.2uX%2.2uX%2.2uX%2.2uX%2.2uX%2.2uX\n",
		ether->ea[0], ether->ea[1], ether->ea[2],
		ether->ea[3], ether->ea[4], ether->ea[5]);

	iunlock(&ctlr->Lock);
	return 0; 

abort:
	iunlock(&ctlr->Lock);
	free(ctlr);
	ether->ctlr = nil;
	return -1;
}

void
etherwavelanlink(void)
{
	addethercard("wavelan", reset);
}
