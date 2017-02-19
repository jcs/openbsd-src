/* $OpenBSD$ */
/*
 * Smart Battery Subsystem device driver
 * ACPI 5.0 spec section 10
 *
 * Copyright (c) 2016-2017 joshua stein <jcs@openbsd.org>
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

#include <dev/acpi/acpireg.h>
#include <dev/acpi/acpivar.h>
#include <dev/acpi/acpidev.h>
#include <dev/acpi/amltypes.h>
#include <dev/acpi/dsdt.h>
#include <dev/acpi/smbus.h>

#include <sys/sensors.h>

/* #define ACPISBS_DEBUG */

#ifdef ACPISBS_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

/* how often (in seconds) to re-poll data */
#define ACPISBS_POLL_FREQ	30

/* number of polls for reading data */
#define SMBUS_TIMEOUT		50

struct acpisbs_battery {
	uint16_t mode;			/* bit flags */
	int	 units;
#define	ACPISBS_UNITS_MW 0
#define	ACPISBS_UNITS_MA 1

	uint16_t at_rate;		/* mAh or mWh */
	uint16_t temperature;		/* 0.1 degK */
	uint16_t voltage;		/* mV */
	uint16_t current;		/* mA */
	uint16_t avg_current;		/* mA */
	uint16_t rel_charge;		/* percent of last_capacity */
	uint16_t abs_charge;		/* percent of design_capacity */
	uint16_t capacity;		/* mAh */
	uint16_t full_capacity;		/* mAh, when fully charged */
	uint16_t run_time;		/* minutes */
	uint16_t avg_empty_time;	/* minutes */
	uint16_t avg_full_time;		/* minutes until full */
	uint16_t charge_current;	/* mA */
	uint16_t charge_voltage;	/* mV */
	uint16_t status;		/* bit flags */
	uint16_t cycle_count;		/* cycles */
	uint16_t design_capacity;	/* mAh */
	uint16_t design_voltage;	/* mV */
	uint16_t spec;			/* formatted */
	uint16_t manufacture_date;	/* formatted */
	uint16_t serial;		/* number */

	char	 manufacturer[SMBUS_DATA_SIZE];
	char	 device_name[SMBUS_DATA_SIZE];
	char	 device_chemistry[SMBUS_DATA_SIZE];
	char	 oem_data[SMBUS_DATA_SIZE];
};

