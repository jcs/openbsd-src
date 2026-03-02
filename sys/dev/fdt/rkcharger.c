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

#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/fdt.h>

#include "rkpmicvar.h"
#include "simplebatvar.h"

#define RK818_VB_MON_REG	0x21
#define RK818_THERMAL_REG	0x22
#define RK818_USB_CTRL_REG	0xA1
#define  RK818_USB_ILIM_MASK		0x0f
#define  RK818_USB_ILIM_450MA		0x00
#define  RK818_USB_ILIM_800MA		0x01
#define  RK818_USB_ILIM_850MA		0x02
#define  RK818_USB_ILIM_1000MA		0x03
#define  RK818_USB_ILIM_1250MA		0x04
#define  RK818_USB_ILIM_1500MA		0x05
#define  RK818_USB_ILIM_1750MA		0x06
#define  RK818_USB_ILIM_2000MA		0x07
#define  RK818_USB_ILIM_2250MA		0x08
#define  RK818_USB_ILIM_2500MA		0x09
#define  RK818_USB_ILIM_2750MA		0x0a
#define  RK818_USB_ILIM_3000MA		0x0b
#define  RK818_USB_VLIM_MASK		0x70
#define  RK818_USB_VLIM_4000MV		(0 << 4)
#define  RK818_USB_VLIM_4100MV		(1 << 4)
#define  RK818_USB_VLIM_4200MV		(2 << 4)
#define  RK818_USB_VLIM_4300MV		(3 << 4)
#define  RK818_USB_VLIM_4400MV		(4 << 4)
#define  RK818_USB_VLIM_4500MV		(5 << 4)
#define  RK818_USB_VLIM_4600MV		(6 << 4)
#define  RK818_USB_VLIM_4700MV		(7 << 4)
#define RK818_CHRG_CTRL_REG1	0xA3
#define  RK818_CHRG_CTRL_CHRG_EN	(1 << 7)
#define  RK818_CHRG_CTRL_VOLT_MASK	0x70
#define  RK818_CHRG_CTRL_VOLT_4050	0x00
#define  RK818_CHRG_CTRL_VOLT_4100	0x10
#define  RK818_CHRG_CTRL_VOLT_4150	0x20
#define  RK818_CHRG_CTRL_VOLT_4200	0x30
#define  RK818_CHRG_CTRL_VOLT_4250	0x40
#define  RK818_CHRG_CTRL_VOLT_4300	0x50
#define  RK818_CHRG_CTRL_VOLT_4350	0x60
#define  RK818_CHRG_CTRL_CUR_MASK	0x0f
#define RK818_CHRG_CTRL_REG2	0xA4
#define  RK818_CHRG_TERM_ANA		(0 << 5)
#define  RK818_CHRG_TERM_DIG		(1 << 5)
#define  RK818_CHRG_TERM_100MA		(0 << 6)
#define  RK818_CHRG_TERM_150MA		(1 << 6)
#define  RK818_CHRG_TERM_200MA		(2 << 6)
#define  RK818_CHRG_TERM_250MA		(3 << 6)
#define  RK818_CHRG_TERM_MASK		(3 << 6)
#define RK818_CHRG_CTRL_REG3	0xA5
#define  RK818_CHRG_TRICKLE_EN		(1 << 0)
#define  RK818_CHRG_TRICKLE_TIME_MASK	(3 << 1)
#define  RK818_CHRG_TRICKLE_30MIN	(0 << 1)
#define  RK818_CHRG_TRICKLE_60MIN	(1 << 1)
#define  RK818_CHRG_TRICKLE_90MIN	(2 << 1)
#define  RK818_CHRG_TRICKLE_120MIN	(3 << 1)
#define  RK818_CHRG_CCCV_EN		(1 << 3)
#define  RK818_CHRG_CCCV_TIME_MASK	(3 << 4)
#define  RK818_CHRG_CCCV_4H		(0 << 4)
#define  RK818_CHRG_CCCV_5H		(1 << 4)
#define  RK818_CHRG_CCCV_6H		(2 << 4)
#define  RK818_CHRG_CCCV_8H		(3 << 4)
#define RK818_SUP_STS_REG	0xA0
#define  RK818_SUP_STS_USB_EXS		(1 << 1)
#define  RK818_SUP_STS_USB_EFF		(1 << 2)
#define  RK818_SUP_STS_CHRG_STATUS	0x70
#define RK818_GGCON_REG		0xB0
#define  RK818_GGCON_GG_EN		(1 << 0)
#define  RK818_GGCON_ADC_CUR_EN		(1 << 3)
#define  RK818_GGCON_VOL_INSTANT	(1 << 4)
#define RK818_GGSTS_REG		0xB1
#define  RK818_GGSTS_BAT_EXIST		(1 << 7)
#define RK818_BAT_OCV_REGH	0xC2
#define RK818_BAT_OCV_REGL	0xC3
#define RK818_BAT_VOL_REGH	0xC4
#define RK818_BAT_VOL_REGL	0xC5
#define RK818_BAT_CUR_AVG_REGH	0xBC
#define RK818_BAT_CUR_AVG_REGL	0xBD
#define RK818_VCALIB0_REGH	0xD5
#define RK818_VCALIB0_REGL	0xD6
#define RK818_VCALIB1_REGH	0xD7
#define RK818_VCALIB1_REGL	0xD8

