/* $OpenBSD$ */
/*
 * Apple SPI "topcase" driver
 *
 * Copyright (c) 2015-2019 joshua stein <jcs@openbsd.org>
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
#include <sys/stdint.h>

#include <dev/acpi/acpireg.h>
#include <dev/acpi/acpivar.h>
#include <dev/acpi/acpidev.h>
#include <dev/acpi/amltypes.h>
#include <dev/acpi/dsdt.h>

#include <dev/ic/ispivar.h>
#include <dev/spi/spivar.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wskbdraw.h>
#include <dev/wscons/wskbdvar.h>
#include <dev/wscons/wsksymdef.h>
#include <dev/wscons/wsksymvar.h>

/* #define SATOPCASE_DEBUG */
#define SATOPCASE_DEBUG

#ifdef SATOPCASE_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

#define SATOPCASE_PACKET_SIZE	256

struct satopcase_kbd_keymap {
	int		row;
	int		col;
	keysym_t	key;
};

struct satopcase_softc {
	struct device	sc_dev;
	spi_tag_t	sc_spi_tag;
	struct ispi_gpe_intr sc_gpe_intr;
	void		*sc_ih;

	struct spi_config sc_spi_conf;

	char		sc_read[SATOPCASE_PACKET_SIZE];

	/* from _DSM */
	uint64_t	spi_sclk_period;
	uint64_t	spi_word_size;
	uint64_t	spi_bit_order;
	uint64_t	spi_spo;
	uint64_t	spi_sph;
	uint64_t	spi_cs_delay;
	uint64_t	reset_a2r_usec;
	uint64_t	reset_rec_usec;

	struct device	*sc_wskbddev;
	int		sc_rawkbd;

	const struct satopcase_kbd_keymap *sc_kmap;
	const keysym_t	sc_kcodes;
	const keysym_t	sc_xt_kcodes;
	int		sc_ksize;
};

struct spi_packet {
	uint8_t		flags;
	uint8_t		device;
	uint16_t	offset;
	uint16_t	remaining;
	uint16_t	length;
	uint8_t		data[246];
	uint16_t	crc16;
};

int	satopcase_match(struct device *, void *, void *);
void	satopcase_attach(struct device *, struct device *, void *);
int	satopcase_detach(struct device *, int);
int	satopcase_intr(void *);
int	satopcase_get_params(struct satopcase_softc *, struct aml_node *);

int	satopcase_kbd_enable(void *, int);
void	satopcase_kbd_setleds(void *, int);
int	satopcase_kbd_ioctl(void *, u_long, caddr_t, int, struct proc *);
void	satopcase_kbd_cnbell(void *, u_int, u_int, u_int);
void	satopcase_kbd_cngetc(void *, u_int *, int *);
void	satopcase_kbd_cnpollc(void *, int);

void	satopcase_handle_msg(struct satopcase_softc *);

struct cfattach satopcase_ca = {
	sizeof(struct satopcase_softc),
	satopcase_match,
	satopcase_attach,
	satopcase_detach,
	NULL
};

struct cfdriver satopcase_cd = {
	NULL, "satopcase", DV_DULL
};

struct wskbd_accessops satopcase_kbd_accessops = {
	satopcase_kbd_enable,
	satopcase_kbd_setleds,
	satopcase_kbd_ioctl,
};

struct wskbd_consops satopcase_kbd_consops = {
	satopcase_kbd_cngetc,
	satopcase_kbd_cnpollc,
	satopcase_kbd_cnbell,
};

const keysym_t satopcase_keycodes_us[] = {
	KS_KEYCODE(0), KS_s,
	KS_KEYCODE(1), KS_a,
	KS_KEYCODE(2), KS_b,
};

struct wscons_keydesc satopcase_kbd_keydesctab[] = {
	{ KB_US, 0, sizeof(satopcase_keycodes_us) / sizeof(keysym_t),
	    satopcase_keycodes_us },
	{ 0, 0, 0, 0 },
};

struct wskbd_mapdata satopcase_kbd_mapdata = {
	satopcase_kbd_keydesctab,
	KB_US,
};