#define CHECK(kind, cmd, val, senst, sens) { \
	SMBUS_READ_##kind, SMBATT_CMD_##cmd, \
	offsetof(struct acpisbs_battery, val), \
	(SMBUS_READ_##kind == SMBUS_READ_BLOCK ? SMBUS_DATA_SIZE : 2), \
	#val, senst, sens }

struct acpisbs_battery_check {
	uint8_t	mode;
	uint8_t command;
	size_t	offset;
	int	len;
	char	*name;
	int	sensor_type;
	char	*sensor_desc;
} acpisbs_battery_checks[] = {
	CHECK(WORD, TEMPERATURE, temperature, SENSOR_TEMP,
	    "internal temperature"),
	CHECK(WORD, VOLTAGE, voltage, SENSOR_VOLTS_DC,
	    "voltage"),
	CHECK(WORD, CURRENT, current, SENSOR_AMPS,
	    "current being supplied"),
	CHECK(WORD, AVERAGE_CURRENT, avg_current, SENSOR_AMPS,
	    "average current supplied"),
	CHECK(WORD, RELATIVE_STATE_OF_CHARGE, rel_charge, SENSOR_PERCENT,
	    "remaining capacity"),
	CHECK(WORD, ABSOLUTE_STATE_OF_CHARGE, abs_charge, SENSOR_PERCENT,
	    "remaining of design capacity"),
	CHECK(WORD, REMAINING_CAPACITY, capacity, SENSOR_AMPHOUR,
	    "remaining capacity"),
	CHECK(WORD, FULL_CHARGE_CAPACITY, full_capacity, SENSOR_AMPHOUR,
	    "capacity when fully charged"),
	CHECK(WORD, RUN_TIME_TO_EMPTY, run_time, SENSOR_INTEGER,
	    "remaining run time minutes"),
	CHECK(WORD, AVERAGE_TIME_TO_EMPTY, avg_empty_time, SENSOR_INTEGER,
	    "avg remaining minutes"),
	CHECK(WORD, AVERAGE_TIME_TO_FULL, avg_full_time, SENSOR_INTEGER,
	    "avg minutes until full charge"),
	CHECK(WORD, CHARGING_CURRENT, charge_current, SENSOR_AMPS,
	    "desired charging rate"),
	CHECK(WORD, CHARGING_VOLTAGE, charge_voltage, SENSOR_VOLTS_DC,
	    "desired charging voltage"),
	CHECK(WORD, BATTERY_STATUS, status, -1,
	    "status"),
	CHECK(WORD, CYCLE_COUNT, cycle_count, SENSOR_INTEGER,
	    "charge and discharge cycles"),
	CHECK(WORD, DESIGN_CAPACITY, design_capacity, SENSOR_AMPHOUR,
	    "capacity of new battery"),
	CHECK(WORD, DESIGN_VOLTAGE, design_voltage, SENSOR_VOLTS_DC,
	    "voltage of new battery"),
#if 0
	CHECK(WORD, SPECIFICATION_INFO, spec, -1,
	    NULL),
#endif
	CHECK(WORD, MANUFACTURE_DATE, manufacture_date, SENSOR_STRING,
	    "date battery was manufactured"),
	CHECK(WORD, SERIAL_NUMBER, serial, SENSOR_STRING,
	    "serial number"),

	CHECK(BLOCK, MANUFACTURER_NAME, manufacturer, SENSOR_STRING,
	    "manufacturer name"),
	CHECK(BLOCK, DEVICE_NAME, device_name, SENSOR_STRING,
	    "battery model number"),
	CHECK(BLOCK, DEVICE_CHEMISTRY, device_chemistry, SENSOR_STRING,
	    "battery chemistry"),
	CHECK(BLOCK, MANUFACTURER_DATA, oem_data, SENSOR_STRING,
	    "manufacturer-specific data"),
};

struct acpisbs_softc {
	struct device		sc_dev;

	struct acpi_softc	*sc_acpi;
	struct aml_node		*sc_devnode;
	struct acpiec_softc     *sc_ec;
	uint8_t			sc_ec_base;

	struct acpisbs_battery	sc_battery;

	struct ksensor		*sc_sensors;
	struct ksensordev	sc_sensordev;
	struct sensor_task	*sc_sensor_task;
	struct timeval		sc_lastpoll;
};

extern void acpiec_read(struct acpiec_softc *, u_int8_t, int, u_int8_t *);
extern void acpiec_write(struct acpiec_softc *, u_int8_t, int, u_int8_t *);

int	acpisbs_match(struct device *, void *, void *);
void	acpisbs_attach(struct device *, struct device *, void *);
void	acpisbs_setup_sensors(struct acpisbs_softc *);
void	acpisbs_refresh_sensors(struct acpisbs_softc *);
void	acpisbs_read(struct acpisbs_softc *);
int	acpisbs_notify(struct aml_node *, int, void *);

int	acpi_smbus_read(struct acpisbs_softc *, uint8_t, uint8_t, int, void *);

struct cfattach acpisbs_ca = {
	sizeof(struct acpisbs_softc),
	acpisbs_match,
	acpisbs_attach,
};

struct cfdriver acpisbs_cd = {
	NULL, "acpisbs", DV_DULL
};

const char *acpisbs_hids[] = {
	"ACPI0002",
	NULL
};

int
acpisbs_match(struct device *parent, void *match, void *aux)
{
	struct acpi_attach_args *aa = aux;
	struct cfdata *cf = match;

	return (acpi_matchhids(aa, acpisbs_hids, cf->cf_driver->cd_name));
}

void
acpisbs_attach(struct device *parent, struct device *self, void *aux)
{
	struct acpisbs_softc *sc = (struct acpisbs_softc *)self;
	struct acpi_attach_args *aa = aux;
	int64_t sbs, val;

	sc->sc_acpi = (struct acpi_softc *)parent;
	sc->sc_devnode = aa->aaa_node;

	memset(&sc->sc_battery, 0, sizeof(sc->sc_battery));

	getmicrotime(&sc->sc_lastpoll);

	if (aml_evalinteger(sc->sc_acpi, sc->sc_devnode, "_SBS", 0, NULL, &sbs))
		return;

	/*
	 * The parent node of the device block containing the _HID must also
	 * have an _EC node, which contains the base address and query value.
	 */
	if (aml_evalinteger(sc->sc_acpi, sc->sc_devnode->parent, "_EC", 0,
	    NULL, &val))
		return;
	sc->sc_ec_base = (val >> 8) & 0xff;

	if (!sc->sc_acpi->sc_ec)
		return;
	sc->sc_ec = sc->sc_acpi->sc_ec;

	printf(": %s", sc->sc_devnode->name);

	if (sbs > 0) {
		acpisbs_read(sc);

		if (sc->sc_battery.device_name[0])
			printf(" model \"%s\"", sc->sc_battery.device_name);
		if (sc->sc_battery.serial)
			printf(" serial %d", sc->sc_battery.serial);
		if (sc->sc_battery.device_chemistry[0])
			printf(" type %s", sc->sc_battery.device_chemistry);
		if (sc->sc_battery.manufacturer[0])
			printf(" oem \"%s\"", sc->sc_battery.manufacturer);
	}

	printf("\n");

	acpisbs_setup_sensors(sc);
	acpisbs_refresh_sensors(sc);

	aml_register_notify(sc->sc_devnode, aa->aaa_dev, acpisbs_notify,
	    sc, ACPIDEV_POLL);
}

void
acpisbs_read(struct acpisbs_softc *sc)
{
	int i;

	for (i = 0; i < nitems(acpisbs_battery_checks); i++) {
		struct acpisbs_battery_check check = acpisbs_battery_checks[i];
		void *p = (void *)&sc->sc_battery + check.offset;
		uint16_t *ival = (uint16_t *)p;
		char *cval = (char *)p;

		acpi_smbus_read(sc, check.mode, check.command, check.len, p);

		if (check.mode == SMBUS_READ_BLOCK)
			DPRINTF(("%s: %s: %s\n", sc->sc_dev.dv_xname,
			    check.name, cval));
		else
			DPRINTF(("%s: %s: %u\n", sc->sc_dev.dv_xname,
			    check.name, *ival));

		switch (check.command) {
		case SMBATT_CMD_BATTERY_MODE:
			if (*ival & SMBATT_BM_CAPACITY_MODE)
				sc->sc_battery.units = ACPISBS_UNITS_MW;
			else
				sc->sc_battery.units = ACPISBS_UNITS_MA;

			break;
		}
	}
}

void
acpisbs_setup_sensors(struct acpisbs_softc *sc)
{
	int i;

	memset(&sc->sc_sensordev, 0, sizeof(sc->sc_sensordev));
	strlcpy(sc->sc_sensordev.xname, DEVNAME(sc),
	    sizeof(sc->sc_sensordev.xname));

	sc->sc_sensors = mallocarray(sizeof(struct ksensor),
	    nitems(acpisbs_battery_checks), M_DEVBUF, M_WAITOK | M_ZERO);

	for (i = 0; i < nitems(acpisbs_battery_checks); i++) {
		struct acpisbs_battery_check check = acpisbs_battery_checks[i];

		if (check.sensor_type < 0)
			continue;

		strlcpy(sc->sc_sensors[i].desc, check.sensor_desc,
		    sizeof(sc->sc_sensors[i].desc));

		if (check.sensor_type == SENSOR_AMPHOUR &&
		    sc->sc_battery.units == ACPISBS_UNITS_MW)
			/* translate to watt-hours */
			sc->sc_sensors[i].type = SENSOR_WATTHOUR;
		else
			sc->sc_sensors[i].type = check.sensor_type;

		sc->sc_sensors[i].value = 0;
		sensor_attach(&sc->sc_sensordev, &sc->sc_sensors[i]);
	}

	sensordev_install(&sc->sc_sensordev);
}

void
acpisbs_refresh_sensors(struct acpisbs_softc *sc)
{
	int i;

	for (i = 0; i < nitems(acpisbs_battery_checks); i++) {
		struct acpisbs_battery_check check = acpisbs_battery_checks[i];
		void *p = (void *)&sc->sc_battery + check.offset;
		uint16_t *ival = (uint16_t *)p;

		if (check.sensor_type < 0)
			continue;

		if (1) { /* present */
			sc->sc_sensors[i].flags = 0;
			sc->sc_sensors[i].status = SENSOR_S_OK;

			switch (check.sensor_type) {
			case SENSOR_AMPS:
				sc->sc_sensors[i].value = *ival * 100;
				break;

			case SENSOR_AMPHOUR:
			case SENSOR_WATTHOUR:
				sc->sc_sensors[i].value = *ival * 10000;
				break;

			case SENSOR_PERCENT:
				sc->sc_sensors[i].value = *ival * 1000;
				break;

			case SENSOR_STRING:
				strlcpy(sc->sc_sensors[i].string, (char *)p,
				    sizeof(sc->sc_sensors[i].string));
				break;

			case SENSOR_TEMP:
				/* .1 degK */
				sc->sc_sensors[i].value = (*ival * 10000) +
				    273150000;
				break;

			case SENSOR_VOLTS_DC:
				sc->sc_sensors[i].value = *ival * 1000;
				break;

			default:
				if (*ival == 65535) {
					sc->sc_sensors[i].value = 0;
					sc->sc_sensors[i].status =
					    SENSOR_S_UNKNOWN;
					sc->sc_sensors[i].flags =
					    SENSOR_FUNKNOWN;
				} else
					sc->sc_sensors[i].value = *ival;
			}
		} else {
			sc->sc_sensors[i].value = 0;
			sc->sc_sensors[i].status = SENSOR_S_UNKNOWN;
			sc->sc_sensors[i].flags = SENSOR_FUNKNOWN;
		}
	}
}

int
acpisbs_notify(struct aml_node *node, int notify_type, void *arg)
{
	struct acpisbs_softc *sc = arg;
	struct timeval tv;

	DPRINTF(("%s: %s: %d\n", sc->sc_dev.dv_xname, __func__, notify_type));
	printf("%s: %s: %d\n", sc->sc_dev.dv_xname, __func__, notify_type);

	getmicrotime(&tv);

	if (tv.tv_sec - sc->sc_lastpoll.tv_sec > ACPISBS_POLL_FREQ) {
		acpisbs_read(sc);
		getmicrotime(&sc->sc_lastpoll);
	}

	acpisbs_refresh_sensors(sc);

	return 0;
}

int
acpi_smbus_read(struct acpisbs_softc *sc, uint8_t type, uint8_t cmd, int len,
    void *buf)
{
	int j;
	uint8_t addr = SMBATT_ADDRESS;
	uint8_t val;

	acpiec_write(sc->sc_ec, sc->sc_ec_base + SMBUS_ADDR, 1, &addr);
	acpiec_write(sc->sc_ec, sc->sc_ec_base + SMBUS_CMD, 1, &cmd);
	acpiec_write(sc->sc_ec, sc->sc_ec_base + SMBUS_PRTCL, 1, &type);

	for (j = SMBUS_TIMEOUT; j < 0; j--) {
		acpiec_read(sc->sc_ec, sc->sc_ec_base + SMBUS_PRTCL, 1, &val);
		if (val == 0)
			break;
	}
	if (j == 0) {
		printf("%s: %s: timeout reading 0x%x\n", sc->sc_dev.dv_xname,
		    __func__, addr);
		return 1;
	}

	acpiec_read(sc->sc_ec, sc->sc_ec_base + SMBUS_STS, 1, &val);
	if (val & SMBUS_STS_MASK) {
		printf("%s: %s: error reading status: 0x%x\n",
		    sc->sc_dev.dv_xname, __func__, addr);
		return 1;
	}

	switch (type) {
        case SMBUS_READ_WORD: {
		uint8_t word[2];
		acpiec_read(sc->sc_ec, sc->sc_ec_base + SMBUS_DATA, 2,
		    (uint8_t *)&word);

		*(uint16_t *)buf = (word[1] << 8) | word[0];

		//DPRINTF(("0x%x + 0x%x = 0x%x\n", word[1], word[0],
		//    *(uint16_t *)buf));

		break;
	}
	case SMBUS_READ_BLOCK:
		bzero(buf, len);

		/* find number of bytes to read */
		acpiec_read(sc->sc_ec, sc->sc_ec_base + SMBUS_BCNT, 1, &val);
		val &= 0x1f;
		if (len > val)
			len = val;

		for (j = 0; j < len; j++) {
			acpiec_read(sc->sc_ec, sc->sc_ec_base + SMBUS_DATA + j,
			    1, &val);
			((char *)buf)[j] = val;

			//DPRINTF(("0x%x ", val));
		}
		//DPRINTF((" = %s\n", (char *)buf));
		break;
	default:
		printf("%s: %s: unknown mode 0x%x\n", sc->sc_dev.dv_xname,
		    __func__, type);
		return 1;
	}

	return 0;
}
