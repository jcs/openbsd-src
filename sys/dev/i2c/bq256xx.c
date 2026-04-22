/* $OpenBSD$ */
/*
 * TI BQ256XX
 * https://www.ti.com/lit/ds/symlink/bq25620.pdf
 * Copyright (c) 2025-2026 joshua stein <jcs@jcs.org>
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
#include <sys/task.h>
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

#define BQ256XX_CHARGE_CURRENT_LIMIT	0x2
#define  BQ256XX_CHARGE_CURRENT_LIMIT_ICHG	0xfc0
#define  BQ256XX_CHARGE_CURRENT_LIMIT_ICHG_SHIFT 6

#define BQ256XX_CHARGE_VOLTAGE_LIMIT	0x4
#define  BQ256XX_CHARGE_VOLTAGE_LIMIT_VREG	0xff8
#define  BQ256XX_CHARGE_VOLTAGE_LIMIT_VREG_SHIFT 3

#define BQ256XX_INPUT_CURRENT_LIMIT	0x6
#define  BQ256XX_INPUT_CURRENT_LIMIT_IINDPM	0xff0
#define  BQ256XX_INPUT_CURRENT_LIMIT_IINDPM_SHIFT 4

#define BQ256XX_PRE_CHARGE_CONTROL	0x10
#define  BQ256XX_PRE_CHARGE_CONTROL_IPRECHG	0x1f0
#define  BQ256XX_PRE_CHARGE_CONTROL_IPRECHG_SHIFT 4

#define BQ256XX_TERMINATION_CONTROL	0x12
#define  BQ256XX_TERMINATION_CONTROL_ITERM	0x1f8
#define  BQ256XX_TERMINATION_CONTROL_ITERM_SHIFT 3

#define BQ256XX_CHARGER_CONTROL_1	0x16
#define  BQ256XX_CHARGER_CONTROL_WATCHDOG	((1 << 1) | (1 << 0))
#define  BQ256XX_CHARGER_CONTROL_WATCHDOG_0	0x0
#define  BQ256XX_CHARGER_CONTROL_WATCHDOG_50	0x1
#define  BQ256XX_CHARGER_CONTROL_WATCHDOG_100	0x2
#define  BQ256XX_CHARGER_CONTROL_WATCHDOG_200	0x3
#define  BQ256XX_CHARGER_CONTROL_WD_RST		(1 << 2)

#define BQ256XX_CHARGER_CONTROL_2	0x17
#define  BQ256XX_CHARGER_CONTROL_2_REG_RST	(1 << 7)

#define BQ256XX_CHARGER_STATUS_1	0x1e
#define  BQ256XX_CHARGER_STATUS_1_CHG_NONE	0x0
#define  BQ256XX_CHARGER_STATUS_1_CHG_TRICKLE	0x1
#define  BQ256XX_CHARGER_STATUS_1_CHG_TAPER	0x2
#define  BQ256XX_CHARGER_STATUS_1_CHG_TOPOFF	0x3
#define  BQ256XX_CHARGER_STATUS_1_VBUS_NONE	0x0
#define  BQ256XX_CHARGER_STATUS_1_VBUS_SDP	0x1
#define  BQ256XX_CHARGER_STATUS_1_VBUS_CDP	0x2
#define  BQ256XX_CHARGER_STATUS_1_VBUS_DCP	0x3
#define  BQ256XX_CHARGER_STATUS_1_VBUS_UNKNOWN	0x4
#define  BQ256XX_CHARGER_STATUS_1_VBUS_NONSTANDARD 0x5
#define  BQ256XX_CHARGER_STATUS_1_VBUS_HVDCP	0x6
#define  BQ256XX_CHARGER_STATUS_1_VBUS_OTG	0x7
#define  BQ256XX_CHARGER_STATUS_1_VBUS_SHIFT	5
#define  BQ256XX_CHARGER_STATUS_1_VBUS_MASK	0x7

#define BQ256XX_CHARGER_CONTROL_4	0x19
#define  BQ256XX_CHARGER_CONTROL_4_EN_EXT_ILIM	(1 << 2)

#define BQ256XX_CHARGER_FLAG_0		0x20
#define BQ256XX_CHARGER_FLAG_1		0x21

#define BQ256XX_NTC_CONTROL_0		0x1a
#define  BQ256XX_NTC_CONTROL_TS_IGNORE		(1 << 7)

#define BQ256XX_PART_INFO		0x38
#define  BQ256XX_PART_INFO_PN			0xfff
#define  BQ256XX_PART_INFO_PN_SHIFT		2
#define  BQ256XX_PART_INFO_PN_BQ25620		0x0
#define  BQ256XX_PART_INFO_PN_BQ25622		0x2

#define BQ256XX_ADC_CONTROL		0x26
#define  BQ256XX_ADC_CONTROL_ADC_EN		(1 << 7)

#define BQ256XX_IBUS_ADC		0x28
#define  BQ256XX_IBUS_ADC_STEP_UA		2000
#define BQ256XX_IBAT_ADC		0x2a
#define  BQ256XX_IBAT_ADC_STEP_UA		4000
#define BQ256XX_VBUS_ADC		0x2c
#define  BQ256XX_VBUS_ADC_STEP_UV		3970
#define BQ256XX_VBAT_ADC		0x30
#define  BQ256XX_VBAT_ADC_STEP_UV		1990

#define BQ256XX_SENS_VBUS		0
#define BQ256XX_SENS_IBUS		1
#define BQ256XX_SENS_VBAT		2
#define BQ256XX_SENS_IBAT		3
#define BQ256XX_NUM_SENS		4

struct bq256xx_softc {
	struct device sc_dev;
	i2c_tag_t sc_tag;
	i2c_addr_t sc_addr;
	int sc_node;
	int sc_part;
	int sc_watchdog;
	uint16_t sc_iindpm;
	uint16_t sc_ichg;
	uint16_t sc_vreg;
	uint16_t sc_iprechg;
	uint16_t sc_iterm;
	uint8_t sc_chg_ctrl;
	uint8_t sc_ntc_ctrl;
	uint32_t *sc_gpio;
	void *sc_ih;
	struct task sc_intr_task;

	struct ksensordev sc_sensdev;
	struct ksensor sc_sens[BQ256XX_NUM_SENS];
};

int	bq256xx_match(struct device *, void *, void *);
void	bq256xx_attach(struct device *, struct device *, void *);
void	bq256xx_refresh(void *);

const struct cfattach bqcharger_ca = {
	sizeof(struct bq256xx_softc), bq256xx_match, bq256xx_attach
};

struct cfdriver bqcharger_cd = {
	NULL, "bqcharger", DV_DULL
};

int	bq256xx_reg_read(struct bq256xx_softc *, uint8_t, int8_t *);
int	bq256xx_reg_read_2(struct bq256xx_softc *, uint8_t, uint16_t *);
int	bq256xx_reg_write(struct bq256xx_softc *, uint8_t, int8_t);
int	bq256xx_reg_write_2(struct bq256xx_softc *, uint8_t, uint16_t);
int	bq256xx_intr(void *);
void	bq256xx_intr_task(void *);
void	bq256xx_update_power(struct bq256xx_softc *);
void	bq256xx_add_sensors(struct bq256xx_softc *);
void	bq256xx_refresh(void *);

int
bq256xx_match(struct device *parent, void *match, void *aux)
{
	struct i2c_attach_args *ia = aux;

	if (strcmp(ia->ia_name, "ti,bq25620") == 0)
		return 1;

	return 0;
}

void
bq256xx_attach(struct device *parent, struct device *self, void *aux)
{
	struct bq256xx_softc *sc = (struct bq256xx_softc *)self;
	struct i2c_attach_args *ia = aux;
	int32_t ival;
	uint32_t uival;
	uint16_t usval;
	int8_t val;

	sc->sc_node = *(int *)ia->ia_cookie;
	sc->sc_tag = ia->ia_tag;
	sc->sc_addr = ia->ia_addr;

	if (bq256xx_reg_read(sc, BQ256XX_PART_INFO, &val) == -1) {
		printf(": failed reading version\n");
		return;
	}

	sc->sc_part = (val >> BQ256XX_PART_INFO_PN_SHIFT) &
	    BQ256XX_PART_INFO_PN;
	switch (sc->sc_part) {
	case BQ256XX_PART_INFO_PN_BQ25620:
		printf(": BQ25620");
		break;
	case BQ256XX_PART_INFO_PN_BQ25622:
		printf(": BQ25622");
		break;
	default:
		printf(": unknown device (0x%x)\n", val);
		return;
	}

	/* configure or disable watchdog */
	ival = OF_getpropint(sc->sc_node, "ti,watchdog-timeout-ms", -1);
	switch (ival) {
	case -1:
	case 0:
	case 50:
	case 100:
	case 200:
		break;
	default:
		/* unsupported timeout, use default of 50s */
		ival = 50;
	}
	if (ival != -1) {
		bq256xx_reg_read(sc, BQ256XX_CHARGER_CONTROL_1, &val);
		val &= ~(BQ256XX_CHARGER_CONTROL_WATCHDOG);
		switch (ival) {
		case 50:
			val |= BQ256XX_CHARGER_CONTROL_WATCHDOG_50;
			break;
		case 100:
			val |= BQ256XX_CHARGER_CONTROL_WATCHDOG_100;
			break;
		case 200:
			val |= BQ256XX_CHARGER_CONTROL_WATCHDOG_200;
			break;
		}

		sc->sc_watchdog = ival;
		sc->sc_chg_ctrl = val;

		bq256xx_reg_write(sc, BQ256XX_CHARGER_CONTROL_1, val);
	}

	if (OF_getpropint(sc->sc_node, "ti,no-thermistor", 0) == 1) {
		bq256xx_reg_read(sc, BQ256XX_NTC_CONTROL_0, &val);
		val |= BQ256XX_NTC_CONTROL_TS_IGNORE;
		sc->sc_ntc_ctrl = val;
		bq256xx_reg_write(sc, BQ256XX_NTC_CONTROL_0, val);
	}

	uival = OF_getpropint(sc->sc_node, "charge-current-limit-microamp", 0);
	if (uival != 0) {
		uival /= (1000 * 80);
		bq256xx_reg_read_2(sc, BQ256XX_CHARGE_CURRENT_LIMIT, &usval);
		usval &= ~(BQ256XX_CHARGE_CURRENT_LIMIT_ICHG);
		usval |= (uival << BQ256XX_CHARGE_CURRENT_LIMIT_ICHG_SHIFT);
		sc->sc_ichg = usval;
		bq256xx_reg_write_2(sc, BQ256XX_CHARGE_CURRENT_LIMIT, usval);
	}

	uival = OF_getpropint(sc->sc_node, "charge-voltage-limit-microvolt", 0);
	if (uival != 0) {
		uival /= (1000 * 10);
		bq256xx_reg_read_2(sc, BQ256XX_CHARGE_VOLTAGE_LIMIT, &usval);
		usval &= ~(BQ256XX_CHARGE_VOLTAGE_LIMIT_VREG);
		usval |= (uival << BQ256XX_CHARGE_VOLTAGE_LIMIT_VREG_SHIFT);
		sc->sc_vreg = usval;
		bq256xx_reg_write_2(sc, BQ256XX_CHARGE_VOLTAGE_LIMIT, usval);
	}

	uival = OF_getpropint(sc->sc_node, "input-current-limit-microamp", 0);
	if (uival != 0) {
		uival /= (1000 * 20);
		bq256xx_reg_read_2(sc, BQ256XX_INPUT_CURRENT_LIMIT, &usval);
		usval &= ~(BQ256XX_INPUT_CURRENT_LIMIT_IINDPM);
		usval |= (uival << BQ256XX_INPUT_CURRENT_LIMIT_IINDPM_SHIFT);
		sc->sc_iindpm = usval;
		bq256xx_reg_write_2(sc, BQ256XX_INPUT_CURRENT_LIMIT, usval);
	}

	uival = OF_getpropint(sc->sc_node, "pre-charge-control-microamp", 0);
	if (uival != 0) {
		uival /= (1000 * 20);
		bq256xx_reg_read_2(sc, BQ256XX_PRE_CHARGE_CONTROL, &usval);
		usval &= ~(BQ256XX_PRE_CHARGE_CONTROL_IPRECHG);
		usval |= (uival << BQ256XX_PRE_CHARGE_CONTROL_IPRECHG_SHIFT);
		sc->sc_iprechg = usval;
		bq256xx_reg_write_2(sc, BQ256XX_PRE_CHARGE_CONTROL, usval);
	}

	uival = OF_getpropint(sc->sc_node, "termination-control-microamp", 0);
	if (uival != 0) {
		uival /= (1000 * 10);
		bq256xx_reg_read_2(sc, BQ256XX_TERMINATION_CONTROL, &usval);
		usval &= ~(BQ256XX_TERMINATION_CONTROL_ITERM);
		usval |= (uival << BQ256XX_TERMINATION_CONTROL_ITERM_SHIFT);
		sc->sc_iterm = usval;
		bq256xx_reg_write_2(sc, BQ256XX_TERMINATION_CONTROL, usval);
	}

	if (sc->sc_part == BQ256XX_PART_INFO_PN_BQ25622) {
		/* disable ILIM pin, use register IINDPM only */
		bq256xx_reg_read(sc, BQ256XX_CHARGER_CONTROL_4, &val);
		if (val & BQ256XX_CHARGER_CONTROL_4_EN_EXT_ILIM) {
			val &= ~BQ256XX_CHARGER_CONTROL_4_EN_EXT_ILIM;
			bq256xx_reg_write(sc, BQ256XX_CHARGER_CONTROL_4, val);
		}
	}

	/* enable continuous ADC, 12-bit */
	bq256xx_reg_write(sc, BQ256XX_ADC_CONTROL, BQ256XX_ADC_CONTROL_ADC_EN);

	/* setup interrupt from gpios property */
	ival = OF_getproplen(sc->sc_node, "gpios");
	if (ival > 0) {
		task_set(&sc->sc_intr_task, bq256xx_intr_task, sc);
		sc->sc_gpio = malloc(ival, M_DEVBUF, M_WAITOK);
		OF_getpropintarray(sc->sc_node, "gpios", sc->sc_gpio, ival);
		gpio_controller_config_pin(sc->sc_gpio, GPIO_CONFIG_INPUT);
		sc->sc_ih = gpio_controller_intr_establish(sc->sc_gpio,
		    IPL_BIO, NULL, bq256xx_intr, sc, sc->sc_dev.dv_xname);
	}

	bq256xx_add_sensors(sc);

	if (sensor_task_register(sc, bq256xx_refresh, 30) == NULL) {
		printf(": unable to register update task\n");
		return;
	}

	printf("\n");
}

