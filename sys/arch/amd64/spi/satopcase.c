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
#include <dev/wscons/wsmousevar.h>

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
int	satopcase_get_dsm_params(struct satopcase_softc *, struct aml_node *);

int	satopcase_kbd_enable(void *, int);
void	satopcase_kbd_setleds(void *, int);
int	satopcase_kbd_ioctl(void *, u_long, caddr_t, int, struct proc *);
void	satopcase_kbd_cnbell(void *, u_int, u_int, u_int);
void	satopcase_kbd_cngetc(void *, u_int *, int *);
void	satopcase_kbd_cnpollc(void *, int);

void	satopcase_tp_init(struct satopcase_softc *);
int	satopcase_tp_enable(void *);
int	satopcase_tp_ioctl(void *, u_long, caddr_t, int, struct proc *);
void	satopcase_tp_disable(void *);

int	satopcase_send_msg(struct satopcase_softc *, size_t, int);
void	satopcase_handle_msg(struct satopcase_softc *);
void	satopcase_info_handle_msg(struct satopcase_softc *,
	    struct satopcase_spi_msg *);
void	satopcase_kbd_handle_msg(struct satopcase_softc *,
	    struct satopcase_spi_msg *);
void	satopcase_tp_handle_msg(struct satopcase_softc *,
	    struct satopcase_spi_msg *);

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

/* keyboard */

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

/* touchpad */

struct wsmouse_accessops satopcase_tp_accessops = {
	satopcase_tp_enable,
	satopcase_tp_ioctl,
	satopcase_tp_disable,
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

int
satopcase_search(struct device *parent, void *match, void *aux)
{
	struct satopcase_softc *sc = (struct satopcase_softc *)parent;
	struct cfdata *cf = match;
	struct wskbddev_attach_args wkaa;
	struct wsmousedev_attach_args wmaa;

	if (strcmp(cf->cf_driver->cd_name, "wskbd") == 0) {
		memset(&wkaa, 0, sizeof(wkaa));

		wkaa.console = 0;
		wkaa.keymap = &satopcase_kbd_mapdata;
		wkaa.accessops = &satopcase_kbd_accessops;
		wkaa.accesscookie = sc;

		if (cf->cf_attach->ca_match(parent, cf, &wkaa) == 0)
			return 0;

		sc->sc_wskbddev = config_attach(parent, cf, &wkaa,
		    wskbddevprint);
		return 1;
	} else if (strcmp(cf->cf_driver->cd_name, "wsmouse") == 0) {
		memset(&wmaa, 0, sizeof(wmaa));

		wmaa.accessops = &satopcase_tp_accessops;
		wmaa.accesscookie = sc;

		if (cf->cf_attach->ca_match(parent, cf, &wmaa) == 0)
			return 0;

		sc->sc_wsmousedev = config_attach(parent, cf, &wmaa,
		    wsmousedevprint);
		return 1;
	}

	return 0;
}


void
satopcase_attach(struct device *parent, struct device *self, void *aux)
{
	struct satopcase_softc *sc = (struct satopcase_softc *)self;
	struct spi_attach_args *sa = aux;

	rw_init(&sc->sc_busylock, sc->sc_dev.dv_xname);

	if (satopcase_get_dsm_params(sc, sa->sa_cookie) != 0)
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

	config_search(satopcase_search, self, self);

	if (sc->sc_wsmousedev)
		satopcase_tp_init(sc);
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

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));

	rw_enter_write(&sc->sc_busylock);

	memset(sc->sc_read_raw, 0, sizeof(struct satopcase_spi_pkt));

	spi_acquire_bus(sc->sc_spi_tag, 0);
	spi_config(sc->sc_spi_tag, &sc->sc_spi_conf);
	spi_read(sc->sc_spi_tag, sc->sc_read_raw, sizeof(sc->sc_read_raw));
	spi_release_bus(sc->sc_spi_tag, 0);

	satopcase_handle_msg(sc);

	rw_exit_write(&sc->sc_busylock);
	wakeup(&sc);

	return 1;
}

int
satopcase_get_dsm_params(struct satopcase_softc *sc, struct aml_node *node)
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

#if 0
		DPRINTF(("%s: %s = %lld\n", sc->sc_dev.dv_xname, k, val));
#endif

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
	struct satopcase_softc *sc = v;

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));

	return 0;
}

void
satopcase_kbd_setleds(void *v, int power)
{
	DPRINTF(("%s\n", __func__));
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
	DPRINTF(("%s\n", __func__));
}

