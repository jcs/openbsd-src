/* $OpenBSD$ */
/*
 * Intel 100 Series SPI controller
 * ACPI attachment
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

#include <dev/ic/ispivar.h>

struct ispi_dsm_params {
	uint64_t spi_sclk_period;
	uint64_t spi_word_size;
	uint64_t spi_bit_order;
	uint64_t spi_spo;
	uint64_t spi_sph;
	uint64_t spi_cs_delay;
	uint64_t reset_a2r_usec;
	uint64_t reset_rec_usec;
};

int		ispi_acpi_match(struct device *, void *, void *);
void		ispi_acpi_attach(struct device *, struct device *, void *);

int		ispi_activate(struct device *, int);
void		ispi_bus_scan(struct device *, struct spibus_attach_args *,
		    void *);

int		ispi_configure(void *, int, int, int);
int		ispi_transfer(void *, struct spi_transfer *);
void		ispi_start(struct ispi_softc *);
void		ispi_send(struct ispi_softc *);
void		ispi_recv(struct ispi_softc *);

struct aml_node *acpi_pci_match(struct device *dev, struct pci_attach_args *pa);

const char *	ispi_spi_intr_string(void *cookie, void *ih);
void *		ispi_spi_intr_establish(void *, void *, int,
		    int (*)(void *), void *, const char *);

int		ispi_acpi_found_satopcase(struct ispi_softc *sc,
		    struct aml_node *node, char *dev);

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

	printf(": %s", sc->sc_devnode->name);

	/* TODO */

	return;
}

void
ispi_acpi_bus_scan(struct device *spi, struct spibus_attach_args *sba,
    void *aux)
{
	struct ispi_softc *sc = (struct ispi_softc *)aux;

	sc->sc_spi = spi;
	aml_find_node(sc->sc_devnode, "_HID", ispi_acpi_found_hid, sc);
}

void *
ispi_spi_intr_establish(void *cookie, void *ih, int level,
    int (*func)(void *), void *arg, const char *name)
{
	struct ispi_intr_info *iii = ih;

	if (iii->gpe_int_node) {
		aml_register_notify(iii->gpe_int_node->parent, NULL,
		    ispi_slave_intr, arg, ACPIDEV_NOPOLL);
		return ih;
	}

	return NULL;
}

int
ispi_acpi_found_hid(struct aml_node *node, void *arg)
{
	struct ispi_softc *sc = (struct ispi_softc *)arg;
	int64_t sta;
	char cdev[16], dev[16];

	if (node->parent == sc->sc_devnode)
		return 0;

	if (acpi_parsehid(node, arg, cdev, dev, 16) != 0)
		return 0;

	if (aml_evalinteger(acpi_softc, node->parent, "_STA", 0, NULL, &sta))
		sta = STA_PRESENT | STA_ENABLED | STA_DEV_OK | 0x1000;

	if ((sta & STA_PRESENT) == 0)
		return 0;

	DPRINTF(("%s: found HID %s at %s\n", sc->sc_dev.dv_xname, dev,
	    aml_nodename(node)));

	acpi_attach_deps(acpi_softc, node->parent);

	if (strcmp(dev, "APP000D") == 0)
		return ispi_acpi_found_satopcase(sc, node->parent, dev);

	return 0;
}