int
satopcase_match(struct device *parent, void *match, void *aux)
{
	struct spi_attach_args *sa = aux;
	struct aml_node *node = sa->sa_cookie;
	struct aml_value res;
	uint64_t val;

	if (strcmp(sa->sa_name, "satopcase") != 0)
		return 0;

	/* don't attach if USB interface is present */
	/* TODO: should we then call UIEN(1) to force USB attachment? */
	if (aml_evalinteger(acpi_softc, node, "UIST", 0, NULL, &val) == 0 &&
	    val) {
		DPRINTF(("%s: not attaching satopcase, USB enabled\n",
		    sa->sa_name));
		return 0;
	}

	/* if SPI is not enabled, enable it */
	if (aml_evalinteger(acpi_softc, node, "SIST", 0, NULL, &val) == 0 &&
	    !val) {
		if (aml_evalname(acpi_softc, node, "SIEN", 0, NULL, &res)) {
			DPRINTF(("%s: couldn't enable SPI mode\n",
			    sa->sa_name));
			return 0;
		}

		DELAY(500);
	}

	return 1;
}

void
satopcase_attach(struct device *parent, struct device *self, void *aux)
{
	struct satopcase_softc *sc = (struct satopcase_softc *)self;
	struct spi_attach_args *sa = aux;
	struct wskbddev_attach_args waa;

	printf("%s: %s\n", __func__, aml_nodename(sa->sa_cookie));

	if (satopcase_get_params(sc, sa->sa_cookie) != 0)
		return;

	sc->sc_spi_tag = sa->sa_tag;

	if (sc->sc_gpe_intr.gpe_node) {
		printf(" %s", spi_intr_string(sc->sc_spi_tag, &sc->sc_gpe_intr));

		sc->sc_ih = spi_intr_establish(sc->sc_spi_tag, &sc->sc_gpe_intr,
		    IPL_TTY, satopcase_intr, sc, sc->sc_dev.dv_xname);
		if (sc->sc_ih == NULL)
			printf(", can't establish interrupt");
	}

	printf("\n");

	waa.console = 0;
	waa.keymap = &satopcase_kbd_mapdata;
	waa.accessops = &satopcase_kbd_accessops;
	waa.accesscookie = sc;

	sc->sc_wskbddev = config_found((struct device *)sc, &waa,
	    wskbddevprint);

	{
		char junk[] = {
0x40, 0xd0, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x20, 0x10, 0x02, 0x00, 0x00,
0x02, 0x00, 0x00, 0xb3, 0xfb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd0, 0x62,
};

		spi_acquire_bus(sc->sc_spi_tag, 0);
		spi_config(sc->sc_spi_tag, &sc->sc_spi_conf);
		spi_write(sc->sc_spi_tag, junk, sizeof(junk));
		spi_release_bus(sc->sc_spi_tag, 0);
	}
}

int
satopcase_detach(struct device *self, int flags)
{
	struct satopcase_softc *sc = (struct satopcase_softc *)self;

	if (sc->sc_ih != NULL) {
		intr_disestablish(sc->sc_ih);
		sc->sc_ih = NULL;
	}

	return (0);
}

int
satopcase_intr(void *arg)
{
	struct satopcase_softc *sc = arg;

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));

	spi_acquire_bus(sc->sc_spi_tag, 0);
	spi_config(sc->sc_spi_tag, &sc->sc_spi_conf);
	spi_read(sc->sc_spi_tag, sc->sc_read, sizeof(sc->sc_read));
	spi_release_bus(sc->sc_spi_tag, 0);

	satopcase_handle_msg(sc);

	return (1);
}

