#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

extern Dev rootdevtab;
extern Dev consdevtab;
extern Dev envdevtab;
extern Dev pipedevtab;
extern Dev procdevtab;
extern Dev mntdevtab;
extern Dev srvdevtab;
extern Dev dupdevtab;
extern Dev rtcdevtab;
extern Dev ssldevtab;
extern Dev mntstatsdevtab;
extern Dev etherdevtab;
extern Dev ipdevtab;
extern Dev drawdevtab;
extern Dev mousedevtab;
extern Dev vgadevtab;
extern Dev scsidevtab;
extern Dev cddevtab;
extern Dev sddevtab;
extern Dev atadevtab;
extern Dev floppydevtab;
extern Dev audiodevtab;
extern Dev i82365devtab;
extern Dev lptdevtab;
extern Dev ns16552devtab;
extern Dev lmldevtab;
Dev* devtab[]={
	&rootdevtab,
	&consdevtab,
	&envdevtab,
	&pipedevtab,
	&procdevtab,
	&mntdevtab,
	&srvdevtab,
	&dupdevtab,
	&rtcdevtab,
	&ssldevtab,
	&mntstatsdevtab,
	&etherdevtab,
	&ipdevtab,
	&drawdevtab,
	&mousedevtab,
	&vgadevtab,
	&scsidevtab,
	&cddevtab,
	&sddevtab,
	&atadevtab,
	&floppydevtab,
	&audiodevtab,
	&i82365devtab,
	&lptdevtab,
	&ns16552devtab,
	&lmldevtab,
	nil,
};

extern void ether2000link(void);
extern void ether2114xlink(void);
extern void ether589link(void);
extern void ether79c970link(void);
extern void ether8003link(void);
extern void ether82557link(void);
extern void etherelnk3link(void);
extern void etherwavelanlink(void);
extern void ethermediumlink(void);
void links(void){
	ether2000link();
	ether2114xlink();
	ether589link();
	ether79c970link();
	ether8003link();
	ether82557link();
	etherelnk3link();
	etherwavelanlink();
	ethermediumlink();
}

extern PCArch archgeneric;
extern PCArch archmp;
PCArch* knownarch[] = {
	&archgeneric,
	&archmp,
	nil,
};

extern SCSIdev scsibuslogicdev;
SCSIdev* scsidev[] = {
	&scsibuslogicdev,
	nil,
};

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"
extern VGAdev vgaark2000pvdev;
extern VGAdev vgaclgd542xdev;
extern VGAdev vgact65545dev;
extern VGAdev vgamach64xxdev;
extern VGAdev vgamga2164wdev;
extern VGAdev vgas3dev;
VGAdev* vgadev[] = {
	&vgaark2000pvdev,
	&vgaclgd542xdev,
	&vgact65545dev,
	&vgamach64xxdev,
	&vgamga2164wdev,
	&vgas3dev,
	nil,
};

extern VGAcur vgaark2000pvcur;
extern VGAcur vgabt485cur;
extern VGAcur vgaclgd542xcur;
extern VGAcur vgact65545cur;
extern VGAcur vgamach64xxcur;
extern VGAcur vgamga2164wcur;
extern VGAcur vgargb524cur;
extern VGAcur vgas3cur;
extern VGAcur vgatvp3020cur;
extern VGAcur vgatvp3026cur;
VGAcur* vgacur[] = {
	&vgaark2000pvcur,
	&vgabt485cur,
	&vgaclgd542xcur,
	&vgact65545cur,
	&vgamach64xxcur,
	&vgamga2164wcur,
	&vgargb524cur,
	&vgas3cur,
	&vgatvp3020cur,
	&vgatvp3026cur,
	nil,
};

#include "../ip/ip.h"
extern void ilinit(Fs*);
extern void tcpinit(Fs*);
extern void udpinit(Fs*);
extern void rudpinit(Fs*);
extern void ipifcinit(Fs*);
extern void icmpinit(Fs*);
extern void greinit(Fs*);
extern void ipmuxinit(Fs*);
void (*ipprotoinit[])(Fs*) = {
	ilinit,
	tcpinit,
	udpinit,
	rudpinit,
	ipifcinit,
	icmpinit,
	greinit,
	ipmuxinit,
	nil,
};

	int cpuserver = 0;
char* conffile = "wavelan";
ulong kerndate = KERNDATE;
