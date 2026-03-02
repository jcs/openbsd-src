/* $OpenBSD$ */
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
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/sensors.h>

#include <machine/fdt.h>
#include <machine/apmvar.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_regulator.h>
#include <dev/ofw/fdt.h>

#include "simplebatvar.h"
#include "apm.h"

enum simplebat_sensor {
	SIMPLEBAT_SENSOR_VOLTAGE,
	SIMPLEBAT_SENSOR_CURRENT,
	SIMPLEBAT_SENSOR_CAPACITY,
	SIMPLEBAT_SENSOR_PRESENT,
	SIMPLEBAT_SENSOR_STATUS,
	SIMPLEBAT_NSENSORS
};

struct simplebat_ocv_entry {
	int voltage;	/* mV */
	int capacity;	/* percent */
};

struct simplebat_softc {
	struct device sc_dev;

	void *sc_cookie;
	int (*sc_voltage)(void *);
	int (*sc_current)(void *);
	int (*sc_present)(void *);
	int (*sc_status)(void *);

	int sc_mvolt_min;
	int sc_mvolt_max;
	int sc_mah_full;
	int sc_minutes_left;
	struct simplebat_ocv_entry *sc_ocv_table;
	int sc_ocv_count;

	struct ksensordev sc_sensdev;
	struct ksensor sc_sens[SIMPLEBAT_NSENSORS];
};

struct simplebat_softc *simplebat_sc;

int	simplebat_match(struct device *, void *, void *);
void	simplebat_attach(struct device *, struct device *, void *);
void	simplebat_refresh(void *);
int	simplebat_ocv_lookup(struct simplebat_softc *, int);
#if NAPM > 0
int	simplebat_apminfo(struct apm_power_info *);
#endif

const struct cfattach simplebat_ca = {
	sizeof(struct simplebat_softc), simplebat_match, simplebat_attach
};

struct cfdriver simplebat_cd = {
	NULL, "simplebat", DV_DULL
};

int
simplebat_match(struct device *parent, void *match, void *aux)
{
	struct simplebat_attach_args *saa = aux;

	return OF_is_compatible(saa->sa_node, "simple-battery");
}

void
simplebat_attach(struct device *parent, struct device *self, void *aux)
{
	struct simplebat_softc *sc = (struct simplebat_softc *)self;
	struct simplebat_attach_args *sa = aux;
	int node = sa->sa_node;
	int len, i, nentries;
	uint32_t *ocv_data;

	sc->sc_cookie = sa->sa_cookie;
	sc->sc_voltage = sa->sa_voltage;
	sc->sc_current = sa->sa_current;
	sc->sc_status = sa->sa_status;
	sc->sc_present = sa->sa_present;

	/* read voltage range from DT (microvolt -> millivolt) */
	sc->sc_mvolt_min = OF_getpropint(node,
	    "voltage-min-design-microvolt", 3400000) / 1000;
	sc->sc_mvolt_max = OF_getpropint(node,
	    "voltage-max-design-microvolt", 4200000) / 1000;
	sc->sc_mah_full = OF_getpropint(node,
	    "charge-full-design-microamp-hours", 0) / 1000;

	/* parse ocv-capacity-table-0 if present */
	len = OF_getproplen(node, "ocv-capacity-table-0");
	if (len > 0) {
		nentries = len / (2 * sizeof(uint32_t));
		ocv_data = malloc(len, M_DEVBUF, M_WAITOK);
		OF_getpropintarray(node, "ocv-capacity-table-0", ocv_data, len);
		sc->sc_ocv_table = mallocarray(nentries,
		    sizeof(struct simplebat_ocv_entry), M_DEVBUF, M_WAITOK);
		for (i = 0; i < nentries; i++) {
			/* uV -> mV */
			sc->sc_ocv_table[i].voltage = ocv_data[i * 2] / 1000;
			sc->sc_ocv_table[i].capacity = ocv_data[i * 2 + 1];
		}
		sc->sc_ocv_count = nentries;
		free(ocv_data, M_DEVBUF, len);
	}

	printf("\n");

	strlcpy(sc->sc_sensdev.xname, sc->sc_dev.dv_xname,
	    sizeof(sc->sc_sensdev.xname));

	strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_VOLTAGE].desc, "battery voltage",
	    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_VOLTAGE].desc));
	sc->sc_sens[SIMPLEBAT_SENSOR_VOLTAGE].type = SENSOR_VOLTS_DC;
	sc->sc_sens[SIMPLEBAT_SENSOR_VOLTAGE].flags = SENSOR_FINVALID;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[SIMPLEBAT_SENSOR_VOLTAGE]);

	strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].desc, "battery current",
	    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].desc));
	sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].type = SENSOR_AMPS;
	sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].flags = SENSOR_FINVALID;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT]);

	strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_CAPACITY].desc, "battery capacity",
	    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_CAPACITY].desc));
	sc->sc_sens[SIMPLEBAT_SENSOR_CAPACITY].type = SENSOR_PERCENT;
	sc->sc_sens[SIMPLEBAT_SENSOR_CAPACITY].flags = SENSOR_FINVALID;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[SIMPLEBAT_SENSOR_CAPACITY]);

	strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_PRESENT].desc, "battery present",
	    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_PRESENT].desc));
	sc->sc_sens[SIMPLEBAT_SENSOR_PRESENT].type = SENSOR_INDICATOR;
	sc->sc_sens[SIMPLEBAT_SENSOR_PRESENT].flags = SENSOR_FINVALID;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[SIMPLEBAT_SENSOR_PRESENT]);

	strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc, "battery status",
	    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc));
	sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].type = SENSOR_INTEGER;
	sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].flags = SENSOR_FINVALID;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[SIMPLEBAT_SENSOR_STATUS]);

	sensordev_install(&sc->sc_sensdev);
	sensor_task_register(sc, simplebat_refresh, 5);

	simplebat_refresh(sc);

