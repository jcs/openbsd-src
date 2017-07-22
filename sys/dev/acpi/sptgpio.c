/*	$OpenBSD$ */
/*
 * Intel Sunrisepoint GPIO
 * Copyright (c) 2017 joshua stein <jcs@openbsd.org>
 * Copyright (c) 2016 Mark Kettenis
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
#include <sys/malloc.h>
#include <sys/systm.h>

#include <dev/acpi/acpireg.h>
#include <dev/acpi/acpivar.h>
#include <dev/acpi/acpidev.h>
#include <dev/acpi/amltypes.h>
#include <dev/acpi/dsdt.h>

#define GENMASK(l, h)		((~0U >> (32 - h -1)) & (~0U << l))

#define SPT_REVID		0x0
#define SPT_REVID_SHIFT		16
#define SPT_REVID_MASK		GENMASK(31, 16)

#define SPT_PADBAR		0x00c

#define SPTLP_IE_OFFSET		0x120
#define SPTLP_GPP_SIZE		24

struct spt_community {
	int barno;
	int first_pin;
	int last_pin;
	int gpp_size;
	int ie_offset;

	bus_space_handle_t sc_memh;
	bus_addr_t sc_addr;
	bus_size_t sc_size;

	int features;

	uint32_t padbar;
};

/* Sunrisepoint-LP */
const struct spt_community sptlp_communities[] = {
	{ 0,	0,	47,	SPTLP_GPP_SIZE,	SPTLP_IE_OFFSET },
	{ 1,	48,	119,	SPTLP_GPP_SIZE,	SPTLP_IE_OFFSET },
	{ 2,	120,	151,	SPTLP_GPP_SIZE,	SPTLP_IE_OFFSET },
};
#define SPTLP_NCOMMUNITIES 3

struct sptgpio_intrhand {
	int (*ih_func)(void *);
	void *ih_arg;
};

struct sptgpio_softc {
	struct device sc_dev;
	struct acpi_softc *sc_acpi;
	struct aml_node *sc_node;

	bus_space_tag_t sc_memt;
	struct spt_community *sc_comms;
	int sc_ncomms;

	int sc_irq;
	int sc_irq_flags;
	void *sc_ih;

	const int *sc_pins;
	int sc_npins;
	struct sptgpio_intrhand *sc_pin_ih;

	struct acpi_gpio sc_gpio;
};

int	sptgpio_match(struct device *, void *, void *);
void	sptgpio_attach(struct device *, struct device *, void *);

struct cfattach sptgpio_ca = {
	sizeof(struct sptgpio_softc), sptgpio_match, sptgpio_attach
};

struct cfdriver sptgpio_cd = {
	NULL, "sptgpio", DV_DULL
};

const char *sptgpio_hids[] = {
	"INT344B",
	NULL
};

int	sptgpio_parse_resources(int, union acpi_resource *, void *);
int	sptgpio_read_pin(void *, int);
void	sptgpio_write_pin(void *, int, int);
void	sptgpio_intr_establish(void *, int, int, int (*)(), void *);
int	sptgpio_intr(void *);

int
sptgpio_match(struct device *parent, void *match, void *aux)
{
	struct acpi_attach_args *aaa = aux;
	struct cfdata *cf = match;

	return acpi_matchhids(aaa, sptgpio_hids, cf->cf_driver->cd_name);
}

int
sptgpio_pin_community(struct sptgpio_softc *sc, int pin)
{
	int i;

	for (i = 0; i < sc->sc_ncomms; i++) {
		if (pin >= sc->sc_comms[i].first_pin &&
		    pin <= sc->sc_comms[i].last_pin)
			return i;
	}

	return -1;
}

void
sptgpio_irq_mask(struct sptgpio_softc *sc, int pin, int mask)
{
	int gpp_offset, gpp, pad;
	struct spt_community *c;
	uint32_t v;

	int comm = sptgpio_pin_community(sc, pin);
	if (comm == -1)
		return;

	c = &sc->sc_comms[comm];
	pad = (pin - c->first_pin);

	gpp_offset = pad % c->gpp_size;
	gpp = pad / c->gpp_size;

	v = bus_space_read_4(sc->sc_memt, c->sc_memh, c->ie_offset + (gpp * 4));
	if (mask)
		v &= ~(1UL << gpp_offset);
	else
		v |= (1UL << gpp_offset);
	bus_space_write_4(sc->sc_memt, c->sc_memh, c->ie_offset + (gpp * 4), v);
}

