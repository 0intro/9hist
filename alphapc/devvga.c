/*
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

enum {
	Qdir,
	Qvgaiob,
	Qvgaiow,
	Qvgaiol,
	Qvgactl,
};

static Dirtab vgadir[] = {
	"vgaiob",	{ Qvgaiob, 0 },		0,	0660,
	"vgaiow",	{ Qvgaiow, 0 },		0,	0660,
	"vgaiol",	{ Qvgaiol, 0 },		0,	0660,
	"vgactl",	{ Qvgactl, 0 },		0,	0660,
};

static void
vgareset(void)
{
	conf.monitor = 1;
}

static Chan*
vgaattach(char* spec)
{
	if(*spec && strcmp(spec, "0"))
		error(Eio);
	return devattach('v', spec);
}

int
vgawalk(Chan* c, char* name)
{
	return devwalk(c, name, vgadir, nelem(vgadir), devgen);
}

static void
vgastat(Chan* c, char* dp)
{
	devstat(c, dp, vgadir, nelem(vgadir), devgen);
}

static Chan*
vgaopen(Chan* c, int omode)
{
	return devopen(c, omode, vgadir, nelem(vgadir), devgen);
}

static void
vgaclose(Chan*)
{
}

static long
vgaread(Chan* c, void* a, long n, vlong off)
{
	int len, port;
	char *p, *s;
	ushort *sp;
	ulong *lp;
	VGAscr *scr;
	ulong offset = off;

	switch(c->qid.path & ~CHDIR){

	case Qdir:
		return devdirread(c, a, n, vgadir, nelem(vgadir), devgen);

	case Qvgactl:
		scr = &vgascreen[0];

		p = malloc(READSTR);
		if(waserror()){
			free(p);
			nexterror();
		}
		if(scr->dev)
			s = scr->dev->name;
		else
			s = "cga";
		len = snprint(p, READSTR, "type: %s\n", s);
		if(scr->gscreen)
			len += snprint(p+len, READSTR-len, "size: %dx%dx%d\n",
				scr->gscreen->r.max.x, scr->gscreen->r.max.y,
				1<<scr->gscreen->ldepth);
		if(scr->cur)
			s = scr->cur->name;
		else
			s = "off";
		len += snprint(p+len, READSTR-len, "hwgc: %s\n", s);
		if(scr->pciaddr)
			snprint(p+len, READSTR-len, "addr: 0x%lux\n",
				scr->pciaddr);
		else
			snprint(p+len, READSTR-len, "addr: 0x%lux\n",
				scr->aperture);

		n = readstr(offset, a, n, p);
		poperror();
		free(p);

		return n;

	case Qvgaiob:
		port = offset;
		for(p = a; port < offset+n; port++)
			*p++ = inb(port);
		return n;

	case Qvgaiow:
		if((n & 0x01) || (offset & 0x01))
			error(Ebadarg);
		n /= 2;
		sp = a;
		for(port = offset; port < offset+n; port += 2)
			*sp++ = ins(port);
		return n*2;

	case Qvgaiol:
		if((n & 0x03) || (offset & 0x03))
			error(Ebadarg);
		n /= 4;
		lp = a;
		for(port = offset; port < offset+n; port += 4)
			*lp++ = inl(port);
		return n*4;

	default:
		error(Egreg);
		break;
	}

	return 0;
}

static void
vgactl(char* a)
{
	int align, i, n, size, x, y, z;
	char *field[4], *p;
	VGAscr *scr;
	extern VGAdev *vgadev[];
	extern VGAcur *vgacur[];

	n = getfields(a, field, 4, 1, " ");
	if(n < 2)
		error(Ebadarg);

	scr = &vgascreen[0];
	if(strcmp(field[0], "hwgc") == 0){
		if(n < 2)
			error(Ebadarg);
		if(strcmp(field[1], "off") == 0){
			lock(&cursor);
			if(scr->cur){
				if(scr->cur->disable)
					scr->cur->disable(scr);
				scr->cur = nil;
			}
			unlock(&cursor);
			return;
		}

		for(i = 0; vgacur[i]; i++){
			if(strcmp(field[1], vgacur[i]->name))
				continue;
			lock(&cursor);
			if(scr->cur && scr->cur->disable)
				scr->cur->disable(scr);
			scr->cur = vgacur[i];
			if(scr->cur->enable)
				scr->cur->enable(scr);
			unlock(&cursor);
			return;
		}
	}
	else if(strcmp(field[0], "type") == 0){
		if(n < 2)
			error(Ebadarg);

		for(i = 0; vgadev[i]; i++){
			if(strcmp(field[1], vgadev[i]->name))
				continue;
			if(scr->dev && scr->dev->disable)
				scr->dev->disable(scr);
			scr->dev = vgadev[i];
			if(scr->dev->enable)
				scr->dev->enable(scr);
			return;
		}
	}
	else if(strcmp(field[0], "size") == 0){
		if(n < 2)
			error(Ebadarg);
		x = strtoul(field[1], &p, 0);
		if(x == 0 || x > 2048)
			error(Ebadarg);
		if(*p)
			p++;

		y = strtoul(p, &p, 0);
		if(y == 0 || y > 2048)
			error(Ebadarg);
		if(*p)
			p++;

		switch(strtoul(p, &p, 0)){
		case 8:
			z = 3;
			break;

		default:
			z = 0;
			error(Ebadarg);
		}

		cursoroff(1);
		if(screensize(x, y, z))
			error(Egreg);
		vgascreenwin(scr);
		cursoron(1);
		return;
	}
	else if(strcmp(field[0], "linear") == 0){
		if(n < 2)
			error(Ebadarg);

		size = strtoul(field[1], 0, 0);
		if(n < 3)
			align = 0;
		else
			align = strtoul(field[2], 0, 0);
		if(screenaperture(size, align))
			error("not enough free address space");
		return;
	}

	error(Ebadarg);
}

static long
vgawrite(Chan* c, void* a, long n, vlong off)
{
	int port;
	char *p;
	ushort *sp;
	ulong *lp;
	ulong offset = off;

	switch(c->qid.path & ~CHDIR){

	case Qdir:
		error(Eperm);

	case Qvgactl:
		if(offset || n >= READSTR)
			error(Ebadarg);
		p = malloc(READSTR);
		if(waserror()){
			free(p);
			nexterror();
		}
		memmove(p, a, n);
		p[n] = 0;
		vgactl(p);
		poperror();
		free(p);
		return n;

	case Qvgaiob:
		p = a;
		for(port = offset; port < offset+n; port++)
			outb(port, *p++);
		return n;

	case Qvgaiow:
		if((n & 01) || (offset & 01))
			error(Ebadarg);
		n /= 2;
		sp = a;
		for(port = offset; port < offset+n; port += 2)
			outs(port, *sp++);
		return n*2;

	case Qvgaiol:
		if((n & 0x03) || (offset & 0x03))
			error(Ebadarg);
		n /= 4;
		lp = a;
		for(port = offset; port < offset+n; port += 4)
			outl(port, *lp++);
		return n*4;

	default:
		error(Egreg);
		break;
	}

	return 0;
}

Dev vgadevtab = {
	'v',
	"vga",

	vgareset,
	devinit,
	vgaattach,
	devclone,
	vgawalk,
	vgastat,
	vgaopen,
	devcreate,
	vgaclose,
	vgaread,
	devbread,
	vgawrite,
	devbwrite,
	devremove,
	devwstat,
};
