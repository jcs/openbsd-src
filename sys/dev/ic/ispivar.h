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

#include <dev/pci/pcivar.h>

#include <dev/spi/spivar.h>

/* #define ISPI_DEBUG */

#ifdef ISPI_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

struct ispi_softc {
	struct device		sc_dev;

	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;

	void			*sc_ih;
	struct pci_attach_args	sc_paa;

	struct spibus_attach_args sc_sba;
	struct device		*sc_spi; /* spi bus after bus_scan callback */
	struct spi_controller	sc_spi_tag;

	SIMPLEQ_HEAD(,spi_transfer) sc_q;
	struct spi_transfer	*sc_transfer;
	struct spi_chunk	*sc_wchunk;
	struct spi_chunk	*sc_rchunk;

	struct acpi_softc	*sc_acpi;
	struct aml_node		*sc_devnode;
	char			sc_hid[16];
	u_int32_t		sc_caps;

	unsigned int		sc_running;
};

struct ispi_intr_info {
	struct aml_node *gpe_int_node;
};

int		ispi_init(struct ispi_softc *sc);

int		ispi_match(struct device *, void *, void *);
void		ispi_attach(struct device *, struct device *, void *);
int		ispi_activate(struct device *, int);
void		ispi_bus_scan(struct device *, struct spibus_attach_args *,
		    void *);
void *		ispi_spi_intr_establish(void *cookie, void *ih, int level,
		    int (*func)(void *), void *arg, const char *name);
const char *	ispi_spi_intr_string(void *cookie, void *ih);
int		ispi_spi_print(void *aux, const char *pnp);

void		ispi_write(struct ispi_softc *sc, int reg, uint32_t val);
uint32_t	ispi_read(struct ispi_softc *sc, int reg);

int		ispi_acpi_found_hid(struct aml_node *node, void *arg);

int		ispi_configure(void *, int, int, int);
int		ispi_transfer(void *, struct spi_transfer *);
void		ispi_start(struct ispi_softc *);
void		ispi_send(struct ispi_softc *);
void		ispi_recv(struct ispi_softc *);

int		ispi_intr(void *);
int		ispi_slave_intr(struct aml_node *node, int, void *);