struct rkcharger_softc {
	struct device sc_dev;
	int sc_node;

	struct rkpmic_softc *sc_pmic;
	int sc_vcalib0;
	int sc_vcalib1;
	uint32_t sc_max_voltage;
	uint32_t sc_max_current;
	uint32_t sc_term_current;

	struct simplebat_softc *sc_bat;
};

int	rkcharger_match(struct device *, void *, void *);
void	rkcharger_attach(struct device *, struct device *, void *);

void	rkcharger_init_charger(struct rkcharger_softc *);

int	rkcharger_battery_voltage(void *);
int	rkcharger_battery_current(void *);
int	rkcharger_battery_status(void *);
int	rkcharger_battery_present(void *);

const struct cfattach rkcharger_ca = {
	sizeof(struct rkcharger_softc), rkcharger_match, rkcharger_attach
};

struct cfdriver rkcharger_cd = {
	NULL, "rkcharger", DV_DULL
};

int
rkcharger_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return OF_is_compatible(faa->fa_node, "rockchip,rk818-charger");
}

void
rkcharger_attach(struct device *parent, struct device *self, void *aux)
{
	struct rkcharger_softc *sc = (struct rkcharger_softc *)self;
	struct fdt_attach_args *faa = aux;
	struct simplebat_attach_args saa;
	uint32_t battery_noderef;
	int battery_node = 0;

	sc->sc_node = faa->fa_node;
	sc->sc_pmic = (struct rkpmic_softc *)parent;

	sc->sc_vcalib0 =
	    (rkpmic_reg_read(sc->sc_pmic, RK818_VCALIB0_REGH) << 8) |
	    rkpmic_reg_read(sc->sc_pmic, RK818_VCALIB0_REGL);
	sc->sc_vcalib1 =
	    (rkpmic_reg_read(sc->sc_pmic, RK818_VCALIB1_REGH) << 8) |
	    rkpmic_reg_read(sc->sc_pmic, RK818_VCALIB1_REGL);

	battery_noderef = OF_getpropint(sc->sc_node, "monitored-battery", 0);
	if (!battery_noderef ||
	    !(battery_node = OF_getnodebyphandle(battery_noderef))) {
		printf(": no monitored-battery\n");
		return;
	}

	printf(": ");

	sc->sc_max_voltage = OF_getpropint(battery_node,
	    "voltage-max-design-microvolt", 0);
	if (sc->sc_max_voltage == 0) {
		sc->sc_max_voltage = 4200000; /* 4.2V */
		printf("no voltage-max-design-microvolt, using %d, ",
		    sc->sc_max_voltage);
	}

	sc->sc_max_current = OF_getpropint(battery_node,
	    "constant-charge-current-max-microamp", 0);
	if (sc->sc_max_current == 0) {
		sc->sc_max_current = 1000000; /* 1A */
		printf("no constant-charge-current-max-microamp, using %d, ",
		    sc->sc_max_current);
	}

	/* default to 150mA */
	sc->sc_term_current = OF_getpropint(battery_node,
	    "charge-term-current-microamp", 150000);

	rkcharger_init_charger(sc);

	if (rkcharger_battery_present(sc)) {
		printf("%d.%dV %dmAh battery\n",
		    sc->sc_max_voltage / 1000000,
		    (sc->sc_max_voltage / 100000) % 10,
		    OF_getpropint(battery_node,
		    "charge-full-design-microamp-hours", 0) / 1000);
	} else {
		printf("no battery present\n");
	}

	/* attach simplebat to handle sensors */
	memset(&saa, 0, sizeof(saa));
	saa.sa_node = battery_node;
	saa.sa_cookie = sc;
	saa.sa_voltage = rkcharger_battery_voltage;
	saa.sa_current = rkcharger_battery_current;
	saa.sa_status = rkcharger_battery_status;
	saa.sa_present = rkcharger_battery_present;
	sc->sc_bat = (struct simplebat_softc *)config_found(self, &saa, NULL);
}

