#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"kernel.h"
#include	"ip.h"

static	short	endian	= 1;
static	byte*	aendian	= (byte*)&endian;
#define	LITTLE	*aendian

ushort
ptclbsum(byte *addr, int len)
{
	ulong losum, hisum, mdsum, x;
	ulong t1, t2;

	losum = 0;
	hisum = 0;
	mdsum = 0;

	x = 0;
	if((ulong)addr & 1) {
		if(len) {
			hisum += addr[0];
			len--;
			addr++;
		}
		x = 1;
	}
	while(len >= 16) {
		t1 = *(ushort*)(addr+0);
		t2 = *(ushort*)(addr+2);	mdsum += t1;
		t1 = *(ushort*)(addr+4);	mdsum += t2;
		t2 = *(ushort*)(addr+6);	mdsum += t1;
		t1 = *(ushort*)(addr+8);	mdsum += t2;
		t2 = *(ushort*)(addr+10);	mdsum += t1;
		t1 = *(ushort*)(addr+12);	mdsum += t2;
		t2 = *(ushort*)(addr+14);	mdsum += t1;
		mdsum += t2;
		len -= 16;
		addr += 16;
	}
	while(len >= 2) {
		mdsum += *(ushort*)addr;
		len -= 2;
		addr += 2;
	}
	if(x) {
		if(len)
			losum += addr[0];
		if(LITTLE)
			losum += mdsum;
		else
			hisum += mdsum;
	} else {
		if(len)
			hisum += addr[0];
		if(LITTLE)
			hisum += mdsum;
		else
			losum += mdsum;
	}

	losum += hisum >> 8;
	losum += (hisum & 0xff) << 8;
	while(hisum = losum>>16)
		losum = hisum + (losum & 0xffff);

	return losum & 0xffff;
}

ushort
ptclcsum(Block *bp, int offset, int len)
{
	byte *addr;
	ulong losum, hisum;
	ushort csum;
	int odd, blocklen, x;

	/* Correct to front of data area */
	while(bp != nil && offset && offset >= BLEN(bp)) {
		offset -= BLEN(bp);
		bp = bp->next;
	}
	if(bp == nil)
		return 0;

	addr = bp->rp + offset;
	blocklen = BLEN(bp) - offset;

	if(bp->next == nil) {
		if(blocklen < len)
			len = blocklen;
		return ~ptclbsum(addr, len) & 0xffff;
	}

	losum = 0;
	hisum = 0;

	odd = 0;
	while(len) {
		x = blocklen;
		if(len < x)
			x = len;

		csum = ptclbsum(addr, x);
		if(odd)
			hisum += csum;
		else
			losum += csum;
		odd = (odd+x) & 1;
		len -= x;

		bp = bp->next;
		if(bp == nil)
			break;
		blocklen = BLEN(bp);
		addr = bp->rp;
	}

	losum += hisum>>8;
	losum += (hisum&0xff)<<8;
	while((csum = losum>>16) != 0)
		losum = csum + (losum & 0xffff);

	return ~losum & 0xffff;
}

Ipaddr
defmask(Ipaddr ip)
{
	switch(ip>>30){
	default:
		return 0xff000000;
	case 2:
		return 0xffff0000;
	case 3:
		return 0xffffff00;
	}
}

int
myetheraddr(uchar *to, char *dev)
{
	int n, fd;
	char buf[256], *ptr;

	/* Make one exist */
	if(*dev == '/' || *dev == '#')
		sprint(buf, "%s/clone", dev);
	else
		sprint(buf, "/net/%s/clone", dev);

	fd = kopen(buf, ORDWR);
	if(fd >= 0)
		kclose(fd);

	if(*dev == '/' || *dev == '#')
		sprint(buf, "%s/0/stats", dev);
	else
		sprint(buf, "/net/%s/0/stats", dev);
	fd = kopen(buf, OREAD);
	if(fd < 0)
		return -1;

	n = kread(fd, buf, sizeof(buf)-1);
	kclose(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;

	ptr = strstr(buf, "addr: ");
	if(!ptr)
		return -1;
	ptr += 6;

	parseether(to, ptr);
	return 0;
}

int
eipconv(va_list *arg, Fconv *f)
{
	char buf[64];
	static char *efmt = "%.2lux%.2lux%.2lux%.2lux%.2lux%.2lux";
	static char *ifmt = "%d.%d.%d.%d";
	uchar *p, ip[4];

	switch(f->chr) {
	case 'E':		/* Ethernet address */
		p = va_arg(*arg, uchar*);
		sprint(buf, efmt, p[0], p[1], p[2], p[3], p[4], p[5]);
		break;
	case 'I':		/* Ip address */
		p = va_arg(*arg, uchar*);
		sprint(buf, ifmt, p[0], p[1], p[2], p[3]);
		break;
	case 'i':
		hnputl(ip, va_arg(*arg, ulong));
		sprint(buf, ifmt, ip[0], ip[1], ip[2], ip[3]);
		break;
	default:
		strcpy(buf, "(eipconv)");
	}
	strconv(buf, f);
	return sizeof(uchar*);
}

#define CLASS(p) ((*(uchar*)(p))>>6)

ulong
parseip(char *to, char *from)
{
	int i;
	char *p;

	p = from;
	memset(to, 0, 4);
	for(i = 0; i < 4 && *p; i++){
		to[i] = strtoul(p, &p, 0);
		if(*p == '.')
			p++;
	}
	switch(CLASS(to)){
	case 0:	/* class A - 1 byte net */
	case 1:
		if(i == 3){
			to[3] = to[2];
			to[2] = to[1];
			to[1] = 0;
		} else if (i == 2){
			to[3] = to[1];
			to[1] = 0;
		}
		break;
	case 2:	/* class B - 2 byte net */
		if(i == 3){
			to[3] = to[2];
			to[2] = 0;
		}
		break;
	}
	return nhgetl((uchar*)to);
}
