#define	SET(x)	x = 0
#define	USED(x)	if(x)

void	alarminit(void);
Alarm*	alarm(int, void (*)(Alarm*), void*);
Block*	allocb(ulong);
int	anyready(void);
void	append(List**, List*);
int	blen(Block *);
int	bround(Block *, int);
void	buzz(int, int);
void	cancel(Alarm*);
int	cangetc(void*);
int	canlock(Lock*);
int	canqlock(QLock*);
int	canputc(void*);
void	chandevinit(void);
void	chandevreset(void);
void	chaninit(void);
void	checkalarms(void);
void	clock(Ureg*);
Chan*	clone(Chan*, Chan*);
void	close(Chan*);
void	closemount(Mount*);
void	closepgrp(Pgrp*);
long	clrfpintr(void);
int	compactpte(Orig*, ulong);
void	confinit(void);
int	consactive(void);
Env*	copyenv(Env*, int);
int	decref(Ref*);
void	delay(int);
void	delete0(List**, List*);
void	delete(List**, List*, List*);
Chan*	devattach(int, char*);
Chan*	devclone(Chan*, Chan*);
void	devdir(Chan*, Qid, char*, long, long, Dir*);
long	devdirread(Chan*, char*, long, Dirtab*, int, Devgen*);
Devgen	devgen;
int	devno(int, int);
Chan*	devopen(Chan*, int, Dirtab*, int, Devgen*);
void	devstat(Chan*, char*, Dirtab*, int, Devgen*);
int	devwalk(Chan*, char*, Dirtab*, int, Devgen*);
void*	dmaalloc(ulong);
void	dumpqueues(void);
void	dumpregs(Ureg*);
void	dumpstack(void);
void	envpgclose(Env *);
void	envcpy(Pgrp*, Pgrp*);
int	eqchan(Chan*, Chan*, long);
int	eqqid(Qid, Qid);
void	error(int);
void	errors(char*);
void	execpc(ulong);
void	exit(void);
int	fault(ulong, int);
void	fdclose(int);
Chan*	fdtochan(int, int);
void	firmware(void);
void	flowctl(Queue*);
void	flushmmu(void);
void	forkmod(Seg*, Seg*, Proc*);
void	freealarm(Alarm*);
void	freeb(Block*);
int	freebroken(void);
void	freechan(Chan*);
void	freepage(Orig*, int);
void	freepte(Orig*);
void	freesegs(int);
Block*	getb(Blist*);
int	getc(IOQ*);
int	getfields(char*, char**, int, char);
Block*	getq(Queue*);
int	gets(IOQ*, void*, int);
void	gotolabel(Label*);
void	growpte(Orig*, ulong);
void*	ialloc(ulong, int);
long	ibrk(ulong, int);
int	incref(Ref*);
void	initq(IOQ*);
void	insert(List**, List*, List*);
void	invalidateu(void);
void	isdir(Chan*);
void	kbdclock(void);
void	kbdrepeat(int);
int	kbdputc(IOQ*, int);
int	kprint(char*, ...);
void	kproc(char*, void(*)(void*), void*);
void	lights(int);
Page*	lkpage(Orig*, ulong);
void	lkpgfree(Page*, int);
void	lock(Lock*);
void	lockinit(void);
Orig*	lookorig(ulong, ulong, int, Chan*);
void	machinit(void);
void	mapstack(Proc*);
void	mklockseg(Seg*);
void	mntdump(void);
void	mmurelease(Proc*);
int	mount(Chan*, Chan*, int);
int	mouseputc(IOQ*, int);
Chan*	namec(char*, int, int, ulong);
Alarm*	newalarm(void);
Chan*	newchan(void);
PTE*	newmod(Orig*);
Mount*	newmount(void);
Orig*	neworig(ulong, ulong, int, Chan*);
Page*	newpage(int, Orig*, ulong);
Pgrp*	newpgrp(void);
Proc*	newproc(void);
void	newqinfo(Qinfo*);
char*	nextelem(char*, char*);
void	nexterror(void);
void	notify(Ureg*);
void	nullput(Queue*, Block*);
int	openmode(ulong);
Block*	padb(Block*, int);
void	pageinit(void);
void	panic(char*, ...);
void	pexit(char*, int);
void	pgrpcpy(Pgrp*, Pgrp*);
void	pgrpinit(void);
void	pgrpnote(Pgrp*, char*, long, int);
Pgrp*	pgrptab(int);
#define	poperror()	u->nerrlab--
int	postnote(Proc*, int, char*, int);
int	pprint(char*, ...);
void	printinit(void);
int	putc(IOQ*, int);
void	putstr(char*);
void	putstrn(char*, long);
void	puts(IOQ*, void*, int);
ulong 	procalarm(ulong);
void	procdump(void);
void	procinit0(void);
Proc*	proctab(int);
Block*	pullup(Block *, int);
Queue*	pushq(Stream*, Qinfo*);
int	putb(Blist*, Block*);
void	putbq(Blist*, Block*);
void	putmmu(ulong, ulong);
int	putq(Queue*, Block*);
ulong	pwait(Waitmsg*);
void	qlock(QLock*);
void	qunlock(QLock*);
int	readnum(ulong, char*, ulong, ulong, int);
void	ready(Proc*);
void	resched(char*);
int	return0(void*);
Proc*	runproc(void);
void	savefpregs(FPsave*);
void	sccintr(void);
void	sccputs(IOQ*, char*, int);
void	sccsetup(void*);
void	sccspecial(int, IOQ*, IOQ*, int);
void	schedinit(void);
void	sched(void);
long	seconds(void);
Seg*	seg(Proc*, ulong);
int	segaddr(Seg*, ulong, ulong);
int	setlabel(Label*);
char*	skipslash(char*);
void	sleep(Rendez*, int(*)(void*), void*);
int	splhi(void);
int	spllo(void);
void	splx(int);
int	streamclose1(Stream*);
int	streamclose(Chan*);
int	streamenter(Stream*);
int	streamexit(Stream*, int);
Devgen	streamgen;
void	streaminit0(void);
void	streaminit(void);
Stream*	streamnew(ushort, ushort, ushort, Qinfo*, int);
void	streamopen(Chan*, Qinfo*);
int	streamparse(char*, Block*);
long	streamread(Chan*, void*, long);
void	streamstat(Chan*, char*, char*);
long	streamwrite(Chan*, void*, long, int);
long	stringread(Chan*, void*, long, char*, ulong);
long	syscall(Ureg*);
void	tsleep(Rendez*, int (*)(void*), void*, int);
void	twakeme(Alarm*);
long	unionread(Chan*, void*, long);
void	unlock(Lock*);
void	unusepage(Page*, int);
void	usepage(Page*, int);
void	userinit(void);
void	validaddr(ulong, ulong, int);
void*	vmemchr(void*, int, ulong);
void	wakeme(Alarm*);
void	wakeup(Rendez*);
