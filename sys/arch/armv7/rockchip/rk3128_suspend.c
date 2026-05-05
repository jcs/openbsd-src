/*	$OpenBSD$	*/
/*
 * Copyright (c) 2026 joshua stein <jcs@jcs.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>

#include <machine/bus.h>
#include <machine/cpu.h>

#include <arm/cpufunc.h>
#include <armv7/armv7/armv7var.h>

extern int	ampintc_intr_pending(void);
extern int	ampintc_get_hpir(void);
extern void	ampintc_set_wakeup_pmr(void);
extern void	ampintc_restore_pmr(void);

/*
 * RK3128 suspend via deep idle: PLLs off, clocks gated, CPU in WFI and wakes
 * on any GIC interrupt.
 */

#define RK3128_CRU_BASE		0x20000000
#define RK3128_CRU_SIZE		0x0200

/* CRU PLL registers: each PLL has 3 CON registers */
#define CRU_PLL_CON(base, i)	((base) + (i) * 4)
#define CRU_APLL_CON(i)	CRU_PLL_CON(0x0000, i)
#define CRU_CPLL_CON(i)	CRU_PLL_CON(0x0020, i)
#define CRU_GPLL_CON(i)	CRU_PLL_CON(0x0030, i)
#define CRU_MODE_CON		0x0040
#define CRU_CLKSEL_CON(i)	(0x0044 + (i) * 4)
#define CRU_PLL_LOCK		(1 << 10)	/* in PLL CON1 */
#define CRU_PLL_POWERDOWN	0x20002000	/* write-mask: set bit 13 */
#define CRU_PLL_POWERON		0x20000000	/* write-mask: clear bit 13 */

/* MODE_CON: 2-bit fields per PLL, write-mask in upper 16 bits */
#define CRU_MODE_APLL_SLOW	0x00030000	/* bits 1:0 = 0 (slow) */
#define CRU_MODE_CPLL_SLOW	0x03000000	/* bits 9:8 = 0 (slow) */
#define CRU_MODE_GPLL_SLOW	0x30000000	/* bits 13:12 = 0 (slow) */

static bus_space_tag_t		rksuspend_iot;
static bus_space_handle_t	rksuspend_cru_ioh;
static int			rksuspend_inited;

static uint32_t			cru_save_mode;
static uint32_t			cru_save_clksel[5];

#define CRU_READ(off)		bus_space_read_4(rksuspend_iot, \
				    rksuspend_cru_ioh, (off))
#define CRU_WRITE(off, val)	bus_space_write_4(rksuspend_iot, \
				    rksuspend_cru_ioh, (off), (val))

static int
rksuspend_init(void)
{
	if (rksuspend_inited)
		return 0;

	rksuspend_iot = &armv7_bs_tag;

	if (bus_space_map(rksuspend_iot, RK3128_CRU_BASE, RK3128_CRU_SIZE, 0,
	    &rksuspend_cru_ioh)) {
		printf("rk3128_suspend: can't map CRU\n");
		return ENOMEM;
	}

	rksuspend_inited = 1;
	return 0;
}

