/* $OpenBSD$ */
/*
 * USB control device driver for LG UltraFine Monitor
 *
 * Copyright (c) 2021 joshua stein <jcs@jcs.org>
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
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/ioctl.h>

#include <machine/bus.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbdivar.h>
#include <dev/usb/usbdevs.h>
#include <dev/usb/usbhid.h>

#include <dev/hid/hid.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsdisplayvar.h>

/* I think 0 is for firmware updates */
#define ULGUF_IFACE_BRIGHTNESS	1
#define ULGUF_IFACE_ALS		2

struct ulguf_softc {
	struct device		sc_dev;
	struct usbd_device	*sc_udev;
	struct usbd_interface	*sc_ibrightness;
	int			sc_repbrightness;
	struct usbd_interface	*sc_ials;
	struct usbd_pipe	*sc_pbrightness;
	struct usbd_pipe	*sc_pals;
	struct usbd_xfer	*sc_xfer;
	int			sc_ifaceno;

	int32_t			sc_minbrightness;
	int32_t			sc_maxbrightness;
	int32_t			sc_curbrightness;
};

int	ulguf_match(struct device *, void *, void *);
void	ulguf_attach(struct device *, struct device *, void *);
int	ulguf_detach(struct device *, int);
int32_t	ulguf_get_brightness(struct ulguf_softc *);
int	ulguf_set_brightness(struct ulguf_softc *, int32_t);

int	ulguf_wscons_get_param(struct wsdisplay_param *);
int	ulguf_wscons_set_param(struct wsdisplay_param *);

static const struct usb_devno ulguf_devs[] = {
	{ USB_VENDOR_LG, USB_PRODUCT_LG_UF21_CONTROL },
};

struct cfdriver ulguf_cd = {
	NULL, "ulguf", DV_DULL
};

const struct cfattach ulguf_ca = {
	sizeof(struct ulguf_softc),
	ulguf_match,
	ulguf_attach,
	ulguf_detach,
};

int
ulguf_match(struct device *parent, void *match, void *aux)
{
	struct usb_attach_arg *uaa = aux;

	if (uaa->iface == NULL)
		return (UMATCH_NONE);

	if (usb_lookup(ulguf_devs, uaa->vendor, uaa->product) != NULL)
		return (UMATCH_VENDOR_PRODUCT);

	return (UMATCH_NONE);
}

int
ulguf_pow(int base, int exp)
{
	int i, div = 1;

	if (exp < 0) {
		div = 0;
		exp = exp * -1;
	}

	for (i = 0; i < exp; i++) {
		if (div)
			base /= 10;
		else
			base *= 10;
	}

	return base;
}

void
ulguf_attach(struct device *parent, struct device *self, void *aux)
{
	struct ulguf_softc *sc = (struct ulguf_softc *)self;
	struct usb_attach_arg *uaa = aux;
	struct usb_hid_descriptor *hid;
	struct hid_data *hd;
	struct hid_item hi;
	usb_endpoint_descriptor_t *ed;
	int err;
	void *desc = NULL;
	int size, colusage = 0;

	sc->sc_udev = uaa->device;

	usbd_claim_iface(sc->sc_udev, 0);
	usbd_claim_iface(sc->sc_udev, ULGUF_IFACE_BRIGHTNESS);
	usbd_claim_iface(sc->sc_udev, ULGUF_IFACE_ALS);

	/* find min/max brightness */
	hid = usbd_get_hid_descriptor(sc->sc_udev,
	    usbd_get_interface_descriptor(
	    &sc->sc_udev->ifaces[ULGUF_IFACE_BRIGHTNESS]));
	if (hid == NULL) {
		printf("%s: no HID descriptor\n", sc->sc_dev.dv_xname);
		return;
	}
	size = UGETW(hid->descrs[0].wDescriptorLength);
	desc = malloc(size, M_USBDEV, M_NOWAIT);
	if (desc == NULL) {
		printf("%s: malloc(%d) failed\n", sc->sc_dev.dv_xname, size);
		return;
	}
	if (usbd_get_report_descriptor(sc->sc_udev, ULGUF_IFACE_BRIGHTNESS,
	    desc, size)) {
		printf("%s: no report descriptor\n", sc->sc_dev.dv_xname);
		free(desc, M_USBDEV, size);
		return;
	}

	hd = hid_start_parse(desc, size, hid_input);
	while (hid_get_item(hd, &hi)) {
		if (hi.kind == hid_collection)
			colusage = hi.usage;
		else if (hi.kind == hid_endcollection)
			colusage = 0;

		if (hi.kind != hid_input ||
		    HID_GET_USAGE_PAGE(hi.usage) != HUP_VESA_VC)
			continue;

		switch (HID_GET_USAGE(hi.usage)) {
		case HUV_BRIGHTNESS: {
			int exp = hid_unit_exp(hi.unit_exponent);
			sc->sc_minbrightness = ulguf_pow(hi.logical_minimum,
			    exp);
			sc->sc_maxbrightness = ulguf_pow(hi.logical_maximum,
			    exp);
			sc->sc_repbrightness = hi.report_ID;
			break;
		}
		}
	}
	hid_end_parse(hd);
	free(desc, M_USBDEV, size);

	if (sc->sc_minbrightness < 1 || sc->sc_maxbrightness < 1) {
		printf("%s: bogus brightness limits (%d, %d)\n",
		    sc->sc_dev.dv_xname, sc->sc_minbrightness,
		    sc->sc_maxbrightness);
		return;
	}

	/* setup brightness pipe */
	ed = usbd_interface2endpoint_descriptor(
	    &sc->sc_udev->ifaces[ULGUF_IFACE_BRIGHTNESS], 0);
	if (ed == NULL) {
		printf("%s: no endpoint descriptor\n", sc->sc_dev.dv_xname);
		return;
	}

	err = usbd_open_pipe(&sc->sc_udev->ifaces[ULGUF_IFACE_BRIGHTNESS],
	    ed->bEndpointAddress, USBD_SHORT_XFER_OK, &sc->sc_pbrightness);
	if (err) {
		printf("%s: setup of pipe failed: %d\n", sc->sc_dev.dv_xname,
		    err);
		return;
	}

	sc->sc_curbrightness = ulguf_get_brightness(sc);

	/* and ALS */
	ed = usbd_interface2endpoint_descriptor(
	    &sc->sc_udev->ifaces[ULGUF_IFACE_ALS], 0);
	if (ed == NULL) {
		printf("%s: no endpoint descriptor\n", sc->sc_dev.dv_xname);
		return;
	}

	err = usbd_open_pipe(&sc->sc_udev->ifaces[ULGUF_IFACE_ALS],
	    ed->bEndpointAddress, USBD_SHORT_XFER_OK, &sc->sc_pbrightness);
	if (err) {
		printf("%s: setup of pipe failed: %d\n", sc->sc_dev.dv_xname,
		    err);
		return;
	}

	/* assume we are the display the user cares about */
	ws_get_param = ulguf_wscons_get_param;
	ws_set_param = ulguf_wscons_set_param;
}