void
satopcase_kbd_cngetc(void *v, u_int *type, int *data)
{
	DPRINTF(("%s\n", __func__));
}

void
satopcase_kbd_cnpollc(void *v, int on)
{
	DPRINTF(("%s\n", __func__));
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
			if (event_type == WSCONS_EVENT_KEY_DOWN)
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
satopcase_tp_init(struct satopcase_softc *sc)
{
	struct satopcase_spi_pkt *pkt = &sc->sc_write_pkt;
	struct satopcase_spi_msg *msg = &pkt->msg;

	memset(pkt, 0, sizeof(struct satopcase_spi_pkt));

	pkt->device = PACKET_DEVICE_INFO;
	msg->type = MSG_TYPE_TP_INFO;
	msg->type2 = MSG_TYPE2_TP_INFO;
	msg->counter = sc->sc_pkt_counter++;
	msg->response_length = htole16(SATOPCASE_PACKET_SIZE * 2);
	msg->length = htole16(sizeof(struct satopcase_tp_info_cmd) - 2);
	msg->tp_info_cmd.crc16 = htole16(satopcase_crc16((uint8_t *)msg,
	    MSG_HEADER_LEN));
	satopcase_send_msg(sc, sizeof(struct satopcase_tp_info_cmd), 1);

	if (!sc->tp_dev_type) {
		printf("%s: failed to probe touchpad\n", sc->sc_dev.dv_xname);
		/* TODO: wsmouse detach? */
		return;
	}

	/* put the touchpad into multitouch mode */

	memset(pkt, 0, sizeof(struct satopcase_spi_pkt));

	pkt->device = PACKET_DEVICE_TOUCHPAD;
	msg->type = MSG_TYPE_TP_MT;
	msg->counter = sc->sc_pkt_counter++;
	msg->response_length = htole16(SATOPCASE_PACKET_SIZE * 2);
	msg->length = htole16(sizeof(struct satopcase_tp_mt_cmd) - 2);
	msg->tp_mt_cmd.mode = htole16(TP_MT_CMD_MT_MODE);
	msg->tp_mt_cmd.crc16 = htole16(satopcase_crc16((uint8_t *)msg,
	    MSG_HEADER_LEN + sizeof(struct satopcase_tp_mt_cmd) - 2));
	satopcase_send_msg(sc, sizeof(struct satopcase_tp_mt_cmd), 1);
}

int
satopcase_tp_enable(void *v)
{
	struct satopcase_softc *sc = v;
	struct wsmousehw *hw = wsmouse_get_hw(sc->sc_wsmousedev);

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));

	KASSERT(sc->tp_dev_type != NULL);

	hw->type = WSMOUSE_TYPE_TOUCHPAD;
	hw->hw_type = WSMOUSEHW_CLICKPAD;
	hw->x_min = sc->tp_dev_type->l_x.min;
	hw->x_max = sc->tp_dev_type->l_x.max;
	hw->y_min = sc->tp_dev_type->l_y.min;
	hw->y_max = sc->tp_dev_type->l_y.max;
	hw->mt_slots = TP_MAX_FINGERS;
	hw->flags = WSMOUSEHW_MT_TRACKING;

	wsmouse_configure(sc->sc_wsmousedev, NULL, 0);

	return 0;
}

void
satopcase_tp_disable(void *v)
{
	struct satopcase_softc *sc = v;

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));
}

int
satopcase_tp_ioctl(void *v, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	struct satopcase_softc *sc = v;
	struct wsmouse_calibcoords *wsmc = (struct wsmouse_calibcoords *)data;
	int wsmode;

	DPRINTF(("%s: %s: cmd 0x%lx\n", sc->sc_dev.dv_xname, __func__, cmd));

	switch (cmd) {
	case WSMOUSEIO_GTYPE: {
		struct wsmousehw *hw = wsmouse_get_hw(sc->sc_wsmousedev);
		*(u_int *)data = hw->type;
		break;
	}

	case WSMOUSEIO_GCALIBCOORDS:
		wsmc->minx = sc->tp_dev_type->l_x.min;
		wsmc->maxx = sc->tp_dev_type->l_x.max;
		wsmc->miny = sc->tp_dev_type->l_y.min;
		wsmc->maxy = sc->tp_dev_type->l_y.max;
		wsmc->swapxy = 0;
		wsmc->resx = 0;
		wsmc->resy = 0;
		break;

	case WSMOUSEIO_SETMODE:
		wsmode = *(u_int *)data;
		if (wsmode != WSMOUSE_COMPAT && wsmode != WSMOUSE_NATIVE) {
			DPRINTF(("%s: invalid mode %d\n", sc->sc_dev.dv_xname,
			    wsmode));
			return EINVAL;
		}
		wsmouse_set_mode(sc->sc_wsmousedev, wsmode);

		DPRINTF(("%s: changing mode to %s\n", sc->sc_dev.dv_xname,
		    (wsmode == WSMOUSE_COMPAT ? "compat" : "native")));

		break;

	default:
		return -1;
	}

	return 0;
}

