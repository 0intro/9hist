#include <u.h>
#include <libc.h>
#include <../boot/boot.h>

/* minimal rc main */
char rcmain[] = "home=/\n"
		"ifs=' \t\n'\n"
		"prompt=('% ' '\t')\n"
		"path=/\n";
		
void
configrc(Method *)
{
	setenv("rcmain", rcmain);
	execl("/rc", "/rc", "-m", "#e/rcmain", "-i", 0);
	fatal("rc");
}

int
connectrc(void)
{
	// does not get here
	return -1;
}