int
bq256xx_reg_read(struct bq256xx_softc *sc, uint8_t reg, int8_t *ret)
{
	int error;

	iic_acquire_bus(sc->sc_tag, I2C_F_POLL);
	error = iic_exec(sc->sc_tag, I2C_OP_READ_WITH_STOP, sc->sc_addr,
	    &reg, sizeof(reg), ret, sizeof(*ret), I2C_F_POLL);
	iic_release_bus(sc->sc_tag, I2C_F_POLL);

	return (error ? -1 : 0);
}

int
bq256xx_reg_read_2(struct bq256xx_softc *sc, uint8_t reg, uint16_t *ret)
{
	int error;

	iic_acquire_bus(sc->sc_tag, I2C_F_POLL);
	error = iic_exec(sc->sc_tag, I2C_OP_READ_WITH_STOP, sc->sc_addr,
	    &reg, sizeof(reg), ret, sizeof(*ret), I2C_F_POLL);
	iic_release_bus(sc->sc_tag, I2C_F_POLL);

	return (error ? -1 : 0);
}

int
bq256xx_reg_write(struct bq256xx_softc *sc, uint8_t reg, int8_t val)
{
	int error;

	iic_acquire_bus(sc->sc_tag, I2C_F_POLL);
	error = iic_exec(sc->sc_tag, I2C_OP_WRITE_WITH_STOP, sc->sc_addr,
	    &reg, sizeof(reg), &val, sizeof(val), I2C_F_POLL);
	iic_release_bus(sc->sc_tag, I2C_F_POLL);

	return (error ? -1 : 0);
}

