#undef	CHDIR	/* BUG */
#include "/sys/src/libc/9syscall/sys.h"

typedef long Syscall(ulong*);
Syscall sysbind, sysbrk_, syschdir, sysclose, syscreate, sysdeath;
Syscall	sysdup, syserrstr, sysexec, sysexits, sysfork, sysforkpgrp;
Syscall	sysfstat, sysfwstat, sysgetpid, sysmount, sysnoted;
Syscall	sysnotify, sysopen, syspipe, sysr1, sysread, sysremove, sysseek;
Syscall syssleep, sysstat, syswait, syswrite, syswstat, sysalarm, syssegbrk;
Syscall syssegattach, syssegdetach, syssegfree, syssegflush;
Syscall sysrendezvous;

Syscall *systab[]={
	[SYSR1]		sysr1,
	[ERRSTR]	syserrstr,
	[BIND]		sysbind,
	[CHDIR]		syschdir,
	[CLOSE]		sysclose,
	[DUP]		sysdup,
	[ALARM]		sysalarm,
	[EXEC]		sysexec,
	[EXITS]		sysexits,
	[FORK]		sysfork,
	[FORKPGRP]	sysforkpgrp,
	[FSTAT]		sysfstat,
	[SEGBRK]	syssegbrk,
	[MOUNT]		sysmount,
	[OPEN]		sysopen,
	[READ]		sysread,
	[SEEK]		sysseek,
	[SLEEP]		syssleep,
	[STAT]		sysstat,
	[WAIT]		syswait,
	[WRITE]		syswrite,
	[PIPE]		syspipe,
	[CREATE]	syscreate,
	[___USERSTR___]	sysdeath,
	[BRK_]		sysbrk_,
	[REMOVE]	sysremove,
	[WSTAT]		syswstat,
	[FWSTAT]	sysfwstat,
	[NOTIFY]	sysnotify,
	[NOTED]		sysnoted,
	[SEGATTACH]	syssegattach,
	[SEGDETACH]	syssegdetach,
	[SEGFREE]	syssegfree,
	[SEGFLUSH]	syssegflush,
	[RENDEZVOUS]	sysrendezvous,
};
