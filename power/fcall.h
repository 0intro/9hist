typedef	struct	Fcall	Fcall;

struct	Fcall
{
	char	type;
	short	fid;
	short	tag;
	union
	{
		struct
		{
			short	newfid;		/* T-Clone */
			short	oldtag;		/* T-Flush */
			Qid	qid;		/* R-Attach, R-Walk, R-Open, R-Create */
		};
		struct
		{
			char	uname[NAMELEN];	/* T-Attach */
			char	aname[NAMELEN];	/* T-Attach */
			char	auth[NAMELEN];	/* T-Attach */
		};
		struct
		{
			char	ename[ERRLEN];	/* R-Error */
		};
		struct
		{
			long	perm;		/* T-Create */ 
			char	name[NAMELEN];	/* T-Walk, T-Create */
			char	mode;		/* T-Create, T-Open */
		};
		struct
		{
			long	offset;		/* T-Read, T-Write */
			long	count;		/* T-Read, T-Write, R-Read */
			char	*data;		/* T-Write, R-Read */
		};
		struct
		{
			char	stat[DIRLEN];	/* T-Wstat, R-Stat */
		};
	};
};

#define	MAXFDATA	8192

enum
{
	Tnop =		50,
	Rnop,
	Tsession =	52,
	Rsession,
/*	Terror =	54,	illegal */
	Rerror =	55,
	Tflush =	56,
	Rflush,
	Tattach =	58,
	Rattach,
	Tclone =	60,
	Rclone,
	Twalk =		62,
	Rwalk,
	Topen =		64,
	Ropen,
	Tcreate =	66,
	Rcreate,
	Tread =		68,
	Rread,
	Twrite =	70,
	Rwrite,
	Tclunk =	72,
	Rclunk,
	Tremove =	74,
	Rremove,
	Tstat =		76,
	Rstat,
	Twstat =	78,
	Rwstat,
};

int	convM2S(char*, Fcall*, int);
int	convS2M(Fcall*, char*);

int	convM2D(char*, Dir*);
int	convD2M(Dir*, char*);

int	fcallconv(void *, int, int, int, int);
int	dirconv(void *, int, int, int, int);
