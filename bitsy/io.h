/* replicated in part from the boot code, for now */

#define XTAL		3686400
#define	CORECLOCK	199066000
#define	MONITOR		0x00020000

#define	SYS_REGBASE	0x90000000

#define	SYSR_FIRQ	0x1000
#define	SYSR_IRQ	0x1400
#define	SIRQ_PCI	(1<<2)
#define	SIRQ_UART	(1<<8)
#define SYSR_UART_SR	0x3400	/* UART Status Register */
#define SYSR_UART_CR	0x3800	/* UART Control Register */
#define SYSR_UART_DR	0x3C00	/* UART Data Register */

#define	SYS_FIRQ	(0x90000000+SYSR_FIRQ)
#define	SYS_IRQ		(0x90000000+SYSR_IRQ)

#define	PCI_REGBASE	0x42000000
#define	PCI_IPXRESET	0x07C
#define	PCI_IRQSTATUS	0x180
#define	PCI_IRQRSTATUS	0x184
#define	PCI_IRQENABLE	0x188
#define	PCI_IRQENA_SET	0x188
#define	PCI_IRQENA_CLR	0x18C
#define	PCI_IRQSOFT	0x190
#define	PCI_FIRQSTATUS	0x280
#define	PCI_FIRQRSTATUS	0x284
#define	PCI_FIRQENABLE	0x288
#define	PCI_FIRQENA_SET	0x288
#define	PCI_FIRQENA_CLR	0x28C
#define	PCI_FIRQSOFT	0x290
#define	PCI_TIMERLOAD	0x300
#define	PCI_TIMERVALUE	0x304
#define	PCI_TIMERCTL	0x308
#define	PCI_TIMERCLEAR	0x30C

#define	TIMER_OFF	0x020

#define	TIMER1_LOAD	(PCI_REGBASE+PCI_TIMERLOAD)
#define	TIMER1_VALUE	(PCI_REGBASE+PCI_TIMERVALUE)
#define	TIMER1_CTL	(PCI_REGBASE+PCI_TIMERCTL)
#define	TIMER1_CLEAR	(PCI_REGBASE+PCI_TIMERCLEAR)

#define	TIMER_PERIODIC	(1<<6)
#define	TIMER_ENABLE	(1<<7)

#define	IRQ_STATUS	(PCI_REGBASE+PCI_IRQSTATUS)
#define	IRQ_ENABLE_SET	(PCI_REGBASE+PCI_IRQENA_SET)
#define	IRQ_ENABLE_CLR	(PCI_REGBASE+PCI_IRQENA_CLR)

#define	FIRQ_OFF	0x100

#define	IRQ_SOFT	1
#define	IRQ_TIMER1	4
#define	IRQ_TIMER2	5
#define	IRQ_TIMER3	6
#define	IRQ_TIMER4	7
#define	IRQ_DOOR	15
#define	IRQ_DMA1	16
#define	IRQ_DMA2	17
#define	IRQ_PCI		18
#define	IRQ_DMA1NB	20
#define	IRQ_DMA2NB	21
#define	IRQ_BIST	22
#define	IRQ_SERR	23
#define	IRQ_I2OIN	25
#define	IRQ_POWER	26
#define	IRQ_DTIMER	27
#define	IRQ_PARITY	28
#define	IRQ_MABORT	29
#define	IRQ_TABORT	30
#define	IRQ_DPARITY	31

#define PCI_CONF1_BASE	0x52000000
#define PCI_CONF0_BASE	0x53000000
#define PCI_IO_BASE	0x54000000
#define	PCI_IO_SIZE	0x00010000
#define PCI_MEM_BASE	0x60000000
#define PCI_MEM_SIZE	0x20000000

/* MMU. */

#define MMUCR_M_ENABLE	(1<<0)	/* MMU enable */
#define MMUCR_A_ENABLE	(1<<1)	/* Address alignment fault enable */
#define MMUCR_C_ENABLE	(1<<2)	/* (data) cache enable */
#define MMUCR_W_ENABLE	(1<<3)	/* write buffer enable */
#define MMUCR_PROG32	(1<<4)	/* PROG32 */
#define MMUCR_DATA32	(1<<5)	/* DATA32 */
#define MMUCR_L_ENABLE	(1<<6)	/* Late abort on earlier CPUs */
#define MMUCR_BIGEND	(1<<7)	/* Big-endian (=1), little-endian (=0) */
#define MMUCR_SYSTEM	(1<<8)	/* System bit, modifies MMU protections */
#define MMUCR_ROM	(1<<9)	/* ROM bit, modifies MMU protections */
#define MMUCR_F		(1<<10)	/* Should Be Zero */
#define MMUCR_Z_ENABLE	(1<<11)	/* Branch prediction enable on 810 */
#define MMUCR_I_ENABLE	(1<<12)	/* Instruction cache enable on SA110 */

#define	SDRAM_BASE	0xC0000000
#define	SDRAM_RESV	(16*1024*1024)
#define	SDRAM_SIZE	(32*1024*1024)

#define	PMEM_BASE	(SDRAM_BASE+SDRAM_RESV)
#define	PMEM_TOP	(SDRAM_BASE+SDRAM_SIZE)
#define	KTOP		(KZERO + (SDRAM_SIZE - SDRAM_RESV))

#define	SLOW_BASE	0x38400000
#define	SLEDOFFSET	0x00108000

#define	LEDADDR		(SLOW_BASE+SLEDOFFSET)

#define	SACADDR		0x08100000