int
ulguf_detach(struct device *self, int flags)
{
	ws_get_param = NULL;
	ws_set_param = NULL;

	return 0;
}

int32_t
ulguf_get_brightness(struct ulguf_softc *sc)
{
	usb_device_request_t req;
	char buf[6] = { 0 };
	usbd_status err;
	int actlen = 0;

	req.bmRequestType = UT_READ_CLASS_INTERFACE;
	req.bRequest = UR_GET_REPORT;
	USETW2(req.wValue, UHID_FEATURE_REPORT, sc->sc_repbrightness);
	USETW(req.wIndex, ULGUF_IFACE_BRIGHTNESS);
	USETW(req.wLength, sizeof(buf));

	err = usbd_do_request_flags(sc->sc_udev, &req, &buf, 0, &actlen,
	    USBD_DEFAULT_TIMEOUT);
	if (err != USBD_NORMAL_COMPLETION && err != USBD_SHORT_XFER) {
		printf("%s: failed getting brightness: %d\n",
		    sc->sc_dev.dv_xname, err);
		return 0;
	}

	return buf[0] + (buf[1] << 8);
}

int
ulguf_set_brightness(struct ulguf_softc *sc, int32_t val)
{
	usb_device_request_t req;
	char buf[6] = { 0 };
	usbd_status err;

	buf[0] = val & 0x00ff;
	buf[1] = (val >> 8) & 0x00ff;

	req.bmRequestType = UT_WRITE_CLASS_INTERFACE;
	req.bRequest = UR_SET_REPORT;
	USETW2(req.wValue, UHID_FEATURE_REPORT, sc->sc_repbrightness);
	USETW(req.wIndex, ULGUF_IFACE_BRIGHTNESS);
	USETW(req.wLength, sizeof(buf));

	err = usbd_do_request(sc->sc_udev, &req, &buf);
	if (err != USBD_NORMAL_COMPLETION) {
		printf("%s: failed setting brightness to %d: %d\n",
		    sc->sc_dev.dv_xname, val, err);
		return -1;
	}

	return 0;
}

int
ulguf_wscons_get_param(struct wsdisplay_param *dp)
{
	struct ulguf_softc *sc = NULL;
	int i;

	if (dp->param != WSDISPLAYIO_PARAM_BRIGHTNESS)
		return -1;

	for (i = 0; i < ulguf_cd.cd_ndevs; i++) {
		if (ulguf_cd.cd_devs[i] != NULL) {
			sc = (struct ulguf_softc *)ulguf_cd.cd_devs[i];
			break;
		}
	}

	if (sc == NULL)
		return -1;

	dp->min = sc->sc_minbrightness;
	dp->max = sc->sc_maxbrightness;
	dp->curval = sc->sc_curbrightness;

	return 0;
}

int
ulguf_wscons_set_param(struct wsdisplay_param *dp)
{
	struct ulguf_softc *sc = NULL;
	int i;

	if (dp->param != WSDISPLAYIO_PARAM_BRIGHTNESS)
		return -1;

	for (i = 0; i < ulguf_cd.cd_ndevs; i++) {
		if (ulguf_cd.cd_devs[i] != NULL) {
			sc = (struct ulguf_softc *)ulguf_cd.cd_devs[i];
			break;
		}
	}

	if (sc == NULL)
		return -1;

	if (dp->curval < sc->sc_minbrightness)
		dp->curval = sc->sc_minbrightness;
	if (dp->curval > sc->sc_maxbrightness)
		dp->curval = sc->sc_maxbrightness;

	ulguf_set_brightness(sc, dp->curval);
	printf("%s: -> %d %d/%d (now %d, %d/%d)\n", __func__, dp->curval,
		dp->min, dp->max,
		ulguf_get_brightness(sc), sc->sc_minbrightness,
		sc->sc_maxbrightness);

	dp->curval = sc->sc_curbrightness = ulguf_get_brightness(sc);
	dp->min = sc->sc_minbrightness;
	dp->max = sc->sc_maxbrightness;

	return 0;
}