#if NAPM > 0
	simplebat_sc = sc;
	apm_setinfohook(simplebat_apminfo);
#endif
}

void
simplebat_refresh(void *arg)
{
	struct simplebat_softc *sc = arg;
	int voltage, current, status, present;
	int capacity, charging;

	present = sc->sc_present(sc->sc_cookie);
	sc->sc_sens[SIMPLEBAT_SENSOR_PRESENT].value = present;
	sc->sc_sens[SIMPLEBAT_SENSOR_PRESENT].status = SENSOR_S_OK;
	sc->sc_sens[SIMPLEBAT_SENSOR_PRESENT].flags &= ~SENSOR_FINVALID;

	if (!present) {
		/* no battery, invalidate other sensors */
		sc->sc_sens[SIMPLEBAT_SENSOR_VOLTAGE].flags |= SENSOR_FINVALID;
		sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].flags |= SENSOR_FINVALID;
		sc->sc_sens[SIMPLEBAT_SENSOR_CAPACITY].flags |= SENSOR_FINVALID;
		sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].flags |= SENSOR_FINVALID;
		return;
	}

	voltage = sc->sc_voltage(sc->sc_cookie);
	if (voltage >= 0) {
		/* mV -> uV */
		sc->sc_sens[SIMPLEBAT_SENSOR_VOLTAGE].value = voltage * 1000;
		sc->sc_sens[SIMPLEBAT_SENSOR_VOLTAGE].status = SENSOR_S_OK;
		sc->sc_sens[SIMPLEBAT_SENSOR_VOLTAGE].flags &= ~SENSOR_FINVALID;

		/* estimate capacity from voltage */
		if (sc->sc_ocv_count > 0)
			capacity = simplebat_ocv_lookup(sc, voltage);
		else if (voltage <= sc->sc_mvolt_min)
			capacity = 0;
		else if (voltage >= sc->sc_mvolt_max)
			capacity = 100;
		else
			capacity = (voltage - sc->sc_mvolt_min) * 100 /
			    (sc->sc_mvolt_max - sc->sc_mvolt_min);

		sc->sc_sens[SIMPLEBAT_SENSOR_CAPACITY].value = capacity * 1000;
		sc->sc_sens[SIMPLEBAT_SENSOR_CAPACITY].status = SENSOR_S_OK;
		sc->sc_sens[SIMPLEBAT_SENSOR_CAPACITY].flags &= ~SENSOR_FINVALID;
	} else {
		sc->sc_sens[SIMPLEBAT_SENSOR_VOLTAGE].flags |= SENSOR_FINVALID;
		sc->sc_sens[SIMPLEBAT_SENSOR_CAPACITY].flags |= SENSOR_FINVALID;
	}

	status = sc->sc_status(sc->sc_cookie);
	charging = (status >= SIMPLEBAT_DEAD_CHARGE &&
	    status <= SIMPLEBAT_CHARGE_DONE);
	current = sc->sc_current(sc->sc_cookie);

	/* mA -> uA */
	sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].value = current * 1000;
	sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].status = SENSOR_S_OK;
	sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].flags &= ~SENSOR_FINVALID;

	if (charging) {
		strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].desc,
		    "battery charging",
		    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].desc));
	} else if (current < 0) {
		strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].desc,
		    "battery discharging",
		    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].desc));
		sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].value = -current * 1000;
	} else {
		strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].desc,
		    "battery current",
		    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_CURRENT].desc));
	}

	/* estimate minutes remaining when discharging */
	if (current < 0 && capacity > 0 && sc->sc_mah_full > 0)
		sc->sc_minutes_left =
		    (sc->sc_mah_full * capacity * 60) / ((-current) * 100);
	else
		sc->sc_minutes_left = -1;

	sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].value = status;
	sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].status = SENSOR_S_OK;
	sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].flags &= ~SENSOR_FINVALID;

	switch (status) {
	case SIMPLEBAT_IDLE:
		strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc,
		    "idle", sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc));
		break;
	case SIMPLEBAT_DISCHARGING:
		strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc,
		    "discharging",
		    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc));
		break;
	case SIMPLEBAT_DEAD_CHARGE:
		strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc,
		    "dead charging",
		    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc));
		break;
	case SIMPLEBAT_TRICKLE_CHARGE:
		strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc,
		    "trickle charging",
		    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc));
		break;
	case SIMPLEBAT_CHARGING:
		strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc,
		    "charging",
		    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc));
		break;
	case SIMPLEBAT_CHARGE_DONE:
		strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc,
		    "charge done",
		    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc));
		break;
	case SIMPLEBAT_FAULT:
		strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc,
		    "charging fault",
		    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc));
		break;
	default:
		strlcpy(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc,
		    "unknown",
		    sizeof(sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].desc));
		break;
	}
}

