void		addrootfile(char*, uchar*, ulong);
void		alarmkproc(void*);
Block*		allocb(int);
int		anyready(void);
Image*		attachimage(int, Chan*, ulong, ulong);
long		authcheck(Chan*, char*, int);
void		authclose(Chan*);
long		authentread(Chan*, char*, int);
long		authentwrite(Chan*, char*, int);
long		authread(Chan*, char*, int);
void		authreply(Session*, ulong, Fcall*);
ulong		authrequest(Session*, Fcall*);
long		authwrite(Chan*, char*, int);
Page*		auxpage(void);
void		buzz(int, int);
void		cachedel(Image*, ulong);
void		cachepage(Page*, Image*);
void		callbacks(void);
void		newcallback(void (*)(void));
int		cangetc(void*);
int		canlock(Lock*);
int		canpage(Proc*);
int		canputc(void*);
int		canqlock(QLock*);
void		chandevinit(void);
void		chandevreset(void);
void		chanfree(Chan*);
void		chanrec(Mnt*);
void		checkalarms(void);
void		cinit(void);
Chan*		clone(Chan*, Chan*);
void		close(Chan*);
void		closeegrp(Egrp*);
void		closefgrp(Fgrp*);
void		closemount(Mount*);
void		closepgrp(Pgrp*);
long		clrfpintr(void);
void		confinit(void);
void		confinit1(int);
int		consactive(void);
void		consdebug(void);
void		copen(Chan*);
void		copypage(Page*, Page*);
int		cread(Chan*, uchar*, int, ulong);
void		cupdate(Chan*, uchar*, int, ulong);
void		cursoroff(int);
void		cursoron(int);
void		cwrite(Chan*, uchar*, int, ulong);
int		decref(Ref*);
int		decrypt(void*, void*, int);
void		delay(int);
Chan*		devattach(int, char*);
Chan*		devclone(Chan*, Chan*);
void		devdir(Chan*, Qid, char*, long, char*, long, Dir*);
long		devdirread(Chan*, char*, long, Dirtab*, int, Devgen*);
Devgen		devgen;
int		devno(int, int);
Chan*		devopen(Chan*, int, Dirtab*, int, Devgen*);
void		devstat(Chan*, char*, Dirtab*, int, Devgen*);
int		devwalk(Chan*, char*, Dirtab*, int, Devgen*);
Chan*		domount(Chan*);
void		dumpqueues(void);
void		dumpregs(Ureg*);
void		dumpstack(void);
Fgrp*		dupfgrp(Fgrp*);
void		duppage(Page*);
void		dupswap(Page*);
int		encrypt(void*, void*, int);
void		envcpy(Egrp*, Egrp*);
int		eqchan(Chan*, Chan*, long);
int		eqqid(Qid, Qid);
void		error(char*);
long		execregs(ulong, ulong, ulong);
void		exhausted(char*);
void		exit(int);
int		fault(ulong, int);
void		fdclose(int, int);
Chan*		fdtochan(int, int, int, int);
int		fixfault(Segment*, ulong, int, int);
void		flushmmu(void);
void		forkchild(Proc*, Ureg*);
void		forkret(void);
void		free(void*);
int		freebroken(void);
void		freechan(Chan*);
void		freepte(Segment*, Pte*);
void		freesegs(int);
void		freesession(Session*);
void		getcolor(ulong, ulong*, ulong*, ulong*);
int		getfields(char*, char**, int, char);
void		gotolabel(Label*);
int		haswaitq(void*);
long		hostdomainwrite(char*, int);
long		hostownerwrite(char*, int);
void		hwcursmove(int, int);
void		hwcursset(ulong*, ulong*, int, int);
void		iallocinit(void);
long		ibrk(ulong, int);
int		incref(Ref*);
void		initscsi(void);
void		initseg(void);
void		isdir(Chan*);
int		iseve(void);
int		ispages(void*);
void		ixsummary(void);
void		kbdclock(void);
int		kbdcr2nl(Queue*, int);
int		kbdputc(Queue*, int);
void		kbdrepeat(int);
long		keyread(char*, int, long);
long		keywrite(char*, int);
void		kickpager(void);
void		killbig(void);
int		kprint(char*, ...);
void		kproc(char*, void(*)(void*), void*);
void		kprocchild(Proc*, void (*)(void*), void*);
void		kproftimer(ulong);
void		ksetenv(char*, char*);
long		latin1(uchar*);
void		lights(int);
void		links(void);
void		lock(Lock*);
void		lockinit(void);
Page*		lookpage(Image*, ulong);
int		m3mouseputc(int);
void		machinit(void);
void*		malloc(ulong);
void		mbbpt(Point);
void		mbbrect(Rectangle);
void		mfreeseg(Segment*, ulong, int);
void		mmurelease(Proc*);
void		mmuswitch(Proc*);
void		mntdump(void);
void		mntrepl(char*);
int		mount(Chan*, Chan*, int, char*);
void		mountfree(Mount*);
void		mousebuttons(int);
void		mouseclock(void);
void		mousectl(char*);
int		mouseputc(int);
void		mousescreenupdate(void);
void		mousetrack(int, int, int);
int		msize(void*);
Chan*		namec(char*, int, int, ulong);
void		nameok(char*);
Chan*		newchan(void);
Mount*		newmount(Mhead*, Chan*, int, char*);
Page*		newpage(int, Segment **, ulong);
Pgrp*		newpgrp(void);
Proc*		newproc(void);
char*		nextelem(char*, char*);
void		nexterror(void);
int		notify(Ureg*);
int		nrand(int);
int		okaddr(ulong, ulong, int);
int		openmode(ulong);
void		pageinit(void);
void		panic(char*, ...);
void		pexit(char*, int);
void		pgrpcpy(Pgrp*, Pgrp*);
void		pgrpnote(ulong, char*, long, int);
Pgrp*		pgrptab(int);
void		pio(Segment *, ulong, ulong, Page **);
void		pixreverse(uchar*, int, int);
#define		poperror()		up->nerrlab--
int		postnote(Proc*, int, char*, int);
int		pprint(char*, ...);
void		printinit(void);
ulong		procalarm(ulong);
int		proccounter(char *name);
void		procctl(Proc*);
void		procdump(void);
void		procinit0(void);
Proc*		proctab(int);
void		ptclone(Chan*, int, int);
void		ptclose(Pthash*);
Pte*		ptealloc(void);
Pte*		ptecpy(Pte*);
Path*		ptenter(Pthash*, Path*, char*);
int		ptpath(Path*, char*, int);
void		putimage(Image*);
void		putmmu(ulong, ulong, Page*);
void		putpage(Page*);
void		putseg(Segment*);
void		putstr(char*);
void		putstr(char*);
void		putstrn(char*, long);
void		putswap(Page*);
ulong		pwait(Waitmsg*);
int		qcanread(Queue*);
void		qclose(Queue*);
int		qconsume(Queue*, void*, int);
void		qhangup(Queue*);
void		qinit(void);
int		qlen(Queue*);
void		qlock(QLock*);
Queue*		qopen(int, int, void (*)(void*), void*);
int		qpass(Queue*, Block*);
int		qproduce(Queue*, void*, int);
long		qread(Queue*, void*, int);
void		qreopen(Queue*);
void		qunlock(QLock*);
long		qwrite(Queue*, void*, int, int);
int		readnum(ulong, char*, ulong, ulong, int);
int		readstr(ulong, char*, ulong, char*);
void		ready(Proc*);
void		relocateseg(Segment*, ulong);
void		resched(char*);
void		resetscsi(void);
void		resrcwait(char*);
int		return0(void*);
void		rlock(RWlock*);
void		rootrecover(Path*, char*);
void		rootreq(Chan*, Mnt*);
void		runlock(RWlock*);
Proc*		runproc(void);
void		savefpregs(FPsave*);
void		sccclock(void);
int		sccintr(void);
void		sccsetup(void*, ulong, int);
void		sched(void);
void		schedinit(void);
int		screenbits(void);
void		screenupdate(void);
int		scsiexec(Target*, int, uchar*, int, void*, int*);
int		scsiinv(int, int, Target**, uchar**, char*);
Target*		scsiunit(int, int);
long		seconds(void);
ulong		segattach(Proc*, ulong, char *, ulong, ulong);
void		segpage(Segment*, Page*);
int		setcolor(ulong, ulong, ulong, ulong);
void		setkernur(Ureg*, Proc*);
int		setlabel(Label*);
void		setregisters(Ureg*, char*, char*, int);
void		setswapchan(Chan*);
char*		skipslash(char*);
void		sleep(Rendez*, int(*)(void*), void*);
void*		smalloc(ulong);
int		splhi(void);
int		spllo(void);
void		splx(int);
void		srvrecover(Chan*, Chan*);
void		swapinit(void);
void		tsleep(Rendez*, int (*)(void*), void*, int);
void		unbreak(Proc*);
void		uncachepage(Page*);
long		unicode(uchar*);
long		unionread(Chan*, void*, long);
void		unlock(Lock*);
void		unmount(Chan*, Chan*);
void		userinit(void);
ulong		userpc(void);
long		userwrite(char*, int);
void		validaddr(ulong, ulong, int);
void		vcacheinval(Page*, ulong);
void*		vmemchr(void*, int, int);
void		wakeup(Rendez*);
Chan*		walk(Chan*, char*, int);
void		wlock(RWlock*);
void		wunlock(RWlock*);
void*		xalloc(ulong);
void*		xallocz(ulong, int);
void		xfree(void*);
void		xhole(ulong, ulong);
void		xinit(void);
void*		xspanalloc(ulong, int, ulong);
void		xsummary(void);
Segment*	data2txt(Segment*);
Segment*	dupseg(Segment**, int, int);
Segment*	newseg(int, ulong, ulong);
Segment*	seg(Proc*, ulong, int);
