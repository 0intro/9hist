#include	<u.h>
#include	"lib.h"
#include	"fcall.h"

#define	CHAR(x)		*p++ = f->x
#define	SHORT(x)	p[0] = f->x; p[1] = f->x>>8; p += 2
#define	LONG(x)		p[0] = f->x; p[1] = f->x>>8; p[2] = f->x>>16; p[3] = f->x>>24; p += 4
#define	VLONG(x)	p[0] = f->x; p[1] = f->x>>8; p[2] = f->x>>16; p[3] = f->x>>24;\
			p[4] = 0; p[5] = 0; p[6] = 0; p[7] = 0; p += 8
#define	STRING(x,n)	memcpy(p, f->x, n); p += n

int
convS2M(Fcall *f, char *ap)
{
	uchar *p;

	p = (uchar*)ap;
	CHAR(type);
	switch(f->type)
	{
	default:
		print("bad type: %d\n", f->type);
		return 0;

	case Tnop:
	case Rnop:
		break;

	case Tsession:
		CHAR(lang);
		break;

	case Tattach:
		SHORT(fid);
		STRING(uname, sizeof(f->uname));
		STRING(aname, sizeof(f->aname));
		break;

	case Tclone:
		SHORT(fid);
		SHORT(newfid);
		break;

	case Twalk:
	case Tremove:
		SHORT(fid);
		STRING(name, sizeof(f->name));
		break;

	case Topen:
		SHORT(fid);
		CHAR(mode);
		break;

	case Tcreate:
		SHORT(fid);
		STRING(name, sizeof(f->name));
		LONG(perm);
		CHAR(mode);
		break;

	case Toread:
	case Tread:
		SHORT(fid);
		VLONG(offset);
		SHORT(count);
		break;

	case Towrite:
		SHORT(fid);
		VLONG(offset);
		SHORT(count);
		STRING(data, f->count);
		break;

	case Twrite:
		SHORT(fid);
		VLONG(offset);
		SHORT(count);
		p++;
		STRING(data, f->count);
		break;

	case Tclunk:
	case Tstat:
		SHORT(fid);
		break;

	case Twstat:
		SHORT(fid);
		STRING(stat, sizeof(f->stat));
		break;

	case Terrstr:
		SHORT(fid);
		SHORT(err);
		break;

	case Tuserstr:
		SHORT(fid);
		SHORT(uid);
		break;
/*
 */
	case Rsession:
		SHORT(err);
		break;

	case Rclone:
	case Rclunk:
	case Rremove:
	case Rwstat:
		SHORT(fid);
		SHORT(err);
		break;

	case Rwalk:
	case Rattach:
	case Ropen:
	case Rcreate:
		SHORT(fid);
		SHORT(err);
		LONG(qid);
		break;

	case Roread:
		SHORT(fid);
		SHORT(err);
		SHORT(count);
		STRING(data, f->count);
		break;

	case Rread:
		SHORT(fid);
		SHORT(err);
		SHORT(count);
		p++;
		STRING(data, f->count);
		break;

	case Rowrite:
	case Rwrite:
		SHORT(fid);
		SHORT(err);
		SHORT(count);
		break;

	case Rstat:
		SHORT(fid);
		SHORT(err);
		STRING(stat, sizeof(f->stat));
		break;

	case Rerrstr:
		SHORT(fid);
		SHORT(err);
		STRING(ename, sizeof(f->ename));
		break;

	case Ruserstr:
		SHORT(fid);
		SHORT(err);
		STRING(uname, sizeof(f->uname));
		break;
	}
	return p - (uchar*)ap;
}

int
convD2M(Dir *f, char *ap)
{
	uchar *p;

	p = (uchar*)ap;
	STRING(name, sizeof(f->name));
	LONG(qid);
	p += 4;
	LONG(mode);
	LONG(atime);
	LONG(mtime);
	VLONG(length);
	SHORT(uid);
	SHORT(gid);
	SHORT(type);
	SHORT(dev);
	return p - (uchar*)ap;
}

