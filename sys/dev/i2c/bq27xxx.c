/* $OpenBSD$ */
/*
 * TI BQ27Z561
 * https://www.ti.com/lit/ug/sluubo7/sluubo7.pdf
 * Copyright (c) 2025 joshua stein <jcs@jcs.org>
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
#include <sys/types.h>
#include <sys/timeout.h>
#include <sys/malloc.h>
#include <sys/sensors.h>

#include <dev/i2c/i2cvar.h>

#include <machine/intr.h>
#include <machine/bus.h>
#include <machine/fdt.h>
#include <machine/apmvar.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_misc.h>
#include <dev/ofw/ofw_gpio.h>
#include <dev/ofw/fdt.h>

#include "apm.h"

#define CMD_CONTROL_STATUS		0x00
#define CMD_AT_RATE			0x02
#define CMD_AT_RATE_TIME_TO_EMPTY	0x04
#define CMD_TEMPERATURE			0x06
#define CMD_VOLTAGE			0x08
#define CMD_BATTERY_STATUS		0x0a
#define  BATTERY_STATUS_DSG			(1 << 0)
#define  BATTERY_STATUS_FD			(1 << 4)
#define  BATTERY_STATUS_FC			(1 << 5)
#define CMD_CURRENT			0x0c
#define CMD_REMAINING_CAPACITY		0x10
#define CMD_FULL_CHARGE_CAPACITY	0x12
#define CMD_AVERAGE_CURRENT		0x14
#define CMD_AVERAGE_TIME_TO_EMPTY	0x16
#define CMD_AVERAGE_TIME_TO_FULL	0x18
#define CMD_MAX_LOAD_CURRENT		0x1e
#define CMD_MAX_LOAD_TIME_TO_EMPTY	0x20
#define CMD_AVERAGE_POWER		0x22
#define CMD_BTP_DISCHARGE_SET		0x24
#define CMD_BTP_CHARGE_SET		0x26
#define CMD_INTERNAL_TEMPERATURE	0x28
#define CMD_CYCLE_COUNT			0x2a
#define CMD_RELATIVE_STATE_OF_CHARGE	0x2c
#define CMD_STATE_OF_HEALTH		0x2e
#define CMD_CHARGING_VOLTAGE		0x30
#define CMD_CHARGING_CURRENT		0x32
#define CMD_DESIGN_CAPACITY		0x3c

struct bq27xxx_softc {
	struct device sc_dev;
	i2c_tag_t sc_tag;
	i2c_addr_t sc_addr;
	uint32_t sc_gpio[4];

	struct ksensordev sc_sensdev;
	struct ksensor sc_sens[10];
};

struct bq27xxx_softc *bq27xxx_sc;

int	bq27xxx_match(struct device *, void *, void *);
void	bq27xxx_attach(struct device *, struct device *, void *);
void	bq27xxx_refresh(void *);
#if NAPM > 0
int	bq27xxx_apminfo(struct apm_power_info *);
#endif

const struct cfattach bqbat_ca = {
	sizeof(struct bq27xxx_softc), bq27xxx_match, bq27xxx_attach
};

struct cfdriver bqbat_cd = {
	NULL, "bqbat", DV_TTY
};

int	bq27xxx_reg_read(struct bq27xxx_softc *, uint8_t, uint16_t *);
void	bq27xxx_add_sensors(struct bq27xxx_softc *);
void	bq27xxx_refresh(void *);

int
bq27xxx_match(struct device *parent, void *match, void *aux)
{
	struct i2c_attach_args *ia = aux;

	if (strcmp(ia->ia_name, "ti,bq27z561") == 0)
		return 1;

	return 0;
}

void
bq27xxx_attach(struct device *parent, struct device *self, void *aux)
{
	struct bq27xxx_softc *sc = (struct bq27xxx_softc *)self;
	struct i2c_attach_args *ia = aux;
	int16_t val;

	sc->sc_tag = ia->ia_tag;
	sc->sc_addr = ia->ia_addr;

	bq27xxx_add_sensors(sc);

	if (bq27xxx_reg_read(sc, CMD_VOLTAGE, &val) == -1) {
		printf(": no battery present\n");
		return;
	}

	if (sensor_task_register(sc, bq27xxx_refresh, 5) == NULL) {
		printf(": unable to register update task\n");
		return;
	}

	bq27xxx_refresh(sc);

#if NAPM > 0
	bq27xxx_sc = sc;
	apm_setinfohook(bq27xxx_apminfo);
#endif

	printf("\n");
}

int
bq27xxx_reg_read(struct bq27xxx_softc *sc, uint8_t reg, uint16_t *ret)
{
	int error;

	iic_acquire_bus(sc->sc_tag, I2C_F_POLL);
	error = iic_exec(sc->sc_tag, I2C_OP_READ_WITH_STOP, sc->sc_addr,
	    &reg, sizeof(reg), ret, sizeof(*ret), I2C_F_POLL);
	iic_release_bus(sc->sc_tag, I2C_F_POLL);

	return (error ? -1 : 0);
}

void
bq27xxx_add_sensors(struct bq27xxx_softc *sc)
{
	strlcpy(sc->sc_sensdev.xname, sc->sc_dev.dv_xname,
	    sizeof(sc->sc_sensdev.xname));

	strlcpy(sc->sc_sens[0].desc, "last full capacity",
	    sizeof(sc->sc_sens[0].desc));
	sc->sc_sens[0].type = SENSOR_AMPHOUR;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[0]);

	strlcpy(sc->sc_sens[1].desc, "voltage", sizeof(sc->sc_sens[1].desc));
	sc->sc_sens[1].type = SENSOR_VOLTS_DC;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[1]);

	strlcpy(sc->sc_sens[2].desc, "minutes until empty",
	    sizeof(sc->sc_sens[2].desc));
	sc->sc_sens[2].type = SENSOR_INTEGER;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[2]);

	strlcpy(sc->sc_sens[3].desc, "minutes until fully charged",
	    sizeof(sc->sc_sens[3].desc));
	sc->sc_sens[3].type = SENSOR_INTEGER;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[3]);

	strlcpy(sc->sc_sens[4].desc, "rate", sizeof(sc->sc_sens[3].desc));
	sc->sc_sens[4].type = SENSOR_WATTS;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[4]);

	strlcpy(sc->sc_sens[5].desc, "remaining capacity",
	    sizeof(sc->sc_sens[5].desc));
	sc->sc_sens[5].type = SENSOR_AMPHOUR;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[5]);

	strlcpy(sc->sc_sens[6].desc, "current",
	    sizeof(sc->sc_sens[6].desc));
	sc->sc_sens[6].type = SENSOR_AMPS;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[6]);

	strlcpy(sc->sc_sens[7].desc, "design capacity",
	    sizeof(sc->sc_sens[7].desc));
	sc->sc_sens[7].type = SENSOR_AMPHOUR;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[7]);

	strlcpy(sc->sc_sens[8].desc, "discharge cycles",
	    sizeof(sc->sc_sens[8].desc));
	sc->sc_sens[8].type = SENSOR_INTEGER;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[8]);

	strlcpy(sc->sc_sens[9].desc, "temperature",
	    sizeof(sc->sc_sens[9].desc));
	sc->sc_sens[9].type = SENSOR_TEMP;
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[9]);

	sensordev_install(&sc->sc_sensdev);
}

void
bq27xxx_refresh(void *arg)
{
	struct bq27xxx_softc *sc = arg;
	int16_t val;

	if (bq27xxx_reg_read(sc, CMD_FULL_CHARGE_CAPACITY, &val) == 0) {
		/* mAh -> uAh */
		sc->sc_sens[0].value = val * 1000;
		sc->sc_sens[0].status = SENSOR_S_OK;
		sc->sc_sens[0].flags = 0;
	} else {
		sc->sc_sens[0].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[0].flags = SENSOR_FUNKNOWN;
	}

	if (bq27xxx_reg_read(sc, CMD_VOLTAGE, &val) == 0) {
		/* mV -> uV */
		sc->sc_sens[1].value = val * 1000;
		sc->sc_sens[1].status = SENSOR_S_OK;
		sc->sc_sens[1].flags = 0;
	} else {
		sc->sc_sens[1].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[1].flags = SENSOR_FUNKNOWN;
	}

	if (bq27xxx_reg_read(sc, CMD_AVERAGE_TIME_TO_EMPTY, &val) == 0) {
		/* mins */
		if (val < 0)
			val = 0;
		sc->sc_sens[2].value = val;
		sc->sc_sens[2].status = SENSOR_S_OK;
		sc->sc_sens[2].flags = 0;
	} else {
		sc->sc_sens[2].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[2].flags = SENSOR_FUNKNOWN;
	}

	if (bq27xxx_reg_read(sc, CMD_AVERAGE_TIME_TO_FULL, &val) == 0) {
		/* mins */
		if (val < 0)
			val = 0;
		sc->sc_sens[3].value = val;
		sc->sc_sens[3].status = SENSOR_S_OK;
		sc->sc_sens[3].flags = 0;
	} else {
		sc->sc_sens[3].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[3].flags = SENSOR_FUNKNOWN;
	}

	if (bq27xxx_reg_read(sc, CMD_AVERAGE_POWER, &val) == 0) {
		/* 10 mW -> uW */
		sc->sc_sens[4].value = (val < 0 ? -val : val) * 10000;
		sc->sc_sens[4].status = SENSOR_S_OK;
		sc->sc_sens[4].flags = 0;

		if (val > 0) {
			strlcpy(sc->sc_sens[4].desc, "battery charging",
			    sizeof(sc->sc_sens[4].desc));
		} else if (val < 0) {
			strlcpy(sc->sc_sens[4].desc, "battery discharging",
			    sizeof(sc->sc_sens[4].desc));
		} else {
			bq27xxx_reg_read(sc, CMD_BATTERY_STATUS, &val);

			if (val & BATTERY_STATUS_FD) {
				strlcpy(sc->sc_sens[4].desc, "battery empty",
				    sizeof(sc->sc_sens[4].desc));
			} else if (val & BATTERY_STATUS_FC) {
				strlcpy(sc->sc_sens[4].desc, "battery charged",
				    sizeof(sc->sc_sens[4].desc));
			} else {
				strlcpy(sc->sc_sens[4].desc, "battery idle",
				    sizeof(sc->sc_sens[4].desc));
			}
		}
	} else {
		sc->sc_sens[4].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[4].flags = SENSOR_FUNKNOWN;
		strlcpy(sc->sc_sens[4].desc, "unknown status",
		    sizeof(sc->sc_sens[4].desc));
	}

	if (bq27xxx_reg_read(sc, CMD_REMAINING_CAPACITY, &val) == 0) {
		/* mAh -> uAh */
		sc->sc_sens[5].value = val * 1000;
		sc->sc_sens[5].status = SENSOR_S_OK;
		sc->sc_sens[5].flags = 0;
	} else {
		sc->sc_sens[5].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[5].flags = SENSOR_FUNKNOWN;
	}

	if (bq27xxx_reg_read(sc, CMD_AVERAGE_CURRENT, &val) == 0) {
		/* mA -> uA */
		sc->sc_sens[6].value = val * 1000;
		sc->sc_sens[6].status = SENSOR_S_OK;
		sc->sc_sens[6].flags = 0;
	} else {
		sc->sc_sens[6].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[6].flags = SENSOR_FUNKNOWN;
	}

	if (bq27xxx_reg_read(sc, CMD_DESIGN_CAPACITY, &val) == 0) {
		/* mAh -> uAh */
		sc->sc_sens[7].value = val * 1000;
		sc->sc_sens[7].status = SENSOR_S_OK;
		sc->sc_sens[7].flags = 0;
	} else {
		sc->sc_sens[7].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[7].flags = SENSOR_FUNKNOWN;
	}

	if (bq27xxx_reg_read(sc, CMD_CYCLE_COUNT, &val) == 0) {
		sc->sc_sens[8].value = val;
		sc->sc_sens[8].status = SENSOR_S_OK;
		sc->sc_sens[8].flags = 0;
	} else {
		sc->sc_sens[8].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[8].flags = SENSOR_FUNKNOWN;
	}

	if (bq27xxx_reg_read(sc, CMD_TEMPERATURE, &val) == 0) {
		/* 0.1K -> uK */
		sc->sc_sens[9].value = val * 100000;
		sc->sc_sens[9].status = SENSOR_S_OK;
		sc->sc_sens[9].flags = 0;
	} else {
		sc->sc_sens[9].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[9].flags = SENSOR_FUNKNOWN;
	}
}

