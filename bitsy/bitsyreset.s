#include "mem.h"

reset:
	mov	$INTREGS+4, r0		// turn off interrupts
	mov	$0, (r0)

	mov	$POWERREGS+14, r0	// set clock speed to 191.7MHz
	mov	$9, (r0)
