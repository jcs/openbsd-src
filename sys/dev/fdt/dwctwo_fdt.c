/*	$OpenBSD: bcm2835_dwctwo.c,v 1.4 2022/09/04 08:42:39 mglocker Exp $	*/
/*
 * Copyright (c) 2015 Masao Uebayashi <uebayasi@tombiinc.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
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
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/kthread.h>

#include <machine/intr.h>
#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/fdt.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdivar.h>
#include <dev/usb/usb_mem.h>
#include <dev/usb/usb_quirks.h>

#include <dev/usb/dwc2/dwc2var.h>
#include <dev/usb/dwc2/dwc2.h>
#include <dev/usb/dwc2/dwc2_core.h>
#include <dev/usb/dwc2/dwc2_hw.h>

struct dwctwo_fdt_softc {
	struct dwc2_softc	sc_dwc2;
	void			*sc_ih;
};

int	dwctwo_fdt_match(struct device *, void *, void *);
void	dwctwo_fdt_attach(struct device *, struct device *, void *);
void	dwctwo_fdt_deferred(void *);

const struct cfattach dwctwo_fdt_ca = {
	sizeof(struct dwctwo_fdt_softc), dwctwo_fdt_match, dwctwo_fdt_attach,
};

struct cfdriver dwctwo_cd = {
	NULL, "dwctwo", DV_DULL
};

void
dwctwo_set_bcm_params(struct dwc2_hsotg *hsotg)
{
	struct dwc2_core_params *p = &hsotg->params;

	p->otg_caps.hnp_support		= 0;	/* HNP/SRP capable */
	p->otg_caps.srp_support		= 0;
	p->host_dma			= 1;
	p->dma_desc_enable		= 0;
	p->speed			= 0;	/* High Speed */
	p->enable_dynamic_fifo		= 1;
	p->en_multiple_tx_fifo		= 1;
	p->host_rx_fifo_size		= 774;	/* 774 DWORDs */
	p->host_nperio_tx_fifo_size	= 256;	/* 256 DWORDs */
	p->host_perio_tx_fifo_size	= 512;	/* 512 DWORDs */
	p->max_transfer_size		= 65535;
	p->max_packet_count		= 511;
	p->host_channels		= 8;
	p->phy_type			= 1;	/* UTMI */
	p->phy_utmi_width		= 8;	/* 8 bits */
	p->phy_ulpi_ddr			= 0;	/* Single */
	p->phy_ulpi_ext_vbus		= 0;
	p->i2c_enable			= 0;
	p->ulpi_fs_ls			= 0;
	p->host_support_fs_ls_low_power	= 0;
	p->host_ls_low_power_phy_clk	= 0;	/* 48 MHz */
	p->ts_dline			= 0;
	p->reload_ctl			= 0;
	p->ahbcfg			= 0x10;
	p->uframe_sched			= 1;
	p->external_id_pin_ctl		= 0;
}

void
dwctwo_set_rk_params(struct dwc2_hsotg *hsotg)
{
	struct dwc2_core_params *p = &hsotg->params;

	p->otg_caps.hnp_support = false;
	p->otg_caps.srp_support = false;
	p->host_rx_fifo_size = 525;
	p->host_nperio_tx_fifo_size = 128;
	p->host_perio_tx_fifo_size = 256;
	p->ahbcfg = GAHBCFG_HBSTLEN_INCR16 << GAHBCFG_HBSTLEN_SHIFT;
	p->power_down = DWC2_POWER_DOWN_PARAM_NONE;
}

int
dwctwo_fdt_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = (struct fdt_attach_args *)aux;

	return (OF_is_compatible(faa->fa_node, "brcm,bcm2708-usb") ||
	    OF_is_compatible(faa->fa_node, "brcm,bcm2835-usb") ||
	    OF_is_compatible(faa->fa_node, "rockchip,rk3066-usb") ||
	    OF_is_compatible(faa->fa_node, "snps,dwc2"));
}

void
dwctwo_fdt_attach(struct device *parent, struct device *self, void *aux)
{
	struct dwctwo_fdt_softc *sc = (struct dwctwo_fdt_softc *)self;
	struct fdt_attach_args *faa = aux;
	int idx;

	sc->sc_dwc2.sc_iot = faa->fa_iot;
	sc->sc_dwc2.sc_bus.pipe_size = sizeof(struct usbd_pipe);
	sc->sc_dwc2.sc_bus.dmatag = faa->fa_dmat;

	if (OF_is_compatible(faa->fa_node, "brcm,bcm2708-usb") ||
	    OF_is_compatible(faa->fa_node, "brcm,bcm2835-usb"))
		sc->sc_dwc2.sc_set_params = dwctwo_set_bcm_params;
	else if (OF_is_compatible(faa->fa_node, "rockchip,rk3066-usb"))
		sc->sc_dwc2.sc_set_params = dwctwo_set_rk_params;
	else if (OF_is_compatible(faa->fa_node, "snps,dwc2")) {
		/* use defaults */
	} else {
		printf(": unknown device\n");
		return;
	}

	printf("\n");

	if (bus_space_map(faa->fa_iot, faa->fa_reg[0].addr,
	    faa->fa_reg[0].size, 0, &sc->sc_dwc2.sc_ioh))
		panic("%s: bus_space_map failed!", __func__);

	idx = OF_getindex(faa->fa_node, "usb", "interrupt-names");
	if (idx == -1)
		idx = 0;

	sc->sc_ih = fdt_intr_establish_idx(faa->fa_node, idx,
	    IPL_VM | IPL_MPSAFE, dwc2_intr, (void *)&sc->sc_dwc2,
	    sc->sc_dwc2.sc_bus.bdev.dv_xname);
	if (sc->sc_ih == NULL)
		panic("%s: intr_establish failed!", __func__);

	kthread_create_deferred(dwctwo_fdt_deferred, sc);
}

void
dwctwo_fdt_deferred(void *self)
{
	struct dwctwo_fdt_softc *sc = (struct dwctwo_fdt_softc *)self;
	int rc;

	strlcpy(sc->sc_dwc2.sc_vendor, "DWC2", sizeof(sc->sc_dwc2.sc_vendor));
	rc = dwc2_init(&sc->sc_dwc2);
	if (rc != 0)
		return;

	sc->sc_dwc2.sc_child = config_found(&sc->sc_dwc2.sc_bus.bdev,
	    &sc->sc_dwc2.sc_bus, usbctlprint);
}
