/* $OpenBSD$ */

/*
 * Chrome EC - LPC interface, only supports ECs that speak version 3, and
 * lightbars that speak version 1
 *
 * Copyright (c) 2016 joshua stein <jcs@openbsd.org>
 * Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *    * Neither the name of Google Inc. nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/conf.h>
#include <sys/ioctl.h>

#include <machine/bus.h>
#include <machine/chromeecvar.h>

/* #define CHROMEEC_DEBUG */

#ifdef CHROMEEC_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

#define EC_LPC_ADDR_HOST_DATA		0x200
#define EC_LPC_ADDR_HOST_CMD		0x204
#define EC_LPC_ADDR_HOST_ARGS		0x800
#define EC_LPC_ADDR_HOST_PARAM		0x804

#define EC_LPC_ADDR_HOST_PACKET		0x800
#define EC_LPC_HOST_PACKET_SIZE		0x100

#define EC_ADDR				0x900
#define EC_SIZE				255

#define EC_ID				0x20 /* "E", 0x21 "C" */
#define EC_ID_VERSION			0x22

#define EC_COMMAND_PROTOCOL_3		0xda

#define EC_LPC_STATUS_TO_HOST		0x01
#define EC_LPC_STATUS_FROM_HOST		0x02
#define EC_LPC_STATUS_PROCESSING	0x04
#define EC_LPC_STATUS_LAST_CMD		0x08
#define EC_LPC_STATUS_BURST_MODE	0x10
#define EC_LPC_STATUS_SCI_PENDING	0x20
#define EC_LPC_STATUS_SMI_PENDING	0x40

/* internal structure just to pass messages to chromeec_send_message */
struct chromeec_message {
	uint32_t command;
	uint8_t command_version;
	uint32_t params_size;
	uint8_t *params;
	uint32_t response_size;
	uint32_t result;
};

/* packed structures for writing to and reading from the device */
struct ec_host_request {
	uint8_t struct_version;
	uint8_t checksum;
	uint16_t command;
	uint8_t command_version;
	uint8_t reserved;
	uint16_t data_len;
} __packed;

struct ec_host_response {
	uint8_t struct_version;
	uint8_t checksum;
	uint16_t result;
	uint16_t data_len;
	uint16_t reserved;
} __packed;

#define EC_CMD_GET_VERSION		0x02
struct ec_response_get_version {
	char		version_string_ro[32];
	char		version_string_rw[32];
	char		reserved[32];
	uint32_t	current_image;
} __packed;

#define EC_CMD_GET_PROTOCOL_INFO	0x0b
struct ec_response_get_protocol_info {
	uint32_t	protocol_versions;
	uint16_t	max_request_packet_size;
	uint16_t	max_response_packet_size;
	uint32_t	flags;
} __packed;

#define EC_CMD_LIGHTBAR_CMD		0x28
struct ec_params_lightbar {
	uint8_t cmd;
# define LIGHTBAR_CMD_DUMP		0
# define LIGHTBAR_CMD_OFF		1
# define LIGHTBAR_CMD_ON		2
# define LIGHTBAR_CMD_INIT		3
# define LIGHTBAR_CMD_SET_BRIGHTNESS	4
# define LIGHTBAR_CMD_SEQ		5
# define LIGHTBAR_CMD_REG		6
# define LIGHTBAR_CMD_SET_RGB		7
# define LIGHTBAR_CMD_GET_SEQ		8
# define LIGHTBAR_CMD_DEMO		9
# define LIGHTBAR_CMD_VERSION		12
# define LIGHTBAR_CMD_GET_BRIGHTNESS	13
# define LIGHTBAR_CMD_GET_RGB		14
# define LIGHTBAR_CMD_GET_DEMO		15
# define LIGHTBAR_CMD_GET_PARAMS_V1	16
# define LIGHTBAR_CMD_SET_PARAMS_V1	17
# define LIGHTBAR_CMD_SET_PROGRAM	18
	union {
		struct {
			uint8_t num;
		} set_brightness, seq, demo;

