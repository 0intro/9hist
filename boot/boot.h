typedef struct Method	Method;
struct Method
{
	char	*name;
	void	(*config)(Method*);
	void	(*auth)(Method*);
	void	(*connect)(Method*);
	char	*arg;
};

extern char	terminal[NAMELEN];
extern char	sys[2*NAMELEN];
extern char	password[NAMELEN];
extern char	username[NAMELEN];
extern char	cputype[NAMELEN];
extern char	*bootdisk;
extern char	*initflag;
extern void	(*pword)(Method*);
extern int	kflag;

/* libc equivalent */
extern int	plumb(char*, char*, int*, char*);
extern int	outin(char*, char*, int);
extern int	sendmsg(int, char*);
extern void	warning(char*);
extern void	fatal(char*);
extern int	readenv(char*, char*, int);
extern void	setenv(char*, char*);
extern void	srvcreate(char*, int);
extern int	dkauth(void);
extern int	dkconnect(void);
extern void	userpasswd(int);
extern void	getpasswd(char*, int);

/* methods */
extern void	config9600(Method*);
extern int	auth9600(Method*);
extern int	connect9600(Method*);
extern void	config192000(Method*);
extern int	auth192000(Method*);
extern int	connect192000(Method*);
extern void	confighs(Method*);
extern int	authhs(Method*);
extern int	connecths(Method*);
extern void	configincon(Method*);
extern int	authincon(Method*);
extern int	connectincon(Method*);
extern void	configil(Method*);
extern int	authil(Method*);
extern int	connectil(Method*);
extern void	configtcp(Method*);
extern int	authtcp(Method*);
extern int	connecttcp(Method*);
extern void	configcyc(Method*);
extern int	authcyc(Method*);
extern int	connectcyc(Method*);
extern void	configlocal(Method*);
extern int	authlocal(Method*);
extern int	connectlocal(Method*);