int
satopcase_send_msg(struct satopcase_softc *sc, size_t len, int wait_reply)
{
	struct satopcase_spi_pkt *packet = &sc->sc_write_pkt;
	int x, tries = 10;

	/* sc->sc_write_raw should have already been zeroed and msg filled in */

	packet->type = PACKET_TYPE_WRITE;
	packet->offset = 0;
	packet->remaining = 0;
	packet->length = htole16(MSG_HEADER_LEN + len);
	packet->crc16 = htole16(satopcase_crc16(sc->sc_write_raw,
	    SATOPCASE_PACKET_SIZE - 2));

	DPRINTF(("%s: outgoing message:", sc->sc_dev.dv_xname));
	for (x = 0; x < SATOPCASE_PACKET_SIZE; x++)
		DPRINTF((" %02x", (((uint8_t *)packet)[x] & 0xff)));
	DPRINTF(("\n"));

	spi_acquire_bus(sc->sc_spi_tag, 0);
	spi_config(sc->sc_spi_tag, &sc->sc_spi_conf);
	spi_write(sc->sc_spi_tag, sc->sc_write_raw,
	    sizeof(struct satopcase_spi_pkt));
	spi_release_bus(sc->sc_spi_tag, 0);

	if (wait_reply) {
		if (cold) {
			do {
				DELAY(10);
				satopcase_intr(sc);
			} while (sc->sc_last_read_error && --tries);
		} else if (tsleep(&sc, PRIBIO, "satopcase", hz / 100) != 0)
			DPRINTF(("%s: timed out waiting for reply\n",
			    sc->sc_dev.dv_xname));
	}

	return 0;
}

void
satopcase_handle_msg(struct satopcase_softc *sc)
{
	uint16_t crc;
	uint16_t msg_crc;
	int x;

	sc->sc_last_read_error = 0;

	DPRINTF(("%s: incoming message:", sc->sc_dev.dv_xname));
	for (x = 0; x < SATOPCASE_PACKET_SIZE; x++)
		DPRINTF((" %02x", (sc->sc_read_raw[x] & 0xff)));
	DPRINTF(("\n"));

	crc = satopcase_crc16(sc->sc_read_raw, SATOPCASE_PACKET_SIZE - 2);
	msg_crc = (sc->sc_read_raw[SATOPCASE_PACKET_SIZE - 1] << 8) |
	    sc->sc_read_raw[SATOPCASE_PACKET_SIZE - 2];
	if (crc != msg_crc) {
		printf("%s: corrupt packet (crc 0x%x != msg crc 0x%x)\n",
		    sc->sc_dev.dv_xname, crc, msg_crc);
		sc->sc_last_read_error = 1;
		return;
	}

	switch (sc->sc_read_pkt.type) {
	case PACKET_TYPE_READ:
		if (sc->sc_read_pkt.remaining || sc->sc_read_pkt.offset) {
			DPRINTF(("%s: remaining %d, offset %d\n",
			    sc->sc_dev.dv_xname, sc->sc_read_pkt.remaining,
			    sc->sc_read_pkt.offset));
		}

		switch (sc->sc_read_pkt.device) {
		case PACKET_DEVICE_KEYBOARD:
			satopcase_kbd_handle_msg(sc, &sc->sc_read_pkt.msg);
			break;
		case PACKET_DEVICE_TOUCHPAD:
			satopcase_tp_handle_msg(sc, &sc->sc_read_pkt.msg);
			break;
		default:
			DPRINTF(("%s: unknown device for read packet: 0x%x\n",
			    sc->sc_dev.dv_xname, sc->sc_read_pkt.device));
			sc->sc_last_read_error = 1;
		}
		break;

	case PACKET_TYPE_WRITE:
		/* command response */
		switch (sc->sc_read_pkt.device) {
		case PACKET_DEVICE_INFO:
			satopcase_info_handle_msg(sc, &sc->sc_read_pkt.msg);
			break;
		case PACKET_DEVICE_TOUCHPAD:
			satopcase_tp_handle_msg(sc, &sc->sc_read_pkt.msg);
			break;
		default:
			DPRINTF(("%s: unknown device for write packet "
			    "response: 0x%x\n", sc->sc_dev.dv_xname,
			    sc->sc_read_pkt.device));
			sc->sc_last_read_error = 1;
		}
		break;
	case PACKET_TYPE_ERROR:
		/*
		 * Response to bogus command, or doing a read when there is
		 * nothing to read (such as when forcing a read while cold and
		 * the corresponding GPE doesn't get serviced until !cold).
		 */
		DPRINTF(("%s: received error packet\n", sc->sc_dev.dv_xname));
		sc->sc_last_read_error = 1;
		break;
	default:
		DPRINTF(("%s: unknown packet type 0x%x\n", sc->sc_dev.dv_xname,
		    sc->sc_read_pkt.type));
		sc->sc_last_read_error = 1;
	}
}

