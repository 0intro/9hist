#undef	CHDIR	/* BUG */
#include "/sys/src/libc/9syscall/sys.h"

typedef long Syscall(ulong*);
Syscall sysbind, sysbrk_, syschdir, sysclose, syscreate, sysdeath;
Syscall	sysdup, syserrstr, sysexec, sysexits, sysfork, sysforkpgrp;
Syscall	sysfstat, sysfwstat, sysgetpid, sysmount, sysnoted;
Syscall	sysnotify, sysopen, syspipe, sysr1, sysread, sysremove, sysseek;
Syscall syssleep, sysstat, syswait, syswrite, syswstat, sysalarm, syssegbrk;
Syscall syssegattach, syssegdetach, syssegfree, syssegflush;
Syscall sysrendezvous, sysrfork, sysunmount;

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
	[RFORK]		sysrfork,
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
	[UNMOUNT]	sysunmount,
};

char *sysctab[]={
	[SYSR1]		"Running",
	[ERRSTR]	"Errstr",
	[BIND]		"Bind",
	[CHDIR]		"Chdir",
	[CLOSE]		"Close",
	[DUP]		"Dup",
	[ALARM]		"Alarm",
	[EXEC]		"Exec",
	[EXITS]		"Exits",
	[FORK]		"Fork",
	[FORKPGRP]	"Forkpgrp",
	[FSTAT]		"Fstat",
	[SEGBRK]	"Segbrk",
	[MOUNT]		"Mount",
	[OPEN]		"Open",
	[READ]		"Read",
	[SEEK]		"Seek",
	[SLEEP]		"Sleep",
	[STAT]		"Stat",
	[WAIT]		"Wait",
	[WRITE]		"Write",
	[PIPE]		"Pipe",
	[CREATE]	"Create",
	[RFORK]		"Rfork",
	[BRK_]		"Brk",
	[REMOVE]	"Remove",
	[WSTAT]		"Wstat",
	[FWSTAT]	"Fwstat",
	[NOTIFY]	"Notify",
	[NOTED]		"Noted",
	[SEGATTACH]	"Segattach",
	[SEGDETACH]	"Segdetach",
	[SEGFREE]	"Segfree",
	[SEGFLUSH]	"Segflush",
	[RENDEZVOUS]	"Rendez",
	[UNMOUNT]	"Unmount",
};
