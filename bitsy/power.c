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

int powerup;

enum {
	pmcr_sf		= 0,

	pcfr_opde	= 0,
	pcfr_fp		= 1,
	pcfr_fs		= 2,
	pcfr_fo		= 3,
};

static void
onoffintr(Ureg*, void *x)
{
	/* On/off button interrupt */
	int i;

	if (powerup) {
		/* Power back up */
		powerup = 0;
		exit(0);
	} else {
		/* Power down */

		/* debounce, 50 ms*/
		for (i = 0; i < 50; i++) {
			delay(1);
			if ((gpioregs->level & GPIO_PWR_ON_i) == 0)
				return;	/* bounced */
		}
		powerup = 1;
		irpower(0);
		audiopower(0);
		lcdpower(0);
		rs232power(0);
		sa1100_power_off();
	}
}

static void
sa1100_power_off(void)
{
	delay(100);
	splhi();
	/* disable internal oscillator, float CS lines */
	powerregs->pcfr = 1<<pcfr_opde | 1<<pcfr_fp | 1<<pcfr_fs;
	/* set lowest clock */
	powerregs->ppcr = 0;
	/* set all GPIOs to input mode */
	gpioregs->direction = 0;
	/* enter sleep mode */
	powerregs->pmcr = 1<<pmcr_sf;
}