		struct {
			uint8_t ctrl, reg, value;
		} reg;

		struct {
			uint8_t led, red, green, blue;
		} set_rgb;

		struct {
			uint8_t led;
		} get_rgb;

		struct chromeec_lightbar_program set_program;
		struct chromeec_lightbar_params_v1 set_params_v1;
	};
} __packed;

struct ec_response_lightbar {
	union {
		struct {
			struct {
				uint8_t reg;
				uint8_t ic0;
				uint8_t ic1;
			} vals[23];
		} dump;

		struct  {
			uint8_t num;
		} get_seq, get_brightness, get_demo;

		struct {
			uint32_t num;
			uint32_t flags;
		} version;

		struct {
			uint8_t red;
			uint8_t green;
			uint8_t blue;
		} get_rgb;

		struct chromeec_lightbar_params_v1 get_params_v1;
	};
} __packed;

#define EC_CMD_SWITCH_ENABLE_WIRELESS	0x91
struct ec_params_switch_enable_wireless_v0 {
	uint8_t enabled;
# define EC_WIRELESS_SWITCH_WLAN	0x01  /* WLAN radio */
# define EC_WIRELESS_SWITCH_BLUETOOTH	0x02  /* Bluetooth radio */
# define EC_WIRELESS_SWITCH_WWAN	0x04  /* WWAN power */
# define EC_WIRELESS_SWITCH_WLAN_POWER	0x08  /* WLAN power */
} __packed;

struct ec_response_switch_enable_wireless_v1 {
	uint8_t now_flags;
	uint8_t suspend_flags;
} __packed;

struct chromeec_softc {
	struct device	sc_dev;
	struct rwlock	sc_lock;
	int		sc_lightbar;

	uint32_t	request_data_size;
	uint8_t		*request_data;
	uint32_t	response_data_size;
	uint8_t		*response_data;
};

/* there can be only one */
struct chromeec_softc *chromeec_softc;

int	chromeec_match(struct device *, void *, void *);
void	chromeec_attach(struct device *, struct device *, void *);
int	chromeec_lightbar_cmd(struct ec_params_lightbar, int);
int	chromeec_init_lightbar(void);
int	chromeec_wireless_enable(struct chromeec_softc *, int);

int	chromeec_send_message(struct chromeec_message *);
int	chromeec_wait_ready(uint16_t);

int	chromeecopen(dev_t, int, int, struct proc *);
int	chromeecclose(dev_t, int, int, struct proc *);
int	chromeecioctl(dev_t, u_long, caddr_t, int, struct proc *);

const struct cfattach chromeec_ca = {
	sizeof(struct chromeec_softc),
	chromeec_match,
	chromeec_attach,
	NULL
};

struct cfdriver chromeec_cd = {
	NULL, "chromeec", DV_DULL
};

/* whitelist of machines we'll probe on, just to avoid trouble */
static const struct chromeec_machine {
	const char *vendor;
	const char *product;
} chromeec_machines[] = {
	{ "GOOGLE", "Samus" },
	{ NULL }
};

extern char *hw_vendor, *hw_prod;

int
chromeec_probe(void)
{
	const struct chromeec_machine *m;

	if (hw_vendor == NULL || hw_prod == NULL)
		return (0);

	for (m = chromeec_machines; m->vendor != NULL; m++) {
		if (strcmp(hw_vendor, m->vendor) == 0 &&
		    strcmp(hw_prod, m->product) == 0)
			return (1);
	}

	return (0);
}

int
chromeec_match(struct device *parent, void *cf, void *aux)
{
	struct chromeec_attach_args *checaa = aux;

	if (strcmp(checaa->checaa_name, chromeec_cd.cd_name) != 0)
		return (0);

	return chromeec_probe();
}

