typedef	struct	Fcall	Fcall;

struct	Fcall
{
	char	type;
	short	fid;
	short	err;
	union
	{
		struct
		{
			short	uid;		/* T-Userstr */
			short	newfid;		/* T-Clone */
			char	lang;		/* T-Session */
			ulong	qid;		/* R-Attach, R-Walk, R-Open, R-Create */
		};
		struct
		{
			char	uname[28];	/* T-Attach, R-Userstr */
			char	aname[28];	/* T-Attach */
		};
		struct
		{
			char	ename[64];	/* R-Errstr */
		};
		struct
		{
			long	perm;		/* T-Create */ 
			char	name[28];	/* T-Walk, T-Create, T-Remove */
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
	Tnop =		0,
	Rnop,
	Tsession =	2,
	Rsession,
	Tattach =	10,
	Rattach,
	Tclone =	12,
	Rclone,
	Twalk =		14,
	Rwalk,
	Topen =		16,
	Ropen,
	Tcreate =	18,
	Rcreate,
	Toread =	20,
	Roread,
	Towrite =	22,
	Rowrite,
	Tclunk =	24,
	Rclunk,
	Tremove =	26,
	Rremove,
	Tstat =		28,
	Rstat,
	Twstat =	30,
	Rwstat,
	Terrstr =	32,
	Rerrstr,
	Tuserstr =	34,
	Ruserstr,
	Tread =		36,
	Rread,
	Twrite =	38,
	Rwrite,
};

int	convM2S(char*, Fcall*, int);
int	convS2M(Fcall*, char*);

int	convM2D(char*, Dir*);
int	convD2M(Dir*, char*);