#if NAPM > 0
int
bq27xxx_apminfo(struct apm_power_info *info)
{
	struct bq27xxx_softc *sc = bq27xxx_sc;
	int16_t val;
	int pct;

	info->ac_state = APM_AC_UNKNOWN;
	info->battery_state = APM_BATT_UNKNOWN;
	info->battery_life = 0;
	info->minutes_left = 0;

	if (sc == NULL)
		return 0;

	if (bq27xxx_reg_read(sc, CMD_BATTERY_STATUS, (uint16_t *)&val) == 0) {
		/* DSG=0 means charger present */
		if (val & BATTERY_STATUS_DSG)
			info->ac_state = APM_AC_OFF;
		else
			info->ac_state = APM_AC_ON;
	}

	if (bq27xxx_reg_read(sc, CMD_RELATIVE_STATE_OF_CHARGE,
	    (uint16_t *)&val) == 0) {
		pct = val > 100 ? 100 : val;
		info->battery_life = pct;
		if (info->ac_state == APM_AC_ON)
			info->battery_state = APM_BATT_CHARGING;
		else if (pct > 50)
			info->battery_state = APM_BATT_HIGH;
		else if (pct > 25)
			info->battery_state = APM_BATT_LOW;
		else
			info->battery_state = APM_BATT_CRITICAL;
	}

	if (info->ac_state == APM_AC_ON) {
		if (bq27xxx_reg_read(sc, CMD_AVERAGE_TIME_TO_FULL,
		    (uint16_t *)&val) == 0)
			info->minutes_left = val;
	} else {
		if (bq27xxx_reg_read(sc, CMD_AVERAGE_TIME_TO_EMPTY,
		    (uint16_t *)&val) == 0)
			info->minutes_left = val;
	}

	return 0;
}
#endif
