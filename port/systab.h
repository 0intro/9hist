#undef	CHDIR	/* BUG */
#include "/sys/src/libc/9syscall/sys.h"

typedef long Syscall(ulong*);

Syscall sysr1;Syscall ;
Syscall syserrstr;Syscall ;
Syscall sysbind;Syscall ;
Syscall syschdir;Syscall ;
Syscall sysclose;Syscall ;
Syscall sysdup;Syscall ;
Syscall sysalarm;Syscall ;
Syscall sysexec;Syscall ;
Syscall sysexits;Syscall ;
Syscall sysfsession;Syscall ;
Syscall sysfauth;Syscall ;
Syscall sysfstat;Syscall ;
Syscall syssegbrk;Syscall ;
Syscall sysmount;Syscall ;
Syscall sysopen;Syscall ;
Syscall sysread;Syscall ;
Syscall sysoseek;Syscall ;
Syscall syssleep;Syscall ;
Syscall sysstat;Syscall ;
Syscall sysrfork;Syscall ;
Syscall syswrite;Syscall ;
Syscall syspipe;Syscall ;
Syscall syscreate;Syscall ;
Syscall sysfd2path;Syscall ;
Syscall sysbrk_;Syscall ;
Syscall sysremove;Syscall ;
Syscall syswstat;Syscall ;
Syscall sysfwstat;Syscall ;
Syscall sysnotify;Syscall ;
Syscall sysnoted;Syscall ;
Syscall syssegattach;Syscall ;
Syscall syssegdetach;Syscall ;
Syscall syssegfree;Syscall ;
Syscall syssegflush;Syscall ;
Syscall sysrendezvous;Syscall ;
Syscall sysunmount;Syscall ;
Syscall syswait;Syscall ;
Syscall syswrite9p;Syscall ;
Syscall sysread9p;Syscall ;
Syscall sysseek;Syscall ;
Syscall systunnel;Syscall ;
Syscall sysexportfs;Syscall ;
Syscall	sysdeath;

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
	[FSESSION]	sysfsession,
	[FAUTH]		sysfauth,
	[FSTAT]		sysfstat,
	[SEGBRK]	syssegbrk,
	[MOUNT]		sysmount,
	[OPEN]		sysopen,
	[READ]		sysread,
	[OSEEK]		sysoseek,
	[SLEEP]		syssleep,
	[STAT]		sysstat,
	[RFORK]		sysrfork,
	[WRITE]		syswrite,
	[PIPE]		syspipe,
	[CREATE]	syscreate,
	[FD2PATH]	sysfd2path,
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
	[WAIT]		syswait,
	[WRITE9P]	syswrite9p,
	[READ9P]	sysread9p,
	[SEEK]		sysseek,
	[TUNNEL]	systunnel,
	[EXPORTFS]	sysexportfs,
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
	[FSESSION]	"Fsession",
	[FAUTH]		"Fauth",
	[FSTAT]		"Fstat",
	[SEGBRK]	"Segbrk",
	[MOUNT]		"Mount",
	[OPEN]		"Open",
	[READ]		"Read",
	[OSEEK]		"Oseek",
	[SLEEP]		"Sleep",
	[STAT]		"Stat",
	[RFORK]		"Rfork",
	[WRITE]		"Write",
	[PIPE]		"Pipe",
	[CREATE]	"Create",
	[FD2PATH]	"Fd2path",
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
	[WAIT]		"Wait",
	[WRITE9P]	"Write9p",
	[READ9P]	"Read9p",
	[SEEK]		"Seek",
	[TUNNEL]	"Tunnel",
	[EXPORTFS]	"Exportfs",
};

int nsyscall = (sizeof systab/sizeof systab[0]);
