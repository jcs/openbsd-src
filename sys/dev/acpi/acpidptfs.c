/* $OpenBSD$ */
/*
 * Copyright (c) 2021 joshua stein <jcs@jcs.org>
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
#include <sys/device.h>
#include <sys/kernel.h>

#include <dev/acpi/acpivar.h>
#include <dev/acpi/acpidev.h>
#include <dev/acpi/amltypes.h>
#include <dev/acpi/dsdt.h>

#include <sys/sensors.h>

struct acpidptfs_softc {
	struct device		sc_dev;

	struct acpi_softc	*sc_acpi;
	struct aml_node		*sc_devnode;

	int			sc_devtype;

	struct ksensor		sc_sensor;
	struct ksensordev	sc_sensdev;
};

#define ACPIDPTFS_TYPE_SENSOR	0x03
#define ACPIDPTFS_TYPE_CHARGER	0x0B
#define ACPIDPTFS_TYPE_BATTERY	0x0C

int	acpidptfs_match(struct device *, void *, void *);
void	acpidptfs_attach(struct device *, struct device *, void *);
void	acpidptfs_sensor_add(struct acpidptfs_softc *);
int	acpidptfs_notify(struct aml_node *, int, void *);
void	acpidptfs_update(struct acpidptfs_softc *);

struct cfattach acpidptfs_ca = {
	sizeof(struct acpidptfs_softc),
	acpidptfs_match,
	acpidptfs_attach,
	NULL,
};

struct cfdriver acpidptfs_cd = {
	NULL, "acpidptfs", DV_DULL
};

const char *acpidptfs_hids[] = {
	"INT3403",
	"INTC1043",
	"INTC1046",
	NULL
};

int
acpidptfs_match(struct device *parent, void *match, void *aux)
{
	struct acpi_attach_args *aaa = aux;
	struct cfdata *cf = match;

	return acpi_matchhids(aaa, acpidptfs_hids, cf->cf_driver->cd_name);
}

void
acpidptfs_attach(struct device *parent, struct device *self, void *aux)
{
	struct acpidptfs_softc *sc = (struct acpidptfs_softc *)self;
	struct acpi_attach_args	*aa = aux;
	int64_t	res;

	sc->sc_acpi = (struct acpi_softc *)parent;
	sc->sc_devnode = aa->aaa_node;
	sc->sc_devtype = -1;

	printf(": %s", sc->sc_devnode->name);

	if (aml_evalinteger((struct acpi_softc *)parent, aa->aaa_node,
	    "_TMP", 0, NULL, &res) == 0)
		sc->sc_devtype = ACPIDPTFS_TYPE_SENSOR;
	else if (aml_evalinteger((struct acpi_softc *)parent, aa->aaa_node,
	    "PTYP", 0, NULL, &res) == 0)
	    	sc->sc_devtype = res;

	switch (sc->sc_devtype) {
	case ACPIDPTFS_TYPE_SENSOR:
		acpidptfs_sensor_add(sc);
		break;
	case ACPIDPTFS_TYPE_CHARGER:
		/* TODO */
		printf(", charger\n");
		break;
	case ACPIDPTFS_TYPE_BATTERY:
		/* TODO */
		printf(", battery\n");
		break;
	default:
		printf(", unknown type\n");
		return;
	}

	aml_register_notify(sc->sc_devnode, aa->aaa_dev, acpidptfs_notify,
	    sc, ACPIDEV_POLL);
}

void
acpidptfs_sensor_add(struct acpidptfs_softc *sc)
{
	struct aml_value res;

	strlcpy(sc->sc_sensdev.xname, DEVNAME(sc),
	    sizeof(sc->sc_sensdev.xname));

	if (aml_evalname(sc->sc_acpi, sc->sc_devnode, "_STR", 0, NULL,
	    &res) == 0)
		strlcpy(sc->sc_sensor.desc, aml_utf16_to_string(&res),
		    sizeof(sc->sc_sensor.desc));
	else
		strlcpy(sc->sc_sensor.desc, sc->sc_devnode->name,
		    sizeof(sc->sc_sensor.desc));

	printf(", sensor \"%s\"\n", sc->sc_sensor.desc);

	aml_freevalue(&res);

	acpidptfs_update(sc);

	sc->sc_sensor.type = SENSOR_TEMP;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sensor);
	sensordev_install(&sc->sc_sensdev);
}

void
acpidptfs_update(struct acpidptfs_softc *sc)
{
	int64_t	val;

	if (aml_evalinteger(sc->sc_acpi, sc->sc_devnode, "_TMP", 0, NULL,
	    &val) != 0) {
		sc->sc_sensor.flags |= SENSOR_FINVALID;
		return;
	}

	sc->sc_sensor.value = (val * 100000); /* -> uK */
	sc->sc_sensor.flags &= ~SENSOR_FINVALID;
}

int
acpidptfs_notify(struct aml_node *node, int notify_type, void *arg)
{
	struct acpidptfs_softc *sc = arg;

	if (notify_type == 0)
		acpidptfs_update(sc);
	else
		printf("%s: %s: %d\n", sc->sc_dev.dv_xname, __func__, notify_type);

	return 0;
}
