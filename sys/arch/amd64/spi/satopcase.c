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

/*
 * SPI keyboard and touchpad driver for MacBook8,1 and newer.
 *
 * Protocol info from macbook12-spi-driver Linux driver by Federico Lorenzi,
 * Ronald Tschalär, et al.
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
#ifdef WSDISPLAY_COMPAT_RAWKBD
#include <dev/wscons/wskbdraw.h>
#endif
#include <dev/wscons/wskbdvar.h>
#include <dev/wscons/wsksymdef.h>
#include <dev/wscons/wsksymvar.h>

#include "satopcasevar.h"

/* #define SATOPCASE_DEBUG */
#define SATOPCASE_DEBUG

#ifdef SATOPCASE_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

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
void	satopcase_kbd_handle_msg(struct satopcase_softc *,
	    struct satopcase_kbd_message);

uint16_t satopcase_crc16(uint8_t *, size_t);

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
}

int
satopcase_detach(struct device *self, int flags)
{
	struct satopcase_softc *sc = (struct satopcase_softc *)self;

	if (sc->sc_ih != NULL) {
		intr_disestablish(sc->sc_ih);
		sc->sc_ih = NULL;
	}

	return 0;
}

int
satopcase_intr(void *arg)
{
	struct satopcase_softc *sc = arg;
	uint16_t crc;
	uint16_t msg_crc;

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));

	spi_acquire_bus(sc->sc_spi_tag, 0);
	spi_config(sc->sc_spi_tag, &sc->sc_spi_conf);
	spi_read(sc->sc_spi_tag, sc->sc_read_raw, sizeof(sc->sc_read_raw));
	spi_release_bus(sc->sc_spi_tag, 0);

	crc = satopcase_crc16(sc->sc_read_raw, SATOPCASE_PACKET_SIZE - 2);
	msg_crc = (sc->sc_read_raw[SATOPCASE_PACKET_SIZE - 1] << 8) |
	    sc->sc_read_raw[SATOPCASE_PACKET_SIZE - 2];
	if (crc != msg_crc) {
		printf("%s: corrupt packet (crc 0x%x != msg crc 0x%x)\n",
		    sc->sc_dev.dv_xname, crc, msg_crc);
		return 1;
	}

	satopcase_handle_msg(sc);

	return 1;
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
		sc->sc_rawkbd = (*(int *)data == WSKBD_RAW);
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
satopcase_kbd_proc_event(struct satopcase_softc *sc, int key, int fn,
    int event_type)
{
	struct wscons_keymap wkm;
	int x, trans = 0;

	DPRINTF(("%s: key %s: %d (fn %d)\n", sc->sc_dev.dv_xname,
	    (event_type == WSCONS_EVENT_KEY_DOWN ? "down" : "up"), key,
	    fn));

	if (fn) {
		wskbd_get_mapentry(&satopcase_kbd_mapdata, key, &wkm);

		for (x = 0; x < nitems(satopcase_kb_fn_trans); x++) {
			struct satopcase_kbd_fn_trans_entry e =
			    satopcase_kb_fn_trans[x];
			if (e.from == wkm.group1[0]) {
				key = e.to;
				trans = 1;
				break;
			}
		}

		if (trans)
			DPRINTF(("%s: translated key with fn to %d\n",
			    sc->sc_dev.dv_xname, key));
		else {
			DPRINTF(("%s: no fn translation for 0x%x 0x%x 0x%x 0x%x\n",
				sc->sc_dev.dv_xname, wkm.group1[0],
				wkm.group1[1], wkm.group2[0], wkm.group2[1]));
			/* Fn+key didn't translate, so don't pass it through */
			return;
		}
	}

#ifdef WSDISPLAY_COMPAT_RAWKBD
	if (sc->sc_rawkbd) {
		unsigned char cbuf[2];
		int c, j = 0, s;

		c = satopcase_raw_keycodes_us[key];
		if (c == RAWKEY_Null)
			return;
		if (c & 0x80)
			cbuf[j++] = 0xe0;
		cbuf[j] = c & 0x7f;
		if (event_type == WSCONS_EVENT_KEY_UP)
			cbuf[j] |= 0x80;
		j++;
		s = spltty();
		wskbd_rawinput(sc->sc_wskbddev, cbuf, j);
		splx(s);
		return;
	}
#endif

	wskbd_input(sc->sc_wskbddev, event_type, key);
}