void
sptgpio_attach(struct device *parent, struct device *self, void *aux)
{
	struct acpi_attach_args *aaa = aux;
	struct sptgpio_softc *sc = (struct sptgpio_softc *)self;
	struct aml_value res;
	int i, j;

	sc->sc_acpi = (struct acpi_softc *)parent;
	sc->sc_node = aaa->aaa_node;
	printf(": %s", sc->sc_node->name);

	/* TODO: determine sptlp/spt-h */
	if (1) {
		sc->sc_comms = malloc(sizeof(sptlp_communities), M_DEVBUF,
		    M_NOWAIT | M_ZERO);
		memcpy(sc->sc_comms, sptlp_communities,
		    sizeof(sptlp_communities));
	}

	if (aml_evalname(sc->sc_acpi, sc->sc_node, "_CRS", 0, NULL, &res)) {
		printf(", can't find registers\n");
		return;
	}

	aml_parse_resource(&res, sptgpio_parse_resources, sc);
	aml_freevalue(&res);

	if (!sc->sc_ncomms) {
		printf(", no communities\n");
		return;
	}

	sc->sc_npins = sc->sc_comms[sc->sc_ncomms - 1].last_pin + 1;

	sc->sc_pin_ih = mallocarray(sc->sc_npins, sizeof(*sc->sc_pin_ih),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (sc->sc_pin_ih == NULL) {
		printf("\n");
		return;
	}

	printf(" irq %d", sc->sc_irq);

	sc->sc_memt = aaa->aaa_memt;
	for (i = 0; i < sc->sc_ncomms; i++) {
		if (bus_space_map(sc->sc_memt, sc->sc_comms[i].sc_addr,
		    sc->sc_comms[i].sc_size, 0,
		    &sc->sc_comms[i].sc_memh)) {
			printf(", can't map memory for community %d\n", i);
			goto free;
		}

		sc->sc_comms[i].padbar = bus_space_read_4(sc->sc_memt,
		    sc->sc_comms[i].sc_memh, SPT_PADBAR);
	}

	sc->sc_ih = acpi_intr_establish(sc->sc_irq, sc->sc_irq_flags, IPL_BIO,
	    sptgpio_intr, sc, sc->sc_dev.dv_xname);
	if (sc->sc_ih == NULL) {
		printf(", can't establish interrupt\n");
		goto unmap;
	}

	sc->sc_gpio.cookie = sc;
	sc->sc_gpio.read_pin = sptgpio_read_pin;
	sc->sc_gpio.write_pin = sptgpio_write_pin;
	sc->sc_gpio.intr_establish = sptgpio_intr_establish;
	sc->sc_node->gpio = &sc->sc_gpio;

	/* Mask all interrupts. */
	for (i = 0; i < sc->sc_ncomms; i++) {
		struct spt_community c = sc->sc_comms[i];

		for (j = c.first_pin; j <= c.last_pin; j++)
			sptgpio_irq_mask(sc, j, 1);
	}

	printf(", %d pins\n", sc->sc_npins);

	return;

unmap:
	for (i = 0; i < sc->sc_ncomms; i++)
		bus_space_unmap(sc->sc_memt, sc->sc_comms[i].sc_memh,
		    sc->sc_comms[i].sc_size);
free:
	free(sc->sc_pin_ih, M_DEVBUF, sc->sc_npins * sizeof(*sc->sc_pin_ih));
}

int
sptgpio_parse_resources(int crsidx, union acpi_resource *crs, void *arg)
{
	struct sptgpio_softc *sc = arg;
	int type = AML_CRSTYPE(crs);

	switch (type) {
	case LR_MEM32FIXED: {
		sc->sc_ncomms++;
		sc->sc_comms[sc->sc_ncomms - 1].sc_addr = crs->lr_m32fixed._bas;
		sc->sc_comms[sc->sc_ncomms - 1].sc_size = crs->lr_m32fixed._len;
		break;
	}
	case LR_EXTIRQ:
		sc->sc_irq = crs->lr_extirq.irq[0];
		sc->sc_irq_flags = crs->lr_extirq.flags;
		break;
	default:
		printf(" type 0x%x\n", type);
		break;
	}

	return 0;
}

int
sptgpio_read_pin(void *cookie, int pin)
{
	return 0;
}

void
sptgpio_write_pin(void *cookie, int pin, int value)
{
}

void
sptgpio_intr_establish(void *cookie, int pin, int flags,
    int (*func)(void *), void *arg)
{
	printf("%s\n", __func__);
}

int
sptgpio_intr(void *arg)
{
	printf("%s\n", __func__);
	return 1;
}