int
bq256xx_reg_write_2(struct bq256xx_softc *sc, uint8_t reg, uint16_t val)
{
	int error;

	iic_acquire_bus(sc->sc_tag, I2C_F_POLL);
	error = iic_exec(sc->sc_tag, I2C_OP_WRITE_WITH_STOP, sc->sc_addr,
	    &reg, sizeof(reg), &val, sizeof(val), I2C_F_POLL);
	iic_release_bus(sc->sc_tag, I2C_F_POLL);

	return (error ? -1 : 0);
}

int
bq256xx_intr(void *arg)
{
	struct bq256xx_softc *sc = arg;

	if (!task_pending(&sc->sc_intr_task))
		task_add(systq, &sc->sc_intr_task);

	return 1;
}

void
bq256xx_intr_task(void *arg)
{
	struct bq256xx_softc *sc = arg;
	int8_t val;

	/* wait for other interrupts to cancel since a few come quickly */
	tsleep_nsec(&sc->sc_intr_task, PWAIT, "bqintr", MSEC_TO_NSEC(100));

	/* clear interrupt */
	bq256xx_reg_read(sc, BQ256XX_CHARGER_FLAG_0, &val);
	bq256xx_reg_read(sc, BQ256XX_CHARGER_FLAG_1, &val);

	/* re-apply charge parameters in case of adapter plug-in or reset */
	if (sc->sc_chg_ctrl != 0)
		bq256xx_reg_write(sc, BQ256XX_CHARGER_CONTROL_1,
		    sc->sc_chg_ctrl);
	if (sc->sc_ntc_ctrl != 0)
		bq256xx_reg_write(sc, BQ256XX_NTC_CONTROL_0,
		    sc->sc_ntc_ctrl);
	if (sc->sc_ichg != 0)
		bq256xx_reg_write_2(sc, BQ256XX_CHARGE_CURRENT_LIMIT,
		    sc->sc_ichg);
	if (sc->sc_vreg != 0)
		bq256xx_reg_write_2(sc, BQ256XX_CHARGE_VOLTAGE_LIMIT,
		    sc->sc_vreg);
	if (sc->sc_iindpm != 0)
		bq256xx_reg_write_2(sc, BQ256XX_INPUT_CURRENT_LIMIT,
		    sc->sc_iindpm);
	if (sc->sc_iprechg != 0)
		bq256xx_reg_write_2(sc, BQ256XX_PRE_CHARGE_CONTROL,
		    sc->sc_iprechg);
	if (sc->sc_iterm != 0)
		bq256xx_reg_write_2(sc, BQ256XX_TERMINATION_CONTROL,
		    sc->sc_iterm);

	bq256xx_update_power(sc);
}

