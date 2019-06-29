/* $OpenBSD$ */
/*
 * Apple SPI touchpad driver for Apple "topcase" devices
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
 * Protocol info mostly from ubcmtp(4)
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/stdint.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsmousevar.h>

#include "satopcasevar.h"

/* #define SATCTP_DEBUG */
#define SATCTP_DEBUG

#ifdef SATCTP_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

/* most of these definitions came from ubcmtp(4) */

#define SATCTP_FINGER_ORIENT	16384
#define SATCTP_SN_PRESSURE	45
#define SATCTP_SN_WIDTH		25
#define SATCTP_SN_COORD		250
#define SATCTP_SN_ORIENT	10

struct satctp_finger {
	uint16_t	origin;
	uint16_t	abs_x;
	uint16_t	abs_y;
	uint16_t	rel_x;
	uint16_t	rel_y;
	uint16_t	tool_major;
	uint16_t	tool_minor;
	uint16_t	orientation;
	uint16_t	touch_major;
	uint16_t	touch_minor;
	uint16_t	unused[2];
	uint16_t	pressure;
	/* Use a constant, synaptics-compatible pressure value for now. */
#define SATCTP_DEFAULT_PRESSURE	40
	uint16_t	multi;
	uint16_t	crc16;
} __packed __attribute((aligned(2)));

static struct satctp_dev_type satctp_devices[] = {
	{
		/* MacBook10,1 */
		0x0417,
		{ SATCTP_SN_PRESSURE, 0, 300 },
		{ SATCTP_SN_WIDTH, 0, 2048 },
		{ SATCTP_SN_COORD, -5087, 5579 },
		{ SATCTP_SN_COORD, -182, 6089 },
		{ SATCTP_SN_ORIENT, -SATCTP_FINGER_ORIENT,
		    SATCTP_FINGER_ORIENT },
	},
};

int	satctp_match(struct device *, void *, void *);
void	satctp_attach(struct device *, struct device *, void *);
int	satctp_detach(struct device *, int);

void	satctp_init(struct satctp_softc *);
int	satctp_enable(void *);
int	satctp_ioctl(void *, u_long, caddr_t, int, struct proc *);
void	satctp_disable(void *);

struct cfattach satctp_ca = {
	sizeof(struct satctp_softc),
	satctp_match,
	satctp_attach,
	NULL,
	NULL
};

struct cfdriver satctp_cd = {
	NULL, "satctp", DV_DULL
};

struct wsmouse_accessops satctp_accessops = {
	satctp_enable,
	satctp_ioctl,
	satctp_disable,
};

int
satctp_match(struct device *parent, void *match, void *aux)
{
	struct satopcase_attach_args *sa = aux;

	if (strcmp(sa->sa_name, "satctp") == 0)
		return 1;

	return 0;
}

void
satctp_attach(struct device *parent, struct device *self, void *aux)
{
	struct satctp_softc *sc = (struct satctp_softc *)self;
	struct satopcase_attach_args *sa = aux;
	struct wsmousedev_attach_args wmaa;

	sc->sc_satopcase = sa->sa_satopcase;

	printf("\n");

	memset(&wmaa, 0, sizeof(wmaa));
	wmaa.accessops = &satctp_accessops;
	wmaa.accesscookie = sc;
	sc->sc_wsmousedev = config_found(self, &wmaa, wsmousedevprint);

	/*
	 * satopcase would set this itself once we return, but we need packet
	 * responses before we finish attaching, so jump the gun.
	 */
	sc->sc_satopcase->sc_satctp = sc;

	satctp_init(sc);
}

void
satctp_init(struct satctp_softc *sc)
{
	struct satopcase_spi_pkt pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.device = SATOPCASE_PACKET_DEVICE_INFO;
	pkt.msg.response_length = htole16(SATOPCASE_PACKET_SIZE * 2);
	pkt.msg.type = SATOPCASE_MSG_TYPE_TP_INFO;
	pkt.msg.type2 = SATOPCASE_MSG_TYPE2_TP_INFO;

	/*
	 * Send the info request, wait for the response, and the controller
	 * will call satctp_recv_info, filling in our dev_type.
	 */
	satopcase_send_msg(sc->sc_satopcase, &pkt,
	    sizeof(struct satctp_info_cmd), 1);

	if (!sc->dev_type) {
		printf("%s: failed to probe touchpad\n", sc->sc_dev.dv_xname);
		/* TODO: wsmouse detach? */
		return;
	}

	/* now put the touchpad into multitouch mode */
	memset(&pkt, 0, sizeof(pkt));
	pkt.device = SATOPCASE_PACKET_DEVICE_TOUCHPAD;
	pkt.msg.type = SATOPCASE_MSG_TYPE_TP_MT;
	pkt.msg.tp_mt_cmd.mode = htole16(SATCTP_MT_CMD_MT_MODE);

	satopcase_send_msg(sc->sc_satopcase, &pkt,
	    sizeof(struct satctp_mt_cmd), 1);
}

