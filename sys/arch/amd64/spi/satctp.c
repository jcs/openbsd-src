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
/* #define SATCTP_FINGER_DEBUG */

#ifdef SATCTP_FINGER_DEBUG
#ifndef SATCTP_DEBUG
#define SATCTP_DEBUG
#endif
#endif

#ifdef SATCTP_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

struct satctp_finger {
	int16_t		origin;
	int16_t		abs_x;
	int16_t		abs_y;
	int16_t		rel_x;
	int16_t		rel_y;
	int16_t		tool_major;
	int16_t		tool_minor;
	int16_t		orientation;
	int16_t		touch_major;
	int16_t		touch_minor;
	int16_t		unused[2];
	int16_t		pressure;
	/* Use a constant, synaptics-compatible pressure value for now. */
#define SATCTP_DEFAULT_PRESSURE	40
	int16_t		multi;
	int16_t		crc16;
} __packed __attribute((aligned(2)));

static struct satctp_dev_type satctp_devices[] = {
	{
		/* MacBookPro12,1 - normally USB-attached */
		0x03df,
		{ -4828, 5345}, /* x */
		{ -203, 6803}, /* y */
	},
	{
		/* MacBook10,1 */
		0x0417,
		{ -5087, 5579 },
		{ -182, 6089 },
	},
};

int	satctp_match(struct device *, void *, void *);
void	satctp_attach(struct device *, struct device *, void *);
int	satctp_detach(struct device *, int);

int	satctp_init(struct satctp_softc *);
void	satctp_configure(struct satctp_softc *);
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

	/* satopcase needs to know how to reach us before we finish attaching */
	sc->sc_satopcase->sc_satctp = sc;

	if (satctp_init(sc) != 0)
		return;

	memset(&wmaa, 0, sizeof(wmaa));
	wmaa.accessops = &satctp_accessops;
	wmaa.accesscookie = sc;
	sc->sc_wsmousedev = config_found(self, &wmaa, wsmousedevprint);

	satctp_configure(sc);
}

int
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
	if (satopcase_send_msg(sc->sc_satopcase, &pkt,
	    sizeof(struct satctp_info_cmd), 1) != 0 || !sc->dev_type.model) {
		printf(": failed to probe touchpad\n");
		return 1;
	}

	printf(": model %04x\n", sc->dev_type.model);

	/* now put the touchpad into multitouch mode */
	memset(&pkt, 0, sizeof(pkt));
	pkt.device = SATOPCASE_PACKET_DEVICE_TOUCHPAD;
	pkt.msg.type = SATOPCASE_MSG_TYPE_TP_MT;
	pkt.msg.tp_mt_cmd.mode = htole16(SATCTP_MT_CMD_MT_MODE);

	if (satopcase_send_msg(sc->sc_satopcase, &pkt,
	    sizeof(struct satctp_mt_cmd), 1) != 0) {
		printf("%s: failed switch to MT mode\n", sc->sc_dev.dv_xname);
		return 1;
	}

	return 0;
}

void
satctp_configure(struct satctp_softc *sc)
{
	struct wsmousehw *hw = wsmouse_get_hw(sc->sc_wsmousedev);

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));

	hw->type = WSMOUSE_TYPE_TOUCHPAD;
	hw->hw_type = WSMOUSEHW_CLICKPAD;
	hw->x_min = sc->dev_type.x.min;
	hw->x_max = sc->dev_type.x.max;
	hw->y_min = sc->dev_type.y.min;
	hw->y_max = sc->dev_type.y.max;
	hw->mt_slots = SATCTP_MAX_FINGERS;
	hw->flags = WSMOUSEHW_MT_TRACKING;

	wsmouse_configure(sc->sc_wsmousedev, NULL, 0);
}

int
satctp_enable(void *v)
{
#ifdef SATCTP_DEBUG
	struct satctp_softc *sc = v;

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));
#endif
	return 0;
}

void
satctp_disable(void *v)
{
#ifdef SATCTP_DEBUG
	struct satctp_softc *sc = v;

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));
#endif
}