void
bq256xx_update_power(struct bq256xx_softc *sc)
{
	extern int hw_power;
	int8_t val;

	if (bq256xx_reg_read(sc, BQ256XX_CHARGER_STATUS_1, &val) != 0)
		return;

	hw_power = ((val >> BQ256XX_CHARGER_STATUS_1_VBUS_SHIFT) &
	    BQ256XX_CHARGER_STATUS_1_VBUS_MASK) !=
	    BQ256XX_CHARGER_STATUS_1_VBUS_NONE;
}

void
bq256xx_add_sensors(struct bq256xx_softc *sc)
{
	strlcpy(sc->sc_sensdev.xname, sc->sc_dev.dv_xname,
	    sizeof(sc->sc_sensdev.xname));

	sc->sc_sens[BQ256XX_SENS_VBUS].type = SENSOR_VOLTS_DC;
	strlcpy(sc->sc_sens[BQ256XX_SENS_VBUS].desc, "input voltage",
	    sizeof(sc->sc_sens[BQ256XX_SENS_VBUS].desc));
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[BQ256XX_SENS_VBUS]);

	sc->sc_sens[BQ256XX_SENS_IBUS].type = SENSOR_AMPS;
	strlcpy(sc->sc_sens[BQ256XX_SENS_IBUS].desc, "input current",
	    sizeof(sc->sc_sens[BQ256XX_SENS_IBUS].desc));
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[BQ256XX_SENS_IBUS]);

	sc->sc_sens[BQ256XX_SENS_VBAT].type = SENSOR_VOLTS_DC;
	strlcpy(sc->sc_sens[BQ256XX_SENS_VBAT].desc, "battery voltage",
	    sizeof(sc->sc_sens[BQ256XX_SENS_VBAT].desc));
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[BQ256XX_SENS_VBAT]);

	sc->sc_sens[BQ256XX_SENS_IBAT].type = SENSOR_AMPS;
	strlcpy(sc->sc_sens[BQ256XX_SENS_IBAT].desc, "battery current",
	    sizeof(sc->sc_sens[BQ256XX_SENS_IBAT].desc));
	sensor_attach(&sc->sc_sensdev, &sc->sc_sens[BQ256XX_SENS_IBAT]);

	sensordev_install(&sc->sc_sensdev);
}