int
simplebat_ocv_lookup(struct simplebat_softc *sc, int voltage)
{
	int i, v1, v2, c1, c2;

	/* table is sorted descending by voltage */

	if (voltage >= sc->sc_ocv_table[0].voltage)
		return sc->sc_ocv_table[0].capacity;
	if (voltage <= sc->sc_ocv_table[sc->sc_ocv_count - 1].voltage)
		return sc->sc_ocv_table[sc->sc_ocv_count - 1].capacity;

	for (i = 1; i < sc->sc_ocv_count; i++) {
		if (voltage >= sc->sc_ocv_table[i].voltage) {
			v1 = sc->sc_ocv_table[i - 1].voltage;
			v2 = sc->sc_ocv_table[i].voltage;
			c1 = sc->sc_ocv_table[i - 1].capacity;
			c2 = sc->sc_ocv_table[i].capacity;
			if (v1 == v2)
				return c1;
			return c2 + (voltage - v2) * (c1 - c2) / (v1 - v2);
		}
	}

	return 0;
}

#if NAPM > 0
int
simplebat_apminfo(struct apm_power_info *info)
{
	struct simplebat_softc *sc = simplebat_sc;

	switch (sc->sc_sens[SIMPLEBAT_SENSOR_STATUS].value) {
	case SIMPLEBAT_TRICKLE_CHARGE:
	case SIMPLEBAT_CHARGING:
	case SIMPLEBAT_CHARGE_DONE:
		info->ac_state = APM_AC_ON;
		break;
	default:
		info->ac_state = APM_AC_OFF;
	}

	if (sc->sc_sens[SIMPLEBAT_SENSOR_PRESENT].status == SENSOR_S_OK) {
		info->battery_life =
		    sc->sc_sens[SIMPLEBAT_SENSOR_CAPACITY].value / 1000;

		if (info->battery_life > 50)
			info->battery_state = APM_BATT_HIGH;
		else if (info->battery_life > 25)
			info->battery_state = APM_BATT_LOW;
		else
			info->battery_state = APM_BATT_CRITICAL;

		info->minutes_left = sc->sc_minutes_left;
	} else {
		info->battery_life = 0;
		info->battery_state = APM_BATT_UNKNOWN;
		info->minutes_left = -1;
	}

	return 0;
}
#endif
