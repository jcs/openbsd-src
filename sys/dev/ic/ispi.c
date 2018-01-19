/* $OpenBSD$ */
/*
 * Intel 100 Series SPI controller
 *
 * Copyright (c) 2015-2018 joshua stein <jcs@openbsd.org>
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
#include <sys/kernel.h>
#include <sys/kthread.h>

#include <dev/acpi/acpireg.h>
#include <dev/acpi/acpivar.h>
#include <dev/acpi/acpidev.h>
#include <dev/acpi/amltypes.h>
#include <dev/acpi/dsdt.h>

#include <dev/pci/lpssreg.h>

#include <dev/spi/spivar.h>

#include <dev/ic/ispivar.h>
#include <dev/ic/ispireg.h>

struct cfdriver ispi_cd = {
	NULL, "ispi", DV_DULL
};

int
ispi_activate(struct device *self, int act)
{
#if 0
	struct ispi_softc *sc = (struct ispi_softc *)self;
	switch (act) {
	case DVACT_SUSPEND:
		/* TODO */
		break;
	case DVACT_WAKEUP:
		/* TODO */
		break;
	}
#endif

	config_activate_children(self, act);

	return 0;
}

const char *
ispi_spi_intr_string(void *cookie, void *ih)
{
       struct ispi_intr_info *iii = ih;
       static char irqstr[128];

       if (iii->gpe_int_node)
	       snprintf(irqstr, sizeof(irqstr), "gpe %s",
		   aml_nodename(iii->gpe_int_node));

       return irqstr;
}

int
ispi_spi_print(void *aux, const char *pnp)
{
	struct spi_attach_args *sa = aux;

	if (pnp != NULL)
		printf("\"%s\" at %s", sa->sa_name, pnp);

	return UNCONF;
}

int
ispi_init(struct ispi_softc *sc)
{
	uint32_t tmp;

	/* load config */
	ispi_write(sc, SSCR0, 0);
	tmp = SSCR1_RxTresh(RX_THRESH_DFLT) | SSCR1_TxTresh(TX_THRESH_DFLT);
	ispi_write(sc, SSCR1, tmp);
	tmp = SSCR0_SCR(2) | SSCR0_Motorola | SSCR0_DataSize(8);
	ispi_write(sc, SSCR0, tmp);

	ispi_write(sc, SSTO, 0);
	ispi_write(sc, SSPSP, 0);

	if (1 /* is lpss_lpt_ssp */) {
		/* lpss_ssp_setup() { */
		/*
			LPSS_SPT_SSP
			.offset = 0x200,
			.reg_general = -1,
			.reg_ssp = 0x20,
			.reg_cs_ctrl = 0x24,
			.reg_capabilities = -1,
			.rx_threshold = 1,
			.tx_threshold_lo = 32,
			.tx_threshold_hi = 56,
		} */
		/* enable software chip select control */
		tmp = ispi_read(sc,
		    0x200 + /* lpss_base */
		    0x24); /* reg_cs_ctrl */
		tmp &= ~(LPSS_CS_CONTROL_SW_MODE | LPSS_CS_CONTROL_CS_HIGH);
        	tmp |= LPSS_CS_CONTROL_SW_MODE | LPSS_CS_CONTROL_CS_HIGH;
        	ispi_write(sc,
		    0x200 + /* lpss_base */
		    0x24, /* reg_cs_ctrl */
		    tmp);
	}

	/* num_chipselect = 1 */
	/* max_clk_rate = 50000000 */

	return 0;
}

void
ispi_write(struct ispi_softc *sc, int reg, uint32_t val)
{
	DPRINTF(("%s: %s(0x%x, 0x%x)\n", sc->sc_dev.dv_xname, __func__, reg,
	    val));

	bus_space_write_4(sc->sc_iot, sc->sc_ioh, reg, val);
}

uint32_t
ispi_read(struct ispi_softc *sc, int reg)
{
	uint32_t val = bus_space_read_4(sc->sc_iot, sc->sc_ioh, reg);
	DPRINTF(("%s: %s(0x%x) = 0x%x\n", sc->sc_dev.dv_xname, __func__, reg,
	    val));
	return val;
}

int
ispi_intr(void *arg)
{
	printf("ispi_intr!\n");

	return 1;
}

int
ispi_configure(void *cookie, int slave, int mode, int speed)
{
	// struct ispi_softc *sc = cookie;

	/* TODO */

	return 0;
}

int
ispi_transfer(void *cookie, struct spi_transfer *st)
{
	struct ispi_softc *sc = cookie;
	int s = splbio();

	spi_transq_enqueue(&sc->sc_q, st);
	if (!sc->sc_running) {
		ispi_start(sc);
	}
	splx(s);

	return 0;
}

void
ispi_start(struct ispi_softc *sc)
{
	/* TODO */

	sc->sc_running = 0;
}

int
ispi_slave_intr(struct aml_node *node, int notify_type, void *arg)
{
	printf("%s\n", __func__);

	return 0;
}
