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
#include <machine/bus.h>
#include <armv7/armv7/armv7_machdep.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/fdt.h>

extern void rk3128_suspend(void);

struct armv7_platform rk3128_platform = {
#ifdef SUSPEND
	.cpu_suspend = rk3128_suspend,
#endif
};

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
