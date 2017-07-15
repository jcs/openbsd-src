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

#define SPTGPIO_CONF_GD_LEVEL	0x01000000
#define SPTGPIO_CONF_GD_TPE	0x02000000
#define SPTGPIO_CONF_GD_TNE	0x04000000
#define SPTGPIO_CONF_GD_MASK	0x07000000
#define SPTGPIO_CONF_DIRECT_IRQ_EN	0x08000000

#define SPTGPIO_PAD_VAL		0x00000001

#define SPTGPIO_IRQ_TS_0	0x800
#define SPTGPIO_IRQ_TS_1	0x804
#define SPTGPIO_IRQ_TS_2	0x808

struct sptgpio_intrhand {
	int (*ih_func)(void *);
	void *ih_arg;
};

struct sptgpio_softc {
	struct device sc_dev;
	struct acpi_softc *sc_acpi;
	struct aml_node *sc_node;

	bus_space_tag_t sc_memt;
	bus_space_handle_t sc_memh;
	bus_addr_t sc_addr;
	bus_size_t sc_size;

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

/*
 * The pads for the pins are randomly ordered.
 */

const int sptlp_pins[] = {
	64, 65,
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

void
sptgpio_attach(struct device *parent, struct device *self, void *aux)
{
	struct acpi_attach_args *aaa = aux;
	struct sptgpio_softc *sc = (struct sptgpio_softc *)self;
	struct aml_value res;
	struct aml_value arg[2];
	struct aml_node *node;
	uint32_t reg;
	int i;

	return;

	sc->sc_acpi = (struct acpi_softc *)parent;
	sc->sc_node = aaa->aaa_node;
	printf(": %s", sc->sc_node->name);

	sc->sc_pins = sptlp_pins;
	sc->sc_npins = nitems(sptlp_pins);

	if (aml_evalname(sc->sc_acpi, sc->sc_node, "_CRS", 0, NULL, &res)) {
		printf(", can't find registers\n");
		return;
	}

	aml_parse_resource(&res, sptgpio_parse_resources, sc);
	printf(" addr 0x%lx/0x%lx", sc->sc_addr, sc->sc_size);
	if (sc->sc_addr == 0 || sc->sc_size == 0) {
		printf("\n");
		return;
	}
	aml_freevalue(&res);

	sc->sc_pin_ih = mallocarray(sc->sc_npins, sizeof(*sc->sc_pin_ih),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (sc->sc_pin_ih == NULL) {
		printf("\n");
		return;
	}

	printf(" irq %d", sc->sc_irq);

	sc->sc_memt = aaa->aaa_memt;
	if (bus_space_map(sc->sc_memt, sc->sc_addr, sc->sc_size, 0,
	    &sc->sc_memh)) {
		printf(", can't map registers\n");
		goto free;
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
	for (i = 0; i < sc->sc_npins; i++) {
		reg = bus_space_read_4(sc->sc_memt, sc->sc_memh,
		    sc->sc_pins[i] * 16);

		/*
		 * Skip pins configured as direct IRQ.  Those are tied
		 * directly to the APIC.
		 */
		if (reg & SPTGPIO_CONF_DIRECT_IRQ_EN)
			continue;

		reg &= ~SPTGPIO_CONF_GD_MASK;
		bus_space_write_4(sc->sc_memt, sc->sc_memh,
		    sc->sc_pins[i] * 16, reg);
	}

	printf(", %d pins\n", sc->sc_npins);

	/* Register address space. */
	memset(&arg, 0, sizeof(arg));
	arg[0].type = AML_OBJTYPE_INTEGER;
	arg[0].v_integer = ACPI_OPREG_GPIO;
	arg[1].type = AML_OBJTYPE_INTEGER;
	arg[1].v_integer = 1;
	node = aml_searchname(sc->sc_node, "_REG");
	if (node && aml_evalnode(sc->sc_acpi, node, 2, arg, NULL))
		printf("%s: _REG failed\n", sc->sc_dev.dv_xname);

	return;

unmap:
	bus_space_unmap(sc->sc_memt, sc->sc_memh, sc->sc_size);
free:
	free(sc->sc_pin_ih, M_DEVBUF, sc->sc_npins * sizeof(*sc->sc_pin_ih));
}

int
sptgpio_parse_resources(int crsidx, union acpi_resource *crs, void *arg)
{
	struct sptgpio_softc *sc = arg;
	int type = AML_CRSTYPE(crs);

	switch (type) {
	case LR_MEM32FIXED:
		sc->sc_addr = crs->lr_m32fixed._bas;
		sc->sc_size = crs->lr_m32fixed._len;
		break;
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
	struct sptgpio_softc *sc = cookie;
	uint32_t reg;

	reg = bus_space_read_4(sc->sc_memt, sc->sc_memh,
	    sc->sc_pins[pin] * 16 + 8);
	return (reg & SPTGPIO_PAD_VAL);
}

void
sptgpio_write_pin(void *cookie, int pin, int value)
{
	struct sptgpio_softc *sc = cookie;
	uint32_t reg;

	reg = bus_space_read_4(sc->sc_memt, sc->sc_memh,
	    sc->sc_pins[pin] * 16 + 8);
	if (value)
		reg |= SPTGPIO_PAD_VAL;
	else
		reg &= ~SPTGPIO_PAD_VAL;
	bus_space_write_4(sc->sc_memt, sc->sc_memh, sc->sc_pins[pin] * 16 + 8,
	    reg);
}

void
sptgpio_intr_establish(void *cookie, int pin, int flags,
    int (*func)(void *), void *arg)
{
	struct sptgpio_softc *sc = cookie;
	uint32_t reg;

	KASSERT(pin >= 0 && pin < sc->sc_npins);

	sc->sc_pin_ih[pin].ih_func = func;
	sc->sc_pin_ih[pin].ih_arg = arg;

	reg = bus_space_read_4(sc->sc_memt, sc->sc_memh, sc->sc_pins[pin] * 16);
	reg &= ~SPTGPIO_CONF_GD_MASK;
	if ((flags & LR_GPIO_MODE) == 0)
		reg |= SPTGPIO_CONF_GD_LEVEL;
	if ((flags & LR_GPIO_POLARITY) == LR_GPIO_ACTLO)
		reg |= SPTGPIO_CONF_GD_TNE;
	if ((flags & LR_GPIO_POLARITY) == LR_GPIO_ACTHI)
		reg |= SPTGPIO_CONF_GD_TPE;
	if ((flags & LR_GPIO_POLARITY) == LR_GPIO_ACTBOTH)
		reg |= SPTGPIO_CONF_GD_TNE | SPTGPIO_CONF_GD_TPE;
	bus_space_write_4(sc->sc_memt, sc->sc_memh, sc->sc_pins[pin] * 16, reg);
}

int
sptgpio_intr(void *arg)
{
	struct sptgpio_softc *sc = arg;
	uint32_t reg;
	int rc = 0;
	int pin;

	for (pin = 0; pin < sc->sc_npins; pin++) {
		if (pin % 32 == 0) {
			reg = bus_space_read_4(sc->sc_memt, sc->sc_memh,
			    SPTGPIO_IRQ_TS_0 + (pin / 8));
			bus_space_write_4(sc->sc_memt, sc->sc_memh,
			    SPTGPIO_IRQ_TS_0 + (pin / 8), reg);
		}
		if (reg & (1 << (pin % 32))) {
			if (sc->sc_pin_ih[pin].ih_func)
				sc->sc_pin_ih[pin].ih_func(sc->sc_pin_ih[pin].ih_arg);
			rc = 1;
		}
	}

	return rc;
}