void
satopcase_handle_msg(struct satopcase_softc *sc)
{
	int x;

	DPRINTF(("%s: incoming message:", sc->sc_dev.dv_xname));
	for (x = 0; x < SATOPCASE_PACKET_SIZE; x++)
		DPRINTF((" %02x", (sc->sc_read_raw[x] & 0xff)));
	DPRINTF(("\n"));

	switch (sc->sc_read_packet.type) {
	case PACKET_TYPE_READ:
		if (sc->sc_read_packet.remaining || sc->sc_read_packet.offset) {
			printf("XXXX: remaining %d, offset %d\n",
				sc->sc_read_packet.remaining,
				sc->sc_read_packet.offset);
		}

		switch (sc->sc_read_packet.device) {
		case PACKET_DEVICE_KEYBOARD:
			satopcase_kbd_handle_msg(sc,
			    sc->sc_read_packet.message.keyboard);
			break;
		}
		break;
	default:
		DPRINTF(("%s: unknown packet type 0x%x\n", sc->sc_dev.dv_xname,
		    sc->sc_read_packet.type));
	}
}

void
satopcase_kbd_handle_msg(struct satopcase_softc *sc,
    struct satopcase_kbd_message kbd_msg)
{
	int pressed[KBD_MSG_KEYS + KBD_MSG_MODS] = { 0 };
	int x, y, tdown;

	if (kbd_msg.overflow)
		return;

	/*
	 * We don't get key codes for modifier keys, so turn bits in the
	 * modifiers field into key codes to track pressed state.
	 */
	for (x = 0; x < KBD_MSG_KEYS; x++)
		pressed[x] = kbd_msg.pressed[x];
	for (x = 0; x < KBD_MSG_MODS; x++)
		pressed[KBD_MSG_KEYS + x] = (kbd_msg.modifiers & (1 << x)) ?
		    (KBD_MOD_CONTROL_L + x) : 0;

	/*
	 * Key press slots are not constant, so when holding down a key, then
	 * another, then lifting the first, the second key code shifts into the
	 * first pressed slot.  Check each slot when determining whether a key
	 * was actually lifted.
	 */
	for (x = 0; x < nitems(sc->kbd_keys_down); x++) {
		if (!sc->kbd_keys_down[x])
			continue;

		tdown = 0;
		for (y = 0; y < nitems(pressed); y++) {
			if (sc->kbd_keys_down[x] == pressed[y]) {
				tdown = 1;
				break;
			}
		}

		if (!tdown)
			satopcase_kbd_proc_event(sc, sc->kbd_keys_down[x],
			    kbd_msg.fn, WSCONS_EVENT_KEY_UP);
	}

	/* Same for new key presses */
	for (x = 0; x < nitems(pressed); x++) {
		if (!pressed[x])
			continue;

		tdown = 0;
		for (y = 0; y < nitems(sc->kbd_keys_down); y++) {
			if (pressed[x] == sc->kbd_keys_down[y]) {
				tdown = 1;
				break;
			}
		}

		if (!tdown)
			satopcase_kbd_proc_event(sc, pressed[x], kbd_msg.fn,
			    WSCONS_EVENT_KEY_DOWN);
	}

	memcpy(sc->kbd_keys_down, pressed, sizeof(sc->kbd_keys_down));
}


uint16_t
satopcase_crc16(uint8_t *msg, size_t len)
{
	uint16_t crc = 0;
	int x;

	for (x = 0; x < len; x++)
		crc = (crc >> 8) ^ crc16_table[(crc ^ msg[x]) & 0xff];

	return crc;
}