int
ispi_acpi_found_satopcase(struct ispi_softc *sc, struct aml_node *node,
    char *dev)
{
	struct spi_attach_args sa;
	struct aml_value cmd[4], res;
	struct ispi_intr_info iii;
	struct ispi_dsm_params dsmp;
	uint64_t val;
	int i;

	/* a0b5b7c6-1318-441c-b0c9-fe695eaf949b */
	static uint8_t topcase_guid[] = {
		0xC6, 0xB7, 0xB5, 0xA0, 0x18, 0x13, 0x1C, 0x44,
		0xB0, 0xC9, 0xFE, 0x69, 0x5E, 0xAF, 0x94, 0x9B,
	};

	/* Don't attach satopcase if USB interface is present */
	if (aml_evalinteger(acpi_softc, node, "UIST", 0, NULL, &val) == 0 &&
	    val) {
		DPRINTF(("%s: not attaching satopcase, USB enabled\n",
		    sc->sc_dev.dv_xname));
		return 0;
	}

	/* Or if SPI is not enabled (what is then?) */
	if (aml_evalinteger(acpi_softc, node, "SIST", 0, NULL, &val) == 0 &&
	    !val) {
		DPRINTF(("%s: SPI mode not enabled\n", sc->sc_dev.dv_xname));
		return 0;
	}

	/*
	 * On newer Apple hardware where we claim an OSI of Darwin, _CRS
	 * doesn't return a useful SpiSerialBusV2 object but instead returns
	 * parameters from a _DSM method when called with a particular UUID
	 * which macOS does.
	 */
	if (!aml_searchname(node, "_DSM")) {
		printf("%s: couldn't find _DSM at %s\n", sc->sc_dev.dv_xname,
		    aml_nodename(node));
		return 0;
	}

	bzero(&cmd, sizeof(cmd));
	cmd[0].type = AML_OBJTYPE_BUFFER;
	cmd[0].v_buffer = (uint8_t *)&topcase_guid;
	cmd[0].length = sizeof(topcase_guid);
	cmd[1].type = AML_OBJTYPE_INTEGER;
	cmd[1].v_integer = 1;
	cmd[1].length = 1;
	cmd[2].type = AML_OBJTYPE_INTEGER;
	cmd[2].v_integer = 1;
	cmd[2].length = 1;
	cmd[3].type = AML_OBJTYPE_BUFFER;
	cmd[3].length = 0;

	if (aml_evalname(acpi_softc, node, "_DSM", 4, cmd, &res)) {
		printf("%s: eval of _DSM at %s failed\n",
		    sc->sc_dev.dv_xname, aml_nodename(node));
		return 0;
	}

	if (res.type != AML_OBJTYPE_PACKAGE) {
		printf("%s: bad _DSM result at %s: %d\n",
		    sc->sc_dev.dv_xname, aml_nodename(node), res.type);
		aml_freevalue(&res);
		return 0;
	}

	if (res.length % 2 != 0) {
		printf("%s: _DSM length %d not even\n", sc->sc_dev.dv_xname,
		    res.length);
		aml_freevalue(&res);
		return 0;
	}

	memset(&iii, 0, sizeof(iii));
	iii.gpe_int_node = aml_searchname(node, "_GPE");

	memset(&dsmp, 0, sizeof(dsmp));
	for (i = 0; i < res.length; i += 2) {
		char *k;
		int64_t v;

		if (res.v_package[i]->type != AML_OBJTYPE_STRING ||
		    res.v_package[i + 1]->type != AML_OBJTYPE_BUFFER) {
			printf("%s: expected string+buffer, got %d+%d\n",
			    sc->sc_dev.dv_xname, res.v_package[i]->type,
			    res.v_package[i + 1]->type);
			break;
		}

		k = res.v_package[i]->v_string;
		v = aml_val2int(res.v_package[i + 1]);

		DPRINTF(("%s: %s = %lld\n", sc->sc_dev.dv_xname, k, v));

		if (strcmp(k, "spiSclkPeriod")) {
			dsmp.spi_sclk_period = v;
		} else if (strcmp(k, "spiWordSize")) {
			dsmp.spi_word_size = v;
		} else if (strcmp(k, "spiBitOrder")) {
			dsmp.spi_bit_order = v;
		} else if (strcmp(k, "spiSPO")) {
			dsmp.spi_spo = v;
		} else if (strcmp(k, "spiSPH")) {
			dsmp.spi_sph = v;
		} else if (strcmp(k, "spiCSDelay")) {
			dsmp.spi_cs_delay = v;
		} else if (strcmp(k, "resetA2RUsec")) {
			dsmp.reset_a2r_usec = v;
		} else if (strcmp(k, "resetRecUsec")) {
			dsmp.reset_rec_usec = v;
		} else {
			printf("%s: unknown _DSM key %s\n",
			    sc->sc_dev.dv_xname, k);
		}
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_tag = sc->sc_sba.sba_tag;
	sa.sa_name = "satopcase";
	sa.sa_intr = &iii;
	sa.sa_cookie = &dsmp;

	if (config_found(sc->sc_spi, &sa, ispi_spi_print)) {
		aml_freevalue(&res);
		node->attached = 1;
		return 0;
	}

	aml_freevalue(&res);

	return 1;
}
