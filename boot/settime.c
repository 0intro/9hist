#include <u.h>
#include <libc.h>
#include <fcall.h>
#include "../boot/boot.h"

void
settime(int islocal)
{
	int n, f;
	int timeset;
	Dir dir;
	char dirbuf[DIRLEN];
	char *srvname;

	print("time...");
	timeset = 0;
	if(islocal){
		/*
		 *  set the time from the real time clock
		 */
		f = open("#r/rtc", ORDWR);
		if(f >= 0){
			if((n = read(f, dirbuf, sizeof(dirbuf)-1)) > 0){
				dirbuf[n] = 0;
				timeset = 1;
			}
			close(f);
		}
	}
	if(timeset == 0){
		/*
		 *  set the time from the access time of the root
		 */
		f = open("#s/boot", ORDWR);
		if(f < 0)
			return;
		if(mount(f, "/n/boot", MREPL, "", "") < 0){
			close(f);
			return;
		}
		close(f);
		if(stat("/n/boot", dirbuf) < 0)
			fatal("stat");
		convM2D(dirbuf, &dir);
		sprint(dirbuf, "%ld", dir.atime);
		unmount(0, "/n/boot");
		/*
		 *  set real time clock if there is one
		 */
		f = open("#r/rtc", ORDWR);
		if(f > 0){
			write(f, dirbuf, strlen(dirbuf));
			close(f);
		}
		close(f);
	}

	f = open("#c/time", OWRITE);
	write(f, dirbuf, strlen(dirbuf));
	close(f);
}