void
chromeec_attach(struct device *parent, struct device *self, void *aux)
{
	struct chromeec_softc *sc = (struct chromeec_softc *)self;
	struct chromeec_message msg = { 0 };
	struct ec_response_get_version *ver;
	struct ec_response_get_protocol_info proto = { 0 };
	bus_space_handle_t ioh;

	/*
	 * Just to make sure nothing else is using this range before we start
	 * poking it.
	 */
	if (bus_space_map(X86_BUS_SPACE_IO, EC_ADDR, EC_SIZE, 0, &ioh) != 0) {
		printf(": failed mapping at 0x%x\n", EC_ADDR);
		return;
	}
	bus_space_unmap(X86_BUS_SPACE_IO, ioh, EC_SIZE);

	if (inb(EC_ADDR + EC_ID) != 'E' || inb(EC_ADDR + EC_ID + 1) != 'C') {
		printf(": couldn't find EC at 0x%x\n", EC_ADDR);
		return;
	}

	chromeec_softc = sc;

	/* until we see otherwise */
	sc->request_data_size = sizeof(struct ec_host_request);
	sc->request_data = malloc(sc->request_data_size, M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	sc->response_data_size = sizeof(struct ec_host_response) +
	    sizeof(struct ec_response_get_protocol_info);
	sc->response_data = malloc(sc->response_data_size, M_DEVBUF,
	    M_NOWAIT | M_ZERO);

	/* make sure the ec supports version 3+ */
	msg.command = EC_CMD_GET_PROTOCOL_INFO;
	msg.response_size = sizeof(struct ec_response_get_protocol_info);
	if (chromeec_send_message(&msg) != 0) {
		printf(": failed with v3 protocol\n");
		return;
	}

	/* resize device buffers based on what we got back */
	memcpy(&proto, sc->response_data, sizeof(proto));

	free(sc->request_data, M_DEVBUF, sc->request_data_size);
	free(sc->response_data, M_DEVBUF, sc->response_data_size);

	sc->request_data_size = proto.max_request_packet_size;
	sc->request_data = malloc(sc->request_data_size, M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	sc->response_data_size = proto.max_response_packet_size;
	sc->response_data = malloc(sc->response_data_size, M_DEVBUF,
	    M_NOWAIT | M_ZERO);

	DPRINTF(("%s: resizing request data to %d, response to %d\n",
	    chromeec_softc->sc_dev.dv_xname, sc->request_data_size,
	    sc->response_data_size));

	/* get ec info */
	bzero(&msg, sizeof(msg));
	msg.command = EC_CMD_GET_VERSION;
	msg.response_size = sizeof(struct ec_response_get_version);
	if (chromeec_send_message(&msg) != 0) {
		printf(": failed getting version info\n");
		return;
	}

	ver = (struct ec_response_get_version *)sc->response_data;
	if (ver->current_image == 1)
		printf(": %s", ver->version_string_ro);
	else if (ver->version_string_rw[0])
		printf(": %s", ver->version_string_rw[0]);
	else
		printf(": unknown image (%d)", ver->current_image);

	if (chromeec_init_lightbar())
		printf(", lightbar");

	printf("\n");

#if 0
	/* try to shut down bluetooth since we can't use it anyway */
	chromeec_wireless_enable(sc, EC_WIRELESS_SWITCH_WLAN |
	    EC_WIRELESS_SWITCH_WLAN);
#endif

	rw_init(&sc->sc_lock, sc->sc_dev.dv_xname);
}

int
chromeec_lightbar_cmd(struct ec_params_lightbar params, int resp)
{
	struct chromeec_message msg = { 0 };
	int ret;

	if (chromeec_softc->sc_lightbar == 0)
		return ENXIO;

	msg.command = EC_CMD_LIGHTBAR_CMD;
	msg.command_version = 0;
	msg.params = (uint8_t *)&params;
	msg.params_size = sizeof(params);
	//if (resp)
		msg.response_size = sizeof(struct ec_response_lightbar);

	ret = chromeec_send_message(&msg);
	if (ret != 0 || msg.result != 0) {
		DPRINTF(("%s: lightbar command ret %d result %d\n",
		    chromeec_softc->sc_dev.dv_xname, ret, msg.result));
		return ENXIO;
	}

	return 0;
}

int
chromeec_init_lightbar(void)
{
	struct ec_params_lightbar params = { 0 };
	struct ec_response_lightbar *resp;
	struct chromeec_lightbar_params_v1 *ps;

	chromeec_softc->sc_lightbar = 1;

	params.cmd = LIGHTBAR_CMD_VERSION;
	if (chromeec_lightbar_cmd(params, 1) != 0) {
		chromeec_softc->sc_lightbar = 0;
		return 0;
	}

	resp = (struct ec_response_lightbar *)chromeec_softc->response_data;
	if (resp->version.num != 1) {
		DPRINTF(("%s: lightbar version %d != 1\n",
		    chromeec_softc->sc_dev.dv_xname, resp->version.num));
		chromeec_softc->sc_lightbar = 0;
		return 0;
	}

	/* re-init */
	bzero(&params, sizeof(params));
	params.cmd = LIGHTBAR_CMD_INIT;
	if (chromeec_lightbar_cmd(params, 0))
		return 1;

	/* take out of demo mode */
	bzero(&params, sizeof(params));
	params.cmd = LIGHTBAR_CMD_DEMO;
	params.demo.num = 0;
	if (chromeec_lightbar_cmd(params, 0))
		return 1;

	/* highest brightness */
	bzero(&params, sizeof(params));
	params.cmd = LIGHTBAR_CMD_SET_BRIGHTNESS;
	params.set_brightness.num = 255;
	if (chromeec_lightbar_cmd(params, 0))
		return 1;

	/* tweak some params */
	bzero(&params, sizeof(params));
	params.cmd = LIGHTBAR_CMD_GET_PARAMS_V1;
	if (chromeec_lightbar_cmd(params, 0))
		return 1;
	ps = (struct chromeec_lightbar_params_v1 *)chromeec_softc->response_data;

	/* enable fast s3 pulsing using color 5 when low/dead, 4 otherwise */
	ps->s3_sleep_for = 100;
	ps->s3_ramp_up = 20000;
	ps->s3_ramp_down = 15000;
	ps->s0_idx[0][0] = ps->s0_idx[1][0] = 5;
	ps->s0_idx[0][1] = ps->s0_idx[1][1] = 5;
	ps->s0_idx[0][2] = ps->s0_idx[1][2] = 4;
	ps->s0_idx[0][3] = ps->s0_idx[1][3] = 4;

	/* un-google-ify */
	ps->color[0].r = ps->color[0].g = ps->color[0].b = 60;
	ps->color[1].r = ps->color[1].g = ps->color[1].b = 100;
	ps->color[2].r = ps->color[2].g = ps->color[2].b = 120;
	ps->color[3].r = ps->color[3].g = ps->color[3].b = 160;

	/* write back */
	bzero(&params, sizeof(params));
	params.cmd = LIGHTBAR_CMD_SET_PARAMS_V1;
	memcpy(&params.set_params_v1, ps,
	    sizeof(struct chromeec_lightbar_params_v1));
	if (chromeec_lightbar_cmd(params, 0))
		return 1;

	/* put in s0 sequence */
	bzero(&params, sizeof(params));
	params.cmd = LIGHTBAR_CMD_SEQ;
	params.seq.num = CHROMEEC_LIGHTBAR_SEQ_S0;
	if (chromeec_lightbar_cmd(params, 0))
		return 1;

	return 1;
}

int
chromeec_wireless_enable(struct chromeec_softc *sc, int flags)
{
	struct ec_params_switch_enable_wireless_v0 params;
	struct chromeec_message msg = { 0 };

	bzero(&params, sizeof(params));
	params.enabled = flags;

	msg.command = EC_CMD_SWITCH_ENABLE_WIRELESS;
	msg.params = (uint8_t *)&params;
	msg.params_size = sizeof(params);
	/* no response for v0 */
	msg.response_size = 0;

	if (chromeec_send_message(&msg) != 0) {
		printf("%s: failed sending wireless command\n",
		    sc->sc_dev.dv_xname);
		return 0;
	}

	return 1;
}

int
chromeec_send_message(struct chromeec_message *msg)
{
	struct ec_host_request *req;
	struct ec_host_response resp;
	uint8_t *out;
	uint8_t csum = 0;
	int i;

	DPRINTF(("%s: %s: command 0x%x, out(%d):",
	    chromeec_softc->sc_dev.dv_xname, __func__, msg->command,
	    msg->params_size));

	bzero(chromeec_softc->request_data,
	    chromeec_softc->request_data_size);
	bzero(chromeec_softc->response_data,
	    chromeec_softc->response_data_size);

	/* build request_data with host request and params */
	req = (struct ec_host_request *)chromeec_softc->request_data;
	req->struct_version = 3;
	req->checksum = 0;
	req->command = msg->command;
	req->command_version = msg->command_version;
	req->reserved = 0;
	req->data_len = msg->params_size;

	/* make sure we can fit the whole request + params into our buffer */
	KASSERT(sizeof(*req) + msg->params_size <=
	    chromeec_softc->request_data_size);

	/*
	 * Calculate checksum, taking into account it's stored in a uint8_t
	 * field so wrapping and negation are expected.
	 */
	for (i = 0; i < sizeof(req); i++)
		csum += chromeec_softc->request_data[i];

	for (i = 0; i < msg->params_size; i++) {
		chromeec_softc->request_data[sizeof(req) + i] = msg->params[i];
		csum += msg->params[i];
	}
	req->checksum = -csum;

	/* now write out the request */
	for (i = 0; i < sizeof(req) + msg->params_size; i++) {
		DPRINTF((" %02x", chromeec_softc->request_data[i]));
		outb(EC_LPC_ADDR_HOST_PACKET + i,
		    chromeec_softc->request_data[i]);
	}

	/* send command */
	DPRINTF((", command 0x%x, checksum %d", EC_COMMAND_PROTOCOL_3,
	    req->checksum));
	outb(EC_LPC_ADDR_HOST_CMD, EC_COMMAND_PROTOCOL_3);

	DPRINTF((", waiting\n"));
	if (chromeec_wait_ready(EC_LPC_ADDR_HOST_CMD)) {
		printf("%s: timed out waiting for ec ready\n",
		    chromeec_softc->sc_dev.dv_xname);
		return 1;
	}

	/* read command result with details about the response */
	msg->result = inb(EC_LPC_ADDR_HOST_DATA);
	DPRINTF(("%s: result 0x%x:", chromeec_softc->sc_dev.dv_xname,
	    msg->result));

	if (msg->result != 0) {
		printf("%s: non-zero result %d to command 0x%x\n",
		    chromeec_softc->sc_dev.dv_xname, msg->result,
		    msg->command);
		return 1;
	}

	csum = 0;
	out = (uint8_t *)&resp;
	for (i = 0; i < sizeof(resp); i++) {
		out[i] = inb(EC_LPC_ADDR_HOST_PACKET + i);
		csum += out[i];
		DPRINTF((" %02x", out[i]));
	}
	DPRINTF(("\n"));

	msg->result = resp.result;

	/* then read the actual response and store it in response_data */
	DPRINTF(("%s: response(%d):", chromeec_softc->sc_dev.dv_xname,
	    resp.data_len));
	for (i = 0; i < resp.data_len; i++) {
		chromeec_softc->response_data[i] =
		    inb(EC_LPC_ADDR_HOST_PACKET + sizeof(resp) + i);
		csum += chromeec_softc->response_data[i];
		DPRINTF((" %02x", chromeec_softc->response_data[i]));
	}
	DPRINTF(("\n"));

	if (csum != 0) {
		printf("%s: invalid packet checksum 0x%x (0x%x)\n",
		    chromeec_softc->sc_dev.dv_xname, resp.checksum, csum);
		return 1;
	}

	return 0;
}

int
chromeec_wait_ready(uint16_t addr)
{
	uint8_t ec_status, last_status = 0;
	int retries = 10000;

	while (retries-- > 0) {
		if ((ec_status = inb(addr)) & (EC_LPC_STATUS_FROM_HOST |
		    EC_LPC_STATUS_PROCESSING))
			DELAY(10);
		else
			return 0;

		last_status = ec_status;
	}

	DPRINTF(("%s: ec status 0x%x, timed out\n",
	    chromeec_softc->sc_dev.dv_xname, ec_status));

	return 1;
}

/* /dev/chromeec interface */

int
chromeecopen(dev_t dev, int flags, int mode, struct proc *p)
{
	if (chromeec_softc == NULL)
		return (ENXIO);

	return (0);
}

int
chromeecclose(dev_t dev, int flags, int mode, struct proc *p)
{
	if (chromeec_softc == NULL)
		return (ENXIO);

	return (0);
}

int
chromeecioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *proc)
{
	struct ec_params_lightbar lb_params;
	int ret, val, resp = 0;

	if (chromeec_softc == NULL)
		return (ENXIO);

	rw_enter_write(&chromeec_softc->sc_lock);

	DPRINTF(("%s: %s: %ld\n", chromeec_softc->sc_dev.dv_xname, __func__,
	    cmd));

	switch (cmd) {
	case CHROMEEC_IOC_LIGHTBAR_SET_POWER:
		val = *(uint8_t *)data;

		if (val == 0)
			lb_params.cmd = LIGHTBAR_CMD_OFF;
		else
			lb_params.cmd = LIGHTBAR_CMD_ON;

		if ((ret = chromeec_lightbar_cmd(lb_params, 0)))
			ret = ENXIO;

		break;

	case CHROMEEC_IOC_LIGHTBAR_INIT:
		lb_params.cmd = LIGHTBAR_CMD_INIT;

		if ((ret = chromeec_lightbar_cmd(lb_params, 0)))
			ret = ENXIO;

		break;

	case CHROMEEC_IOC_LIGHTBAR_GET_BRIGHTNESS:
		lb_params.cmd = LIGHTBAR_CMD_GET_BRIGHTNESS;

		if ((ret = chromeec_lightbar_cmd(lb_params, 1))) {
			ret = ENXIO;
			break;
		}

		*(uint8_t *)data = ((struct ec_response_lightbar *)
		    chromeec_softc->response_data)->get_brightness.num;

		break;

	case CHROMEEC_IOC_LIGHTBAR_SET_BRIGHTNESS:
		lb_params.cmd = LIGHTBAR_CMD_SET_BRIGHTNESS;
		lb_params.set_brightness.num = 0xff & *(uint8_t *)data;

		if ((ret = chromeec_lightbar_cmd(lb_params, resp)))
			ret = ENXIO;

		break;

	case CHROMEEC_IOC_LIGHTBAR_GET_SEQ:
		lb_params.cmd = LIGHTBAR_CMD_GET_SEQ;

		if ((ret = chromeec_lightbar_cmd(lb_params, 1))) {
			ret = ENXIO;
			break;
		}

		*(uint8_t *)data = ((struct ec_response_lightbar *)
		    chromeec_softc->response_data)->get_seq.num;

		break;

	case CHROMEEC_IOC_LIGHTBAR_SET_SEQ:
		val = *(uint8_t *)data;

		lb_params.cmd = LIGHTBAR_CMD_SEQ;
		lb_params.seq.num = val;
		if ((ret = chromeec_lightbar_cmd(lb_params, 0)))
			ret = ENXIO;

		break;

	case CHROMEEC_IOC_LIGHTBAR_GET_RGB: {
		struct chromeec_led_rgb *lrgb = (struct chromeec_led_rgb *)data;

		lb_params.cmd = LIGHTBAR_CMD_GET_RGB;
		lb_params.get_rgb.led = lrgb->led;

		if ((ret = chromeec_lightbar_cmd(lb_params, 1))) {
			ret = ENXIO;
			break;
		}

		struct ec_response_lightbar *erl =
		    ((struct ec_response_lightbar *)
		    chromeec_softc->response_data);

		lrgb->red = erl->get_rgb.red;
		lrgb->green = erl->get_rgb.green;
		lrgb->blue = erl->get_rgb.blue;

		break;
	}
	case CHROMEEC_IOC_LIGHTBAR_SET_RGB: {
		struct chromeec_led_rgb *lrgb = (struct chromeec_led_rgb *)data;

		lb_params.cmd = LIGHTBAR_CMD_SET_RGB;
		lb_params.set_rgb.led = lrgb->led;
		lb_params.set_rgb.red = lrgb->red;
		lb_params.set_rgb.green = lrgb->green;
		lb_params.set_rgb.blue = lrgb->blue;

		if ((ret = chromeec_lightbar_cmd(lb_params, resp)))
			ret = ENXIO;

		break;
	}
	case CHROMEEC_IOC_LIGHTBAR_GET_DEMO:
		lb_params.cmd = LIGHTBAR_CMD_GET_DEMO;

		if ((ret = chromeec_lightbar_cmd(lb_params, 1))) {
			ret = ENXIO;
			break;
		}

		*(uint8_t *)data = ((struct ec_response_lightbar *)
		    chromeec_softc->response_data)->get_demo.num;

		break;

	case CHROMEEC_IOC_LIGHTBAR_SET_DEMO:
		val = *(uint8_t *)data;

		lb_params.cmd = LIGHTBAR_CMD_DEMO;
		lb_params.demo.num = (val ? 1 : 0);

		if ((ret = chromeec_lightbar_cmd(lb_params, 0)))
			ret = ENXIO;

		break;

	case CHROMEEC_IOC_LIGHTBAR_GET_PARAMS_V1: {
		struct chromeec_lightbar_params_v1 *ps =
		    (struct chromeec_lightbar_params_v1 *)data;

		lb_params.cmd = LIGHTBAR_CMD_GET_PARAMS_V1;
		if ((ret = chromeec_lightbar_cmd(lb_params, 1))) {
			ret = ENXIO;
			break;
		}

		memcpy(ps, (struct chromeec_lightbar_params_v1 *)
		    chromeec_softc->response_data,
		    sizeof(struct chromeec_lightbar_params_v1));

		break;
	}
	case CHROMEEC_IOC_LIGHTBAR_SET_PARAMS_V1: {
		struct chromeec_lightbar_params_v1 *ps =
		    (struct chromeec_lightbar_params_v1 *)data;

		lb_params.cmd = LIGHTBAR_CMD_SET_PARAMS_V1;
		memcpy(&lb_params.set_params_v1, ps,
		    sizeof(struct chromeec_lightbar_params_v1));
		if ((ret = chromeec_lightbar_cmd(lb_params, 0))) {
			ret = ENXIO;
			break;
		}

		break;
	}
	case CHROMEEC_IOC_LIGHTBAR_SET_PROGRAM: {
		struct chromeec_lightbar_program *pg =
		    (struct chromeec_lightbar_program *)data;
		int j;

		lb_params.cmd = LIGHTBAR_CMD_SET_PROGRAM;
		memcpy(&lb_params.set_program, pg,
		    sizeof(struct chromeec_lightbar_program));

		printf("loading program of size %d:\n",
			lb_params.set_program.size);
		for (j = 0; j < sizeof(struct chromeec_lightbar_program); j++) {
			printf(" %02x", lb_params.set_program.data[j]);
		}
		printf("\n");

		if ((ret = chromeec_lightbar_cmd(lb_params, 0))) {
			ret = ENXIO;
			break;
		}

		break;
	}
	default:
		printf("%s: ioctl 0x%lx\n", chromeec_softc->sc_dev.dv_xname,
		    cmd);
		ret = ENOTTY;
		break;
	}

	rw_exit_write(&chromeec_softc->sc_lock);
	return (ret);
}