void
bq256xx_refresh(void *arg)
{
	struct bq256xx_softc *sc = arg;
	uint16_t val;
	uint8_t bval;

	if (sc->sc_watchdog) {
		/* feed watchdog */
		if (bq256xx_reg_read(sc, BQ256XX_CHARGER_CONTROL_1,
		    &bval) == 0) {
			bval |= BQ256XX_CHARGER_CONTROL_WD_RST;
			bq256xx_reg_write(sc, BQ256XX_CHARGER_CONTROL_1, bval);
		}
	}

	bq256xx_update_power(sc);

	/* IINDPM resets to POR on adapter plug-in, re-apply if changed */
	if (sc->sc_iindpm != 0) {
		if (bq256xx_reg_read_2(sc, BQ256XX_INPUT_CURRENT_LIMIT,
		    &val) == 0 && val != sc->sc_iindpm)
			bq256xx_reg_write_2(sc, BQ256XX_INPUT_CURRENT_LIMIT,
			    sc->sc_iindpm);
	}

	/* VBUS: bits [14:2], unsigned */
	if (bq256xx_reg_read_2(sc, BQ256XX_VBUS_ADC, &val) == 0) {
		sc->sc_sens[BQ256XX_SENS_VBUS].value =
		    (int64_t)((val >> 2) & 0x1fff) * BQ256XX_VBUS_ADC_STEP_UV;
		sc->sc_sens[BQ256XX_SENS_VBUS].status = SENSOR_S_OK;
		sc->sc_sens[BQ256XX_SENS_VBUS].flags = 0;
	} else {
		sc->sc_sens[BQ256XX_SENS_VBUS].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[BQ256XX_SENS_VBUS].flags = SENSOR_FUNKNOWN;
	}

	/* IBUS: bits [15:1], 2's complement */
	if (bq256xx_reg_read_2(sc, BQ256XX_IBUS_ADC, &val) == 0) {
		sc->sc_sens[BQ256XX_SENS_IBUS].value =
		    (int64_t)((int16_t)val >> 1) * BQ256XX_IBUS_ADC_STEP_UA;
		sc->sc_sens[BQ256XX_SENS_IBUS].status = SENSOR_S_OK;
		sc->sc_sens[BQ256XX_SENS_IBUS].flags = 0;
	} else {
		sc->sc_sens[BQ256XX_SENS_IBUS].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[BQ256XX_SENS_IBUS].flags = SENSOR_FUNKNOWN;
	}

	/* VBAT: bits [12:1], unsigned */
	if (bq256xx_reg_read_2(sc, BQ256XX_VBAT_ADC, &val) == 0) {
		sc->sc_sens[BQ256XX_SENS_VBAT].value =
		    (int64_t)((val >> 1) & 0xfff) * BQ256XX_VBAT_ADC_STEP_UV;
		sc->sc_sens[BQ256XX_SENS_VBAT].status = SENSOR_S_OK;
		sc->sc_sens[BQ256XX_SENS_VBAT].flags = 0;
	} else {
		sc->sc_sens[BQ256XX_SENS_VBAT].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[BQ256XX_SENS_VBAT].flags = SENSOR_FUNKNOWN;
	}

	/* IBAT: bits [15:2], 2's complement, step 4mA */
	if (bq256xx_reg_read_2(sc, BQ256XX_IBAT_ADC, &val) == 0) {
		sc->sc_sens[BQ256XX_SENS_IBAT].value =
		    (int64_t)((int16_t)val >> 2) * BQ256XX_IBAT_ADC_STEP_UA;
		sc->sc_sens[BQ256XX_SENS_IBAT].status = SENSOR_S_OK;
		sc->sc_sens[BQ256XX_SENS_IBAT].flags = 0;
	} else {
		sc->sc_sens[BQ256XX_SENS_IBAT].status = SENSOR_S_UNKNOWN;
		sc->sc_sens[BQ256XX_SENS_IBAT].flags = SENSOR_FUNKNOWN;
	}
}
