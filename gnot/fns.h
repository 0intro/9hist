#include "../port/portfns.h"

#define	P_oper(sel, inst)	(P_qlock(sel), inst, P_qunlock(sel))
#define	P_qlock(sel)	(sel >= 0 ? (qlock(&portpage), PORTSELECT = portpage.select = sel) : -1)
#define	P_qunlock(sel)	(sel >= 0 ? (qunlock(&portpage),0) : -1)
#define	P_read(sel, addr, val, type)	P_oper(sel, val = *(type *)(PORT+addr))
#define	P_write(sel, addr, val, type)	P_oper(sel, *(type *)(PORT+addr) = val)

void	addportintr(int (*)(void));
void	clearmmucache(void);
void	duartbaud(int);
void	duartbreak(int);
void	duartdtr(int);
int	duartinputport(void);
void	duartstartrs232o(void);
void	duartstarttimer(void);
void	duartstoptimer(void);
#define	evenaddr(x)	/* 68020 doesn't care */
void	fault68020(Ureg*, FFrame*);
#define	flushapage(x)
void	flushcpucache(void);
#define	flushpage(x) if(x)
#define	flushvirt()
int	fpcr(int);
void	fpregrestore(char*);
void	fpregsave(char*);
void	fprestore(FPsave*);
void	fpsave(FPsave*);
int	getrs232o(void);
void	incontoggle(void);
void	kbdchar(int);
void	kbdclock(void);
void	kbdrepeat(int);
KMap*	kmap(Page*);
void	kmapinit(void);
int	kprint(char*, ...);
void	kunmap(KMap*);
void	mmuinit(void);
void	mousebuttons(int);
void	mouseclock(void);
int	portprobe(char*, int, int, int, long);
void	printinit(void);
void	procrestore(Proc*, uchar*);
void	procsave(uchar*, int);
void	procsetup(Proc*);
void	putkmmu(ulong, ulong);
void	putstr(char*);
void	putstrn(char*, long);
void	restartprint(Alarm*);
void	restfpregs(FPsave*);
void	rs232ichar(int);
void	screeninit(void);
void	screenputc(int);
int	scsicap(int, uchar *);
Scsi*	scsicmd(int, int, long);
void	scsictrlintr(void);
void	scsidmaintr(void);
int	scsiexec(Scsi *, int);
void	scsiinit(void);
int	scsiready(int);
uchar*	scsirecv(uchar *);
void	scsireset(void);
void	scsirun(void);
int	scsisense(int, uchar *);
uchar*	scsixmit(uchar *);
int	spl1(void);
void	spldone(void);
int	splduart(void);
int	tas(char*);
void	touser(void);
#define	waserror()	(u->nerrlab++, setlabel(&u->errlab[u->nerrlab-1]))
