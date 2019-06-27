/* $OpenBSD$ */
/*
 * Intel LPSS SPI controller
 * ACPI attachment
 *
 * Copyright (c) 2015-2019 joshua stein <jcs@openbsd.org>
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

#include <dev/ic/ispivar.h>

#ifdef ISPI_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

int		ispi_acpi_match(struct device *, void *, void *);
void		ispi_acpi_attach(struct device *, struct device *, void *);

int		ispi_activate(struct device *, int);
void		ispi_acpi_bus_scan(struct ispi_softc *);

int		ispi_configure(void *, int, int, int);
void		ispi_start(struct ispi_softc *);
void		ispi_send(struct ispi_softc *);
void		ispi_recv(struct ispi_softc *);

void *		ispi_spi_intr_establish(void *, void *, int,
		    int (*)(void *), void *, const char *);

struct cfattach ispi_acpi_ca = {
	sizeof(struct ispi_softc),
	ispi_acpi_match,
	ispi_acpi_attach,
	NULL,
	ispi_activate,
};

const char *ispi_acpi_hids[] = {
	"INT33C0",
	"INT33C1",
	"INT3430",
	"INT3431",
	NULL
};

int
ispi_acpi_match(struct device *parent, void *match, void *aux)
{
	struct acpi_attach_args *aaa = aux;
	struct cfdata *cf = match;

	return acpi_matchhids(aaa, ispi_acpi_hids, cf->cf_driver->cd_name);
}

void
ispi_acpi_attach(struct device *parent, struct device *self, void *aux)
{
	struct ispi_softc *sc = (struct ispi_softc *)self;
	struct acpi_attach_args *aa = aux;

	sc->sc_acpi = (struct acpi_softc *)parent;
	sc->sc_devnode = aa->aaa_node;
	sc->sc_nsubdevs = 0;

	printf(": %s", sc->sc_devnode->name);

	ispi_init(sc);

	/* TODO: acpi-only initialization */

	return;
}

void
ispi_acpi_bus_scan(struct ispi_softc *sc)
{
	aml_find_node(sc->sc_devnode, "_HID", ispi_acpi_found_hid, sc);
}

void *
ispi_spi_intr_establish(void *cookie, void *ih, int level,
    int (*func)(void *), void *arg, const char *name)
{
	struct ispi_softc *sc = (struct ispi_softc *)cookie;
	struct ispi_gpe_intr *igi = ih;
	int flags;

	if (sc->sc_nsubdevs == nitems(sc->sc_subdevs))
		return NULL;

	if (igi && igi->gpe_node) {
		sc->sc_subdevs[sc->sc_nsubdevs].cookie = sc;
		sc->sc_subdevs[sc->sc_nsubdevs].func = func;
		sc->sc_subdevs[sc->sc_nsubdevs].arg = arg;

		/*
		 * Avoid using the ACPI task queue because it's too slow, and
		 * our subdev might not be doing anything ACPI-related anyway.
		 */
		flags = GPE_DIRECT;
		if (level)
			flags |= GPE_LEVEL;

		acpi_set_gpehandler(acpi_softc, igi->gpe_int, ispi_subdev_intr,
		    &sc->sc_subdevs[sc->sc_nsubdevs], flags);

		sc->sc_nsubdevs++;

		return ih;
	}

	return NULL;
}

int
ispi_acpi_found_hid(struct aml_node *node, void *arg)
{
	struct ispi_softc *sc = (struct ispi_softc *)arg;
	struct spi_attach_args sa;
	int64_t sta;
	char cdev[16], dev[16];

	if (node->parent == sc->sc_devnode)
		return 0;

	if (acpi_parsehid(node, arg, cdev, dev, 16) != 0)
		return 0;

	sta = acpi_getsta(acpi_softc, node->parent);
	if ((sta & STA_PRESENT) == 0)
		return 0;

	DPRINTF(("%s: found HID %s at %s\n", sc->sc_dev.dv_xname, dev,
	    aml_nodename(node)));

	acpi_attach_deps(acpi_softc, node->parent);

	if (strcmp(dev, "APP000D") == 0) {
		memset(&sa, 0, sizeof(sa));
		sa.sa_tag = &sc->sc_spi_tag;
		sa.sa_name = "satopcase";
		sa.sa_cookie = node->parent;

		if (config_found(&sc->sc_dev, &sa, ispi_spi_print)) {
			node->attached = 1;
			return 1;
		}
	}

	return 0;
}
