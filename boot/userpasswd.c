#include <u.h>
#include <libc.h>
#include <auth.h>
#include <../boot/boot.h>

char	username[64];
char	password[64];
#ifdef asdf
extern	char *sauth;
#endif asdf

char *homsg = "can't set user name or key; please reboot";

/*
 *  get/set user name.
 */
void
setusername(int, Method*)
{
	if(*username == 0 || strcmp(username, "none") == 0){
		strcpy(username, "none");
		outin("user", username, sizeof(username));
	}

	/* set host's owner (and uid of current process) */
	if(writefile("#c/hostowner", username, strlen(username)) < 0)
		fatal(homsg);
}
