#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

#include	"gnot.h"

#define	MINX	8

extern	Font	defont0;

struct{
	Point	pos;
	int	bwid;
}out;

Bitmap	screen =
{
	(ulong*)((4*1024*1024-256*1024)|KZERO),	/* BUG */
	0,
	64,
	0,
	0, 0, 1024, 1024,
	0
};

void
screeninit(void)
{
	bitblt(&screen, Pt(0, 0), &screen, screen.rect, 0);
	out.pos.x = MINX;
	out.pos.y = 0;
	out.bwid = defont0.info[' '].width;
}

void
screenputc(int c)
{
	char buf[2];
	int nx;

	if(c == '\n'){
		out.pos.x = MINX;
		out.pos.y += defont0.height;
		if(out.pos.y > screen.rect.max.y-defont0.height)
			out.pos.y = screen.rect.min.y;
		bitblt(&screen, Pt(0, out.pos.y), &screen,
		    Rect(0, out.pos.y, screen.rect.max.x, out.pos.y+2*defont0.height), 0);
	}else if(c == '\t'){
		nx = out.pos.x + (8-(out.pos.x/out.bwid&7))*out.bwid;
		out.pos.x = nx;
		if(out.pos.x >= screen.rect.max.x)
			screenputc('\n');
	}else if(c == '\b'){
		if(out.pos.x >= out.bwid+MINX){
			out.pos.x -= out.bwid;
			screenputc(' ');
			out.pos.x -= out.bwid;
		}
	}else{
		if(out.pos.x >= screen.rect.max.x-out.bwid)
			screenputc('\n');
		buf[0] = c&0x7F;
		buf[1] = 0;
		out.pos = string(&screen, out.pos, &defont0, buf, S);
	}
}

/*
 * Register set for half the duart.  There are really two sets.
 */
struct Duart{
	uchar	mr1_2;		/* Mode Register Channels 1 & 2 */
	uchar	sr_csr;		/* Status Register/Clock Select Register */
	uchar	cmnd;		/* Command Register */
	uchar	data;		/* RX Holding / TX Holding Register */
	uchar	ipc_acr;	/* Input Port Change/Aux. Control Register */
	uchar	is_imr;		/* Interrupt Status/Interrupt Mask Register */
	uchar	ctur;		/* Counter/Timer Upper Register */
	uchar	ctlr;		/* Counter/Timer Lower Register */
};

enum{
	CHAR_ERR	=0x00,	/* MR1x - Mode Register 1 */
	EVEN_PAR	=0x00,
	ODD_PAR		=0x04,
	NO_PAR		=0x10,
	CBITS8		=0x03,
	CBITS7		=0x02,
	CBITS6		=0x01,
	CBITS5		=0x00,
	NORM_OP		=0x00,	/* MR2x - Mode Register 2 */
	TWOSTOPB	=0x0F,
	ONESTOPB	=0x07,
	ENB_RX		=0x01,	/* CRx - Command Register */
	DIS_RX		=0x02,
	ENB_TX		=0x04,
	DIS_TX		=0x08,
	RESET_MR 	=0x10,
	RESET_RCV  	=0x20,
	RESET_TRANS  	=0x30,
	RESET_ERR  	=0x40,
	RESET_BCH	=0x50,
	STRT_BRK	=0x60,
	STOP_BRK	=0x70,
	RCV_RDY		=0x01,	/* SRx - Channel Status Register */
	FIFOFULL	=0x02,
	XMT_RDY		=0x04,
	XMT_EMT		=0x08,
	OVR_ERR		=0x10,
	PAR_ERR		=0x20,
	FRM_ERR		=0x40,
	RCVD_BRK	=0x80,
	IM_IPC		=0x80,	/* IMRx/ISRx - Interrupt Mask/Interrupt Status */
	IM_DBB		=0x40,
	IM_RRDYB	=0x20,
	IM_XRDYB	=0x10,
	IM_CRDY		=0x08,
	IM_DBA		=0x04,
	IM_RRDYA	=0x02,
	IM_XRDYA	=0x01,
};

uchar keymap[]={
/*80*/	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,
	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0x8e,	0x58,
/*90*/	0x90,	0x91,	0x92,	0x93,	0x94,	0x95,	0x96,	0x97,
	0x98,	0x99,	0x9a,	0x9b,	0x58,	0x58,	0x58,	0x58,
/*A0*/	0x58,	0xa1,	0xa2,	0xa3,	0xa4,	0xa5,	0xa6,	0xa7,
	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0xae,	0xaf,
/*B0*/	0xb0,	0xb1,	0xb2,	0xb3,	0xb4,	0xb5,	0x80,	0xb7,
	0xb8,	0xb9,	0x00,	0xbb,	0x1e,	0xbd,	0x60,	0x1f,
/*C0*/	0xc0,	0xc1,	0xc2,	0xc3,	0xc4,	0x58,	0xc6,	0x0d,
	0xc8,	0xc9,	0xca,	0xcb,	0xcc,	0xcd,	0xce,	0xcf,
/*D0*/	0x09,	0x08,	0xd2,	0xd3,	0xd4,	0xd5,	0xd6,	0xd7,
	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0x7f,	0x58,
/*E0*/	0x58,	0x58,	0xe2,	0x1b,	0x0a,	0xe5,	0x58,	0x0d,
	0xe8,	0xe9,	0xea,	0xeb,	0xec,	0xed,	0xee,	0xef,
/*F0*/	0x09,	0x08,	0xb2,	0x1b,	0x0a,	0xf5,	0x81,	0x58,
	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0x7f,	0xb2,
};

void
duartintr(void)
{
	int cause, status, c;
	Duart *duart;

	duart = DUARTREG;
	cause = duart->is_imr;
	/*
	 * I can guess your interrupt.
	 */
	/*
	 * Is it 1?
	 */
	if(cause & IM_RRDYA){		/* keyboard input */
		status = duart->sr_csr;
		c = duart->data;
		if(status & (FRM_ERR|OVR_ERR|PAR_ERR))
			duart->cmnd = RESET_ERR;
		if(c == 0x7F)
			c = 0xFF;	/* VIEW key (bizarre) */
		if(c & 0x80)
			c = keymap[c&0x7F];
		kbdchar(c);
	}
	/*
	 * Is it 2?
	 */
	if(cause & IM_RRDYB)		/* pen input */
		c = duart[1].data;
}

