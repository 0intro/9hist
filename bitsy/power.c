#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"init.h"
#include	"pool.h"

/* Power management for the bitsy */

static void
powerintr(Ureg*, void *x)
{
	/* Power button interrupt */
	int i;

	/* debounce, 50 ms*/
	for (i = 0; i < 50; i++) {
		delay(1);
		if ((gpioregs->level & GPIO_PWR_ON_i) == 0)
			return;	/* bounced */
	}
	rs232power(0);
}