void
rkcharger_init_charger(struct rkcharger_softc *sc)
{
	uint8_t val;

	/* configure max voltage and current, enable charging */
	val = RK818_CHRG_CTRL_CHRG_EN;

	if (sc->sc_max_voltage >= 4350000)
		val |= RK818_CHRG_CTRL_VOLT_4350;
	else if (sc->sc_max_voltage >= 4300000)
		val |= RK818_CHRG_CTRL_VOLT_4300;
	else if (sc->sc_max_voltage >= 4250000)
		val |= RK818_CHRG_CTRL_VOLT_4250;
	else if (sc->sc_max_voltage >= 4200000)
		val |= RK818_CHRG_CTRL_VOLT_4200;
	else if (sc->sc_max_voltage >= 4150000)
		val |= RK818_CHRG_CTRL_VOLT_4150;
	else if (sc->sc_max_voltage >= 4100000)
		val |= RK818_CHRG_CTRL_VOLT_4100;
	else
		val |= RK818_CHRG_CTRL_VOLT_4050;

	/* charge current: 1000mA + (val * 200mA) */
	if (sc->sc_max_current >= 1000000)
		val |= ((sc->sc_max_current / 1000 - 1000) / 200) &
		    RK818_CHRG_CTRL_CUR_MASK;

	rkpmic_reg_write(sc->sc_pmic, RK818_CHRG_CTRL_REG1, val);

	/* charge termination current, use digital termination */
	val = RK818_CHRG_TERM_DIG;
	if (sc->sc_term_current >= 250000)
		val |= RK818_CHRG_TERM_250MA;
	else if (sc->sc_term_current >= 200000)
		val |= RK818_CHRG_TERM_200MA;
	else if (sc->sc_term_current >= 150000)
		val |= RK818_CHRG_TERM_150MA;
	else
		val |= RK818_CHRG_TERM_100MA;
	rkpmic_reg_write(sc->sc_pmic, RK818_CHRG_CTRL_REG2, val);

	/* enable trickle charge and CC/CV timeouts */
	val = RK818_CHRG_TRICKLE_EN | RK818_CHRG_TRICKLE_60MIN |
	    RK818_CHRG_CCCV_EN | RK818_CHRG_CCCV_5H;
	rkpmic_reg_write(sc->sc_pmic, RK818_CHRG_CTRL_REG3, val);

	/* set USB input current limit to 2A, voltage to 4.4V */
	val = RK818_USB_ILIM_2000MA | RK818_USB_VLIM_4400MV;
	rkpmic_reg_write(sc->sc_pmic, RK818_USB_CTRL_REG, val);

	/* enable gas guage and ADC for current measurement */
	val = rkpmic_reg_read(sc->sc_pmic, RK818_GGCON_REG);
	val |= RK818_GGCON_GG_EN | RK818_GGCON_ADC_CUR_EN;
	rkpmic_reg_write(sc->sc_pmic, RK818_GGCON_REG, val);

	/* update hw_power */
	rkcharger_battery_status(sc);
}