int
satctp_enable(void *v)
{
	struct satctp_softc *sc = v;
	struct wsmousehw *hw = wsmouse_get_hw(sc->sc_wsmousedev);

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));

	KASSERT(sc->dev_type != NULL);

	hw->type = WSMOUSE_TYPE_TOUCHPAD;
	hw->hw_type = WSMOUSEHW_CLICKPAD;
	hw->x_min = sc->dev_type->l_x.min;
	hw->x_max = sc->dev_type->l_x.max;
	hw->y_min = sc->dev_type->l_y.min;
	hw->y_max = sc->dev_type->l_y.max;
	hw->mt_slots = SATCTP_MAX_FINGERS;
	hw->flags = WSMOUSEHW_MT_TRACKING;

	wsmouse_configure(sc->sc_wsmousedev, NULL, 0);

	return 0;
}

void
satctp_disable(void *v)
{
	struct satctp_softc *sc = v;

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));
}

int
satctp_ioctl(void *v, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	struct satctp_softc *sc = v;
	struct wsmouse_calibcoords *wsmc = (struct wsmouse_calibcoords *)data;
	int wsmode;

	switch (cmd) {
	case WSMOUSEIO_GTYPE: {
		struct wsmousehw *hw = wsmouse_get_hw(sc->sc_wsmousedev);
		*(u_int *)data = hw->type;
		break;
	}

	case WSMOUSEIO_GCALIBCOORDS:
		wsmc->minx = sc->dev_type->l_x.min;
		wsmc->maxx = sc->dev_type->l_x.max;
		wsmc->miny = sc->dev_type->l_y.min;
		wsmc->maxy = sc->dev_type->l_y.max;
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

void
satctp_recv_info(struct satctp_softc *sc, struct satopcase_spi_msg *msg)
{
	switch (le16toh(msg->type)) {
	case SATOPCASE_MSG_TYPE_TP_INFO: {
		uint16_t model = le16toh(msg->tp_info.model);
		int i;

		for (i = 0; i < nitems(satctp_devices); i++) {
			if (model == satctp_devices[i].model) {
				sc->dev_type = &satctp_devices[i];
				DPRINTF(("%s: touchpad device is type 0x%04x\n",
				    sc->sc_dev.dv_xname, model));
				break;
			}
		}

		if (!sc->dev_type) {
			printf("%s: unrecognized device model 0x%04x, "
			    "touchpad coordinates will be wrong\n",
			    sc->sc_dev.dv_xname, model);
			sc->dev_type = &satctp_devices[0];
		}
	}
		break;
	default:
		DPRINTF(("%s: unhandled info type 0x%x\n", sc->sc_dev.dv_xname,
		    le16toh(msg->type)));
	}
}

void
satctp_recv_msg(struct satctp_softc *sc, struct satopcase_spi_msg *msg)
{
	int x, s, contacts;
	struct satctp_finger *finger;

	switch (le16toh(msg->type)) {
	case SATOPCASE_MSG_TYPE_TP_MT:
		DPRINTF(("%s: got ack for mt mode: 0x%x\n",
		    sc->sc_dev.dv_xname, le16toh(msg->tp_mt_cmd.mode)));
		break;
	case SATOPCASE_MSG_TYPE_TP_DATA:
		contacts = 0;

		for (x = 0; x < msg->tp_data.fingers; x++) {
			finger = (struct satctp_finger *)&msg->tp_data.
			    finger_data[x * sizeof(struct satctp_finger)];

			if (letoh16(finger->touch_major) == 0)
				continue; /* finger lifted */

			sc->frame[contacts].x = (int16_t)letoh16(finger->abs_x);
			sc->frame[contacts].y = (int16_t)letoh16(finger->abs_y);
			sc->frame[contacts].pressure = SATCTP_DEFAULT_PRESSURE;
			contacts++;
		}

		DPRINTF(("%s: data: fingers:%d contacts:%d button:%d\n",
		    sc->sc_dev.dv_xname, msg->tp_data.fingers, contacts,
		    msg->tp_data.button));

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