#undef	CHAR
#undef	SHORT
#undef	LONG
#undef	VLONG
#undef	STRING

#define	CHAR(x)		f->x = *p++
#define	SHORT(x)	f->x = (p[0] | (p[1]<<8)); p += 2
#define	LONG(x)		f->x = (p[0] | (p[1]<<8) |\
			(p[2]<<16) | (p[3]<<24)); p += 4
#define	VLONG(x)	f->x = (p[0] | (p[1]<<8) |\
			(p[2]<<16) | (p[3]<<24)); p += 8
#define	STRING(x,n)	memcpy(f->x, p, n); p += n

int
convM2S(char *ap, Fcall *f, int n)
{
	uchar *p;

	p = (uchar*)ap;
	CHAR(type);
	switch(f->type)
	{
	default:
		print("bad type: %d\n", f->type);
		return 0;

	case Tnop:
	case Rnop:
		break;

	case Tsession:
		CHAR(lang);
		break;

	case Tattach:
		SHORT(fid);
		STRING(uname, sizeof(f->uname));
		STRING(aname, sizeof(f->aname));
		break;

	case Tclone:
		SHORT(fid);
		SHORT(newfid);
		break;

	case Twalk:
	case Tremove:
		SHORT(fid);
		STRING(name, sizeof(f->name));
		break;

	case Topen:
		SHORT(fid);
		CHAR(mode);
		break;

	case Tcreate:
		SHORT(fid);
		STRING(name, sizeof(f->name));
		LONG(perm);
		CHAR(mode);
		break;

	case Toread:
	case Tread:
		SHORT(fid);
		VLONG(offset);
		SHORT(count);
		break;

	case Towrite:
		SHORT(fid);
		VLONG(offset);
		SHORT(count);
		f->data = (char*)p; p += f->count;
		break;

	case Twrite:
		SHORT(fid);
		VLONG(offset);
		SHORT(count);
		p++;
		f->data = (char*)p; p += f->count;
		break;

	case Tclunk:
	case Tstat:
		SHORT(fid);
		break;

	case Twstat:
		SHORT(fid);
		STRING(stat, sizeof(f->stat));
		break;

	case Terrstr:
		SHORT(fid);
		SHORT(err);
		break;

	case Tuserstr:
		SHORT(fid);
		SHORT(uid);
		break;

	case Rsession:
		SHORT(err);
		break;

	case Rclone:
	case Rclunk:
	case Rremove:
	case Rwstat:
		SHORT(fid);
		SHORT(err);
		break;

	case Rwalk:
	case Rattach:
	case Ropen:
	case Rcreate:
		SHORT(fid);
		SHORT(err);
		LONG(qid);
		break;

	case Roread:
		SHORT(fid);
		SHORT(err);
		SHORT(count);
		f->data = (char*)p; p += f->count;
		break;

	case Rread:
		SHORT(fid);
		SHORT(err);
		SHORT(count);
		p++;
		f->data = (char*)p; p += f->count;
		break;

	case Rowrite:
	case Rwrite:
		SHORT(fid);
		SHORT(err);
		SHORT(count);
		break;

	case Rstat:
		SHORT(fid);
		SHORT(err);
		STRING(stat, sizeof(f->stat));
		break;

	case Rerrstr:
		SHORT(fid);
		SHORT(err);
		STRING(ename, sizeof(f->ename));
		break;

	case Ruserstr:
		SHORT(fid);
		SHORT(err);
		STRING(uname, sizeof(f->uname));
		break;
	}
	if((uchar*)ap+n == p)
		return n;
	return 0;
}

int
convM2D(char *ap, Dir *f)
{
	uchar *p;

	p = (uchar*)ap;
	STRING(name, sizeof(f->name));
	LONG(qid);
	p += 4;
	LONG(mode);
	LONG(atime);
	LONG(mtime);
	VLONG(length);
	SHORT(uid);
	SHORT(gid);
	SHORT(type);
	SHORT(dev);
	return p - (uchar*)ap;
}