int
rkcharger_battery_voltage(void *cookie)
{
	struct rkcharger_softc *sc = cookie;
	int raw, volt_k, volt_b;

	/* during charging, this will read high but it's all we've got */
	raw = (rkpmic_reg_read(sc->sc_pmic, RK818_BAT_VOL_REGH) << 8) |
	    rkpmic_reg_read(sc->sc_pmic, RK818_BAT_VOL_REGL);

	/* two-point linear calibration (vcalib0 @ 3000mV, vcalib1 @ 4200mV) */
	if (sc->sc_vcalib1 == sc->sc_vcalib0)
		return raw;
	volt_k = (4200 - 3000) * 1000 / (sc->sc_vcalib1 - sc->sc_vcalib0);
	volt_b = 4200 * 1000 - (volt_k * sc->sc_vcalib1);
	return (volt_k * raw + volt_b) / 1000;
}

int
rkcharger_battery_current(void *cookie)
{
	struct rkcharger_softc *sc = cookie;
	int raw, ma;

	raw = (rkpmic_reg_read(sc->sc_pmic, RK818_BAT_CUR_AVG_REGH) << 8) |
	    rkpmic_reg_read(sc->sc_pmic, RK818_BAT_CUR_AVG_REGL);

	/* 12-bit signed: bit 11 is sign */
	if (raw & 0x800)
		raw -= 4096;
	/* 1506/1000 is the RK818 current ADC uA-per-LSB (from vendor kernel) */
	ma = raw * 1506 / 1000;

	return ma;
}

int
rkcharger_battery_status(void *cookie)
{
	struct rkcharger_softc *sc = cookie;
	extern int hw_power;
	uint8_t val;

	val = rkpmic_reg_read(sc->sc_pmic, RK818_SUP_STS_REG);

	/* USB present? */
	if (!(val & RK818_SUP_STS_USB_EXS)) {
		hw_power = 0;
		return SIMPLEBAT_DISCHARGING;
	}

	hw_power = 1;

	switch ((val & RK818_SUP_STS_CHRG_STATUS) >> 4) {
	case 0:
		return SIMPLEBAT_IDLE;
	case 1:
		return SIMPLEBAT_DEAD_CHARGE;
	case 2:
		return SIMPLEBAT_TRICKLE_CHARGE;
	case 3:
		return SIMPLEBAT_CHARGING;
	case 4:
		return SIMPLEBAT_CHARGE_DONE;
	case 5:
		/* USB over voltage */
		return SIMPLEBAT_FAULT;
	case 6:
		/* battery temperature fault */
		return SIMPLEBAT_FAULT;
	case 7:
		/* charging time fault */
		return SIMPLEBAT_FAULT;
	default:
		return SIMPLEBAT_UNKNOWN;
	}
}

int
rkcharger_battery_present(void *cookie)
{
	struct rkcharger_softc *sc = cookie;
	uint8_t val;

	val = rkpmic_reg_read(sc->sc_pmic, RK818_GGSTS_REG);

	if (val & RK818_GGSTS_BAT_EXIST)
		return 1;

	/* see if voltage is there */
	if (rkcharger_battery_voltage(sc) > 2500)
		return 1;

	return 0;
}