int
satctp_ioctl(void *v, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	struct satctp_softc *sc = v;
	struct wsmouse_calibcoords *wsmc = (struct wsmouse_calibcoords *)data;
	int wsmode;

	if (!sc->sc_wsmousedev)
		return -1;

	switch (cmd) {
	case WSMOUSEIO_GTYPE: {
		struct wsmousehw *hw = wsmouse_get_hw(sc->sc_wsmousedev);
		*(u_int *)data = hw->type;
		break;
	}

	case WSMOUSEIO_GCALIBCOORDS:
		wsmc->minx = sc->dev_type.x.min;
		wsmc->maxx = sc->dev_type.x.max;
		wsmc->miny = sc->dev_type.y.min;
		wsmc->maxy = sc->dev_type.y.max;
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
				memcpy(&sc->dev_type, &satctp_devices[i],
				    sizeof(struct satctp_dev_type));
				DPRINTF(("%s: touchpad device is type 0x%04x\n",
				    sc->sc_dev.dv_xname, model));
				break;
			}
		}

		if (!sc->dev_type.model) {
			printf(": unrecognized device model");
			sc->dev_type.model = model;
			/* shrug */
			sc->dev_type.x.min = -5000;
			sc->dev_type.x.max = 5000;
			sc->dev_type.y.min = -200;
			sc->dev_type.y.max = 6000;
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
	int x, s, contacts, xpct, ypct, palm;
	struct satctp_finger *finger;

	switch (le16toh(msg->type)) {
	case SATOPCASE_MSG_TYPE_TP_MT:
		DPRINTF(("%s: got ack for mt mode: 0x%x\n",
		    sc->sc_dev.dv_xname, le16toh(msg->tp_mt_cmd.mode)));
		break;
	case SATOPCASE_MSG_TYPE_TP_DATA:
		if (!sc->sc_wsmousedev)
			return;

		for (x = 0, contacts = 0, palm = 0; x < msg->tp_data.fingers;
		    x++) {
			finger = (struct satctp_finger *)&msg->tp_data.
			    finger_data[x * sizeof(struct satctp_finger)];

#ifdef SATCTP_FINGER_DEBUG
			DPRINTF(("%s: finger[%d] origin:%d abs_x:%d abs_y:%d "
			    "rel_x:%d rel_y:%d tool_major:%d tool_minor:%d "
			    "orientation:%d touch_major:%d touch_minor:%d "
			    "pressure:%d multi:%d",
			    sc->sc_dev.dv_xname, x,
			    (int16_t)letoh16(finger->origin),
			    (int16_t)letoh16(finger->abs_x),
			    (int16_t)letoh16(finger->abs_y),
			    (int16_t)letoh16(finger->rel_x),
			    (int16_t)letoh16(finger->rel_y),
			    (int16_t)letoh16(finger->tool_major),
			    (int16_t)letoh16(finger->tool_minor),
			    (int16_t)letoh16(finger->orientation),
			    (int16_t)letoh16(finger->touch_major),
			    (int16_t)letoh16(finger->touch_minor),
			    (int16_t)letoh16(finger->pressure),
			    (int16_t)letoh16(finger->multi)));
#endif

			if (finger->touch_major == 0 || finger->pressure == 0) {
				/* finger lifted */
#ifdef SATCTP_FINGER_DEBUG
				DPRINTF((" (lifted)\n"));
#endif
				continue;
			}

			if ((int16_t)letoh16(finger->orientation) != 16384) {
				/*
				 * Not a point, check location for palm
				 * rejection
				 */

				/* ((x + abs(min)) * 100) / (abs(min) + max) */
				xpct = (((int16_t)letoh16(finger->abs_x) +
				    abs(sc->dev_type.x.min)) * 100) /
				    (abs(sc->dev_type.x.min) +
				    sc->dev_type.x.max);
				ypct = (((int16_t)letoh16(finger->abs_y) +
				    abs(sc->dev_type.y.min)) * 100) /
				    (abs(sc->dev_type.y.min) +
				    sc->dev_type.y.max);
#ifdef SATCTP_FINGER_DEBUG
				DPRINTF((" xpct:%d ypct:%d", xpct, ypct));
#endif

				if (xpct >= 75 && ypct <= 50) {
					/*
					 * Non-point in the lower half of the
					 * right quarter = probably a palm,
					 * reject
					 */
#ifdef SATCTP_FINGER_DEBUG
					DPRINTF((" (palm area)\n"));
#endif
					/*
					 * We can cancel this touch but there
					 * is often another touch registered
					 * a different, bogus coordinate, so
					 * cancel that one too.
					 */
					palm = 1;
					break;
				}
			}

			DPRINTF(("\n"));

			sc->frame[contacts].x = (int16_t)letoh16(finger->abs_x);
			sc->frame[contacts].y = (int16_t)letoh16(finger->abs_y);
			sc->frame[contacts].pressure = SATCTP_DEFAULT_PRESSURE;
			contacts++;
		}

		DPRINTF(("%s: data: fingers:%d contacts:%d button:%d palm:%d\n",
		    sc->sc_dev.dv_xname, msg->tp_data.fingers, contacts,
		    msg->tp_data.button, palm));

		s = spltty();
		wsmouse_buttons(sc->sc_wsmousedev, !!(msg->tp_data.button));
		if (!palm)
			wsmouse_mtframe(sc->sc_wsmousedev, sc->frame, contacts);
		wsmouse_input_sync(sc->sc_wsmousedev);
		splx(s);

		break;
	default:
		printf("%s: unhandled tp message type 0x%x\n",
		    sc->sc_dev.dv_xname, le16toh(msg->type));
	}
}