int
satopcase_get_params(struct satopcase_softc *sc, struct aml_node *node)
{
	/* a0b5b7c6-1318-441c-b0c9-fe695eaf949b */
	static uint8_t topcase_guid[] = {
		0xC6, 0xB7, 0xB5, 0xA0, 0x18, 0x13, 0x1C, 0x44,
		0xB0, 0xC9, 0xFE, 0x69, 0x5E, 0xAF, 0x94, 0x9B,
	};
	struct aml_value cmd[4], res;
	struct aml_node *gpe_node;
	uint64_t val;
	int i;

	/*
	 * On newer Apple hardware where we claim an OSI of Darwin, _CRS
	 * doesn't return a useful SpiSerialBusV2 object but instead returns
	 * parameters from a _DSM method when called with a particular UUID
	 * which macOS does.
	 */
	if (!aml_searchname(node, "_DSM")) {
		printf("%s: couldn't find _DSM at %s\n", sc->sc_dev.dv_xname,
		    aml_nodename(node));
		return 1;
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
		return 1;
	}

	if (res.type != AML_OBJTYPE_PACKAGE) {
		printf("%s: bad _DSM result at %s: %d\n",
		    sc->sc_dev.dv_xname, aml_nodename(node), res.type);
		aml_freevalue(&res);
		return 1;
	}

	if (res.length % 2 != 0) {
		printf("%s: _DSM length %d not even\n", sc->sc_dev.dv_xname,
		    res.length);
		aml_freevalue(&res);
		return 1;
	}

	for (i = 0; i < res.length; i += 2) {
		char *k;

		if (res.v_package[i]->type != AML_OBJTYPE_STRING ||
		    res.v_package[i + 1]->type != AML_OBJTYPE_BUFFER) {
			printf("%s: expected string+buffer, got %d+%d\n",
			    sc->sc_dev.dv_xname, res.v_package[i]->type,
			    res.v_package[i + 1]->type);
			aml_freevalue(&res);
			return 1;
		}

		k = res.v_package[i]->v_string;
		val = aml_val2int(res.v_package[i + 1]);

		DPRINTF(("%s: %s = %lld\n", sc->sc_dev.dv_xname, k, val));

		if (strcmp(k, "spiSclkPeriod") == 0) {
			sc->spi_sclk_period = val;
			sc->sc_spi_conf.sc_freq = 1000000000 / val;
		} else if (strcmp(k, "spiWordSize") == 0) {
			sc->spi_word_size = val;
			sc->sc_spi_conf.sc_bpw = val;
		} else if (strcmp(k, "spiBitOrder") == 0) {
			sc->spi_bit_order = val;
		} else if (strcmp(k, "spiSPO") == 0) {
			sc->spi_spo = val;
			if (val)
				sc->sc_spi_conf.sc_flags |= SPI_CONFIG_CPOL;
		} else if (strcmp(k, "spiSPH") == 0) {
			sc->spi_sph = val;
			if (val)
				sc->sc_spi_conf.sc_flags |= SPI_CONFIG_CPHA;
		} else if (strcmp(k, "spiCSDelay") == 0) {
			sc->spi_cs_delay = val;
		} else if (strcmp(k, "resetA2RUsec") == 0) {
			sc->reset_a2r_usec = val;
		} else if (strcmp(k, "resetRecUsec") == 0) {
			sc->reset_rec_usec = val;
		} else {
			DPRINTF(("%s: unknown _DSM key %s\n",
			    sc->sc_dev.dv_xname, k));
		}
	}
	aml_freevalue(&res);

	gpe_node = aml_searchname(node, "_GPE");
	if (gpe_node) {
		aml_evalinteger(acpi_softc, gpe_node->parent, "_GPE", 0, NULL,
		    &val);
		sc->sc_gpe_intr.gpe_node = gpe_node;
		sc->sc_gpe_intr.gpe_int = val;
	}

	return 0;
}

int
satopcase_kbd_enable(void *v, int power)
{
	printf("%s\n", __func__);
	return 0;
}

void
satopcase_kbd_setleds(void *v, int power)
{
	printf("%s\n", __func__);
}

int
satopcase_kbd_ioctl(void *v, u_long cmd, caddr_t data, int flag, struct proc *p)
{
#ifdef WSDISPLAY_COMPAT_RAWKBD
	struct satopcase_softc *sc = v;
#endif

	switch (cmd) {
	case WSKBDIO_GTYPE:
		*(int *)data = WSKBD_TYPE_USB;
		return 0;
	case WSKBDIO_SETLEDS:
		return 0;
	case WSKBDIO_GETLEDS:
		*(int *)data = 0;
		return 0;
#ifdef WSDISPLAY_COMPAT_RAWKBD
	case WSKBDIO_SETMODE:
		sc->sc_rawkbd = *(int *)data == WSKBD_RAW;
		return 0;
#endif
	}
	return -1;
}

void
satopcase_kbd_cnbell(void *v, u_int pitch, u_int period, u_int volume)
{
	printf("%s\n", __func__);
}

void
satopcase_kbd_cngetc(void *v, u_int *type, int *data)
{
	printf("%s\n", __func__);
}

void
satopcase_kbd_cnpollc(void *v, int on)
{
	printf("%s\n", __func__);
}

void
satopcase_handle_msg(struct satopcase_softc *sc)
{
	int x;

	DPRINTF(("%s: incoming message:", sc->sc_dev.dv_xname));
	for (x = 0; x < SATOPCASE_PACKET_SIZE; x++)
		DPRINTF((" %02x", (sc->sc_read[x] & 0xff)));
	DPRINTF(("\n"));
}
