enum{
	Enevermind,	/* never mind */
	Enofd,		/* no free file descriptors */
	Ebadfd,		/* fd out of range or not open */
	Ebadusefd,	/* inappropriate use of fd */
	Ebadarg,	/* bad arg in system call */
	Enonexist,	/* file does not exist */
	Efilename,	/* file name syntax */
	Ebadchar,	/* bad character in file name */
	Ebadsharp,	/* unknown device in # filename */
	Ebadexec,	/* a.out header invalid */
	Eioload,	/* i/o error in demand load */
	Eperm,		/* permission denied */
	Enotdir,	/* not a directory */
	Enochild,	/* no living children */
	Enoseg,		/* no free segments */
	Ebadmount,	/* inconsistent mount */
	Enomount,	/* mount table full */
	Enomntdev,	/* no free mount devices */
	Eshutdown,	/* mounted device shut down */
	Einuse,		/* device or object already in use */
	Eio,		/* i/o error */
	Eisdir,		/* file is a directory */
	Ebaddirread,	/* directory read not quantized */
	Esegaddr,	/* illegal segment addresses or size */
	Enoenv,		/* no free environment resources */
	Eprocdied,	/* process exited */
	Enocreate,	/* mounted directory forbids creation */
	Enotunion,	/* attempt to union with non-mounted directory */
	Emount,		/* inconsistent mount */
	Enosrv,		/* no free server slots */
	Enoqueue,	/* no free stream queues */
	Ebadld,		/* illegal line discipline */
	Enostream,	/* no free stream heads */
	Etoobig,	/* read or write too large */
	Etoosmall,	/* read or write too small */
	Ehungup,	/* write to hungup stream */
	Ebadnet,	/* illegal network address */
	Enoifc,		/* bad interface or no free interface slots */
	Enodev,		/* no free devices */
	Ebadctl,	/* bad process or stream control request */
	Enonote,	/* note overflow */
	Eintr,		/* interrupted */
	Edestbusy,	/* datakit destination busy */
	Enetbusy,	/* datakit controller busy */
	Edestotl,	/* datakit destination out to lunch */
	Enetotl,	/* datakit controller out to lunch */
	Erejected,	/* datakit destination rejected call */
	Ebadblt,	/* bad bitblt or bitmap request */
	Enobitmap,	/* out of bitmap descriptors */
	Enobitstore,	/* out of bitmap storage */
	Ebadbitmap,	/* unallocated bitmap */
	Ebadfont,	/* unallocated font */
	Eshortmsg,	/* short message */
	Ebadmsg,	/* format error or mismatch in message */
	Ebadcnt,	/* read count greater than requested */
	Enofont,	/* out of font descriptors */
	Enovmem,	/* virtual memory allocation failed */
	Enoasync,	/* out of async stream modules */
	Enopipe,	/* out of pipes */
	Egreg,		/* ken hasn't implemented datakit */
};