void
rk3128_suspend(void)
{
	int i;

	if (rksuspend_init() != 0)
		return;

	/*
	 * Power down PLLs and switch to 24MHz oscillator, except for DPLL to
	 * keep DDR active.
	 */
	cru_save_mode = CRU_READ(CRU_MODE_CON);
	cru_save_clksel[0] = CRU_READ(CRU_CLKSEL_CON(0));
	cru_save_clksel[1] = CRU_READ(CRU_CLKSEL_CON(1));
	cru_save_clksel[2] = CRU_READ(CRU_CLKSEL_CON(10));
	cru_save_clksel[3] = CRU_READ(CRU_CLKSEL_CON(24));
	cru_save_clksel[4] = CRU_READ(CRU_CLKSEL_CON(29));

	/* switch CPLL and GPLL to slow mode */
	CRU_WRITE(CRU_MODE_CON, CRU_MODE_CPLL_SLOW);
	CRU_WRITE(CRU_MODE_CON, CRU_MODE_GPLL_SLOW);

	/* set safe dividers for peripherals running on 24MHz */
	CRU_WRITE(CRU_CLKSEL_CON(24), 0x00030003);	/* crypto div=4 */
	CRU_WRITE(CRU_CLKSEL_CON(10), 0x331f0000);	/* peri aclk/hclk/pclk */
	CRU_WRITE(CRU_CLKSEL_CON(29), 0x1f000000);	/* pmu div=1 */

	/* power down CPLL and GPLL */
	CRU_WRITE(CRU_CPLL_CON(1), CRU_PLL_POWERDOWN);
	CRU_WRITE(CRU_GPLL_CON(1), CRU_PLL_POWERDOWN);

	/* switch APLL to slow mode, set safe core dividers */
	CRU_WRITE(CRU_MODE_CON, CRU_MODE_APLL_SLOW);
	CRU_WRITE(CRU_CLKSEL_CON(0), 0x001f0000);	/* core div=1 */
	CRU_WRITE(CRU_CLKSEL_CON(1), 0x00070003);	/* pclk_dbg div=4 */

	/* power down APLL */
	CRU_WRITE(CRU_APLL_CON(1), CRU_PLL_POWERDOWN);

	cpu_idcache_wbinv_all();

	/*
	 * Clear stale pending GIC interrupts and raise PMR so nIRQ can wake
	 * the CPU from WFI.  Must be last before WFI because any spl or printf
	 * call resets PMR to 0.
	 */
	ampintc_set_wakeup_pmr();

	do {
		__asm volatile("dsb; wfi");
	} while (!ampintc_intr_pending());

	/* XXX */
	printf("%s: woke from irq %d\n", __func__, ampintc_get_hpir());

	ampintc_restore_pmr();

	/* --- resume path (running on 24MHz oscillator) --- */

	/* restore APLL */
	CRU_WRITE(CRU_APLL_CON(1), CRU_PLL_POWERON);
	delay(200);
	for (i = 10000; i > 0; i--) {
		if (CRU_READ(CRU_APLL_CON(1)) & CRU_PLL_LOCK)
			break;
		delay(1);
	}
	if (i == 0)
		printf("%s: APLL lock timeout\n", __func__);
	else {
		CRU_WRITE(CRU_CLKSEL_CON(0),
		    0x001f0000 | (cru_save_clksel[0] & 0x001f));
		CRU_WRITE(CRU_CLKSEL_CON(1),
		    0x00070000 | (cru_save_clksel[1] & 0x0007));
		CRU_WRITE(CRU_MODE_CON,
		    0x00030000 | (cru_save_mode & 0x0003));
	}

	/* restore GPLL */
	CRU_WRITE(CRU_GPLL_CON(1), CRU_PLL_POWERON);
	delay(200);
	for (i = 10000; i > 0; i--) {
		if (CRU_READ(CRU_GPLL_CON(1)) & CRU_PLL_LOCK)
			break;
		delay(1);
	}
	if (i == 0)
		printf("%s: GPLL lock timeout\n", __func__);
	else {
		CRU_WRITE(CRU_CLKSEL_CON(10),
		    0x331f0000 | (cru_save_clksel[2] & 0x331f));
		CRU_WRITE(CRU_CLKSEL_CON(24),
		    0x00030000 | (cru_save_clksel[3] & 0x0003));
		CRU_WRITE(CRU_CLKSEL_CON(29),
		    0x1f000000 | (cru_save_clksel[4] & 0x1f00));
		CRU_WRITE(CRU_MODE_CON,
		    0x30000000 | (cru_save_mode & 0x3000));
	}

	/* restore CPLL */
	CRU_WRITE(CRU_CPLL_CON(1), CRU_PLL_POWERON);
	delay(200);
	for (i = 10000; i > 0; i--) {
		if (CRU_READ(CRU_CPLL_CON(1)) & CRU_PLL_LOCK)
			break;
		delay(1);
	}
	if (i == 0)
		printf("%s: CPLL lock timeout\n", __func__);
	else
		CRU_WRITE(CRU_MODE_CON,
		    0x03000000 | (cru_save_mode & 0x0300));
}