void
satopcase_info_handle_msg(struct satopcase_softc *sc,
    struct satopcase_spi_msg *msg)
{
	switch (le16toh(msg->type)) {
	case MSG_TYPE_TP_INFO: {
		uint16_t model = le16toh(msg->tp_info.model);
		int i;

		for (i = 0; i < nitems(satopcase_tp_devices); i++) {
			if (model == satopcase_tp_devices[i].model) {
				sc->tp_dev_type = &satopcase_tp_devices[i];
				break;
			}
		}

		if (!sc->tp_dev_type) {
			printf("%s: unrecognized device model 0x%04x, "
			    "touchpad coordinates will be wrong\n",
			    sc->sc_dev.dv_xname, model);
			sc->tp_dev_type = &satopcase_tp_devices[0];
		}

		break;
	}
	}
}

void
satopcase_kbd_handle_msg(struct satopcase_softc *sc,
    struct satopcase_spi_msg *msg)
{
	struct satopcase_kbd_data *kbd_msg = &msg->kbd_data;
	int pressed[KBD_DATA_KEYS + KBD_DATA_MODS] = { 0 };
	int x, y, tdown;

	if (le16toh(msg->type) != MSG_TYPE_KBD_DATA) {
		DPRINTF(("%s: unhandled keyboard message type 0x%x\n",
		    sc->sc_dev.dv_xname, le16toh(msg->type)));
		return;
	}

	if (kbd_msg->overflow)
		return;

	/*
	 * We don't get key codes for modifier keys, so turn bits in the
	 * modifiers field into key codes to track pressed state.
	 */
	for (x = 0; x < KBD_DATA_KEYS; x++)
		pressed[x] = kbd_msg->pressed[x];
	for (x = 0; x < KBD_DATA_MODS; x++)
		pressed[KBD_DATA_KEYS + x] = (kbd_msg->modifiers & (1 << x)) ?
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
			    kbd_msg->fn, WSCONS_EVENT_KEY_UP);
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
			satopcase_kbd_proc_event(sc, pressed[x], kbd_msg->fn,
			    WSCONS_EVENT_KEY_DOWN);
	}

	memcpy(sc->kbd_keys_down, pressed, sizeof(sc->kbd_keys_down));
}

void
satopcase_tp_handle_msg(struct satopcase_softc *sc,
    struct satopcase_spi_msg *msg)
{
	int x, s, contacts;
	struct satopcase_tp_finger *finger;

	switch (le16toh(msg->type)) {
	case MSG_TYPE_TP_MT:
		DPRINTF(("%s: got ack for mt mode: 0x%x\n",
		    sc->sc_dev.dv_xname, le16toh(msg->tp_mt_cmd.mode)));
		break;
	case MSG_TYPE_TP_DATA:
		contacts = 0;

		for (x = 0; x < msg->tp_data.fingers; x++) {
			finger = &msg->tp_data.finger_data[x];

			if (letoh16(finger->touch_major) == 0)
				continue; /* finger lifted */

			sc->frame[contacts].x = (int16_t)letoh16(finger->abs_x);
			sc->frame[contacts].y = (int16_t)letoh16(finger->abs_y);
			sc->frame[contacts].pressure = SATOPCASE_TP_DEFAULT_PRESSURE;
			contacts++;
		}

		s = spltty();
		wsmouse_buttons(sc->sc_wsmousedev, !!(msg->tp_data.button));
		wsmouse_mtframe(sc->sc_wsmousedev, sc->frame, contacts);
		wsmouse_input_sync(sc->sc_wsmousedev);
		splx(s);

		break;
	default:
		printf("%s: unhandled tp message type 0x%x\n",
		    sc->sc_dev.dv_xname, le16toh(msg->type));
	}
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
