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
#include <armv7/armv7/armv7var.h>
#include <armv7/armv7/armv7_machdep.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_clock.h>
#include <dev/ofw/fdt.h>

extern void rk3128_suspend(void);

#ifdef MULTIPROCESSOR
#define SRAM_ENTRY_PA		0x0008
#define SRAM_DOORBELL		0x0004
#define SRAM_DOORBELL_MAGIC	0xdeadbeaf

void	*rk3128_smp_sram(void *);
int	 rk3128_smp_spinup(struct cpu_info *, paddr_t);
#endif

struct armv7_platform rk3128_platform = {
#ifdef SUSPEND
	.cpu_suspend = rk3128_suspend,
#endif
#ifdef MULTIPROCESSOR
	.smp_spinup = rk3128_smp_spinup,
#endif
};

#ifdef MULTIPROCESSOR
void *
rk3128_smp_sram(void *node)
{
	void *child, *sram;

	for (child = fdt_child_node(node); child != NULL;
	    child = fdt_next_node(child)) {
		if (fdt_is_compatible(child, "rockchip,rk3066-smp-sram"))
			return child;
		sram = rk3128_smp_sram(child);
		if (sram != NULL)
			return sram;
	}

	return NULL;
}

/*
 * out of reset, secondary cores run the bootrom, which parks them in
 * wfe polling a mailbox at the start of sram: a magic word at
 * SRAM_DOORBELL and the address to jump to at SRAM_ENTRY_PA.
 */
int
rk3128_smp_spinup(struct cpu_info *ci, paddr_t entry)
{
	bus_space_handle_t ioh;
	struct fdt_reg reg;
	void *node;

	node = rk3128_smp_sram(fdt_next_node(NULL));
	if (node == NULL)
		return 0;

	if (fdt_get_reg(node, 0, &reg) != 0 || reg.size < 12)
		return 0;

	if (bus_space_map(&armv7_bs_tag, reg.addr, reg.size, 0, &ioh) != 0)
		return 0;

	/* clear stale magic so a released core parks in the bootrom */
	bus_space_write_4(&armv7_bs_tag, ioh, SRAM_DOORBELL, 0);

	/*
	 * rk3036/rk312x have no pmu power domain for the cores; the
	 * core is powered up by cycling its cru soft-reset.
	 */
	reset_assert_idx(ci->ci_node, 0);
	delay(1000);
	reset_deassert_idx(ci->ci_node, 0);
	delay(1000);

	/* Set wake vector */
	bus_space_write_4(&armv7_bs_tag, ioh, SRAM_ENTRY_PA, entry);
	/* Notify boot rom that we are ready to start */
	bus_space_write_4(&armv7_bs_tag, ioh, SRAM_DOORBELL,
	    SRAM_DOORBELL_MAGIC);
	__asm volatile("dsb sy; sev" ::: "memory");

	bus_space_unmap(&armv7_bs_tag, ioh, reg.size);

	return 1;
}
#endif

struct armv7_platform *
rockchip_platform_match(void)
{
	int node;

	node = OF_finddevice("/");
	if (OF_is_compatible(node, "rockchip,rk3128") ||
	    OF_is_compatible(node, "rockchip,rk3126"))
		return (&rk3128_platform);

	return (NULL);
}
