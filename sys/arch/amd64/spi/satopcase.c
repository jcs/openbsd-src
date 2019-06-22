/* $OpenBSD$ */
/*
 * Apple SPI "topcase" controller driver
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
 * Protocol info from macbook12-spi-driver Linux driver by Federico Lorenzi,
 * Ronald Tschalär, et al.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/stdint.h>

#include "satopcasevar.h"

/* #define SATOPCASE_FORCE_ATTACH */

/* #define SATOPCASE_DEBUG */

#ifdef SATOPCASE_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

int	satopcase_match(struct device *, void *, void *);
void	satopcase_attach(struct device *, struct device *, void *);
int	satopcase_enable_spi(struct satopcase_softc *);
int	satopcase_activate(struct device *, int);
int	satopcase_get_dsm_params(struct satopcase_softc *, struct aml_node *);
int	satopcase_intr(void *);

void	satopcase_recv_msg(struct satopcase_softc *);

uint16_t satopcase_crc16(uint8_t *, size_t);

struct cfattach satopcase_ca = {
	sizeof(struct satopcase_softc),
	satopcase_match,
	satopcase_attach,
	NULL,
	satopcase_activate,
};

struct cfdriver satopcase_cd = {
	NULL, "satopcase", DV_DULL
};

int
satopcase_match(struct device *parent, void *match, void *aux)
{
	struct spi_attach_args *sa = aux;
	struct aml_node *node = sa->sa_cookie;
	uint64_t val;

	if (strcmp(sa->sa_name, "satopcase") != 0)
		return 0;

	/* don't attach if USB interface is present */
	if (aml_evalinteger(acpi_softc, node, "UIST", 0, NULL, &val) == 0 &&
	    val) {
#ifdef SATOPCASE_FORCE_ATTACH
		DPRINTF(("%s: USB enabled, forcing attachment\n",
		    sa->sa_name));
#else
		return 0;
#endif
	}

	return 1;
}

int
satopcase_print(void *aux, const char *pnp)
{
	struct satopcase_attach_args *sa = aux;

	if (pnp != NULL)
		printf("\"%s\" at %s", sa->sa_name, pnp);

	return UNCONF;
}

void
satopcase_attach(struct device *parent, struct device *self, void *aux)
{
	struct satopcase_softc *sc = (struct satopcase_softc *)self;
	struct spi_attach_args *sa = aux;
	struct satopcase_attach_args saa;

	rw_init(&sc->sc_busylock, sc->sc_dev.dv_xname);

	sc->sc_dev_node = sa->sa_cookie;

	/* if SPI is not enabled, enable it */
	if (satopcase_enable_spi(sc) != 0) {
		printf(": failed enabling SPI\n");
		return;
	}

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

	/* attach keyboard */
	saa.sa_satopcase = sc;
	saa.sa_name = "satckbd";
	sc->sc_satckbd = (struct satckbd_softc *)config_found(self, &saa,
	    satopcase_print);

	/* and touchpad */
	saa.sa_name = "satctp";
	sc->sc_satctp = (struct satctp_softc *)config_found(self, &saa,
	    satopcase_print);
}

int
satopcase_enable_spi(struct satopcase_softc *sc)
{
	uint64_t val;
	struct aml_value arg;

	/* if SPI is not enabled, enable it */
	if (aml_evalinteger(acpi_softc, sc->sc_dev_node, "SIST", 0, NULL,
	    &val) == 0 && !val) {
	    	DPRINTF(("%s: SIST is %lld\n", sc->sc_dev.dv_xname, val));

		bzero(&arg, sizeof(arg));
		arg.type = AML_OBJTYPE_INTEGER;
		arg.v_integer = 1;
		arg.length = 1;

		if (aml_evalname(acpi_softc, sc->sc_dev_node, "SIEN", 1, &arg,
		    NULL) != 0) {
			DPRINTF(("%s: couldn't enable SPI mode\n", __func__));
			return 1;
		}

		DELAY(500);
	} else {
	    	DPRINTF(("%s: SIST is already %lld\n", sc->sc_dev.dv_xname,
		    val));
	}

	return 0;
}

int
satopcase_activate(struct device *self, int act)
{
	struct satopcase_softc *sc = (struct satopcase_softc *)self;

	switch (act) {
	case DVACT_WAKEUP:
		if (satopcase_enable_spi(sc) != 0) {
			printf("%s: failed re-enabling SPI\n",
			    sc->sc_dev.dv_xname);
			return 0;
		}

		break;
	}

	return config_activate_children(self, act);
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
satopcase_intr(void *arg)
{
	struct satopcase_softc *sc = arg;

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));

	if (rw_status(&sc->sc_busylock))
		/*
		 * Avoid locking against ourselves, the GPE will re-fire if we
		 * don't read the outstanding data.
		 */
		return 1;

	/* serialize packet access */
	rw_enter_write(&sc->sc_busylock);

	memset(sc->read_raw, 0, sizeof(struct satopcase_spi_pkt));

	spi_acquire_bus(sc->sc_spi_tag, 0);
	spi_config(sc->sc_spi_tag, &sc->sc_spi_conf);
	spi_read(sc->sc_spi_tag, sc->read_raw, sizeof(sc->read_raw));
	spi_release_bus(sc->sc_spi_tag, 0);

	satopcase_recv_msg(sc);

	rw_exit_write(&sc->sc_busylock);
	wakeup(&sc);

	return 1;
}

int
satopcase_send_msg(struct satopcase_softc *sc, struct satopcase_spi_pkt *pkt,
    int msg_len, int wait_reply)
{
	int x, tries = 100, didlock = 0;
	uint16_t crc16;

	/* if we're here from another thread, we probably didn't lock */
	if (!rw_status(&sc->sc_busylock)) {
		didlock = 1;
		rw_enter_write(&sc->sc_busylock);
	}

	/* complete the message parameters */
	pkt->msg.counter = sc->pkt_counter++;
	pkt->msg.length = htole16(msg_len - 2);
	if (!pkt->msg.response_length)
		pkt->msg.response_length = pkt->msg.length;

	crc16 = htole16(satopcase_crc16(pkt->data, SATOPCASE_MSG_HEADER_LEN +
	    msg_len - 2));
	((uint8_t *)&pkt->msg)[SATOPCASE_MSG_HEADER_LEN + msg_len - 2] =
	    crc16 & 0xff;
	((uint8_t *)&pkt->msg)[SATOPCASE_MSG_HEADER_LEN + msg_len - 1] =
	    crc16 >> 8;

	/* and now the outer packet parameters */
	pkt->type = SATOPCASE_PACKET_TYPE_WRITE;
	pkt->offset = 0;
	pkt->remaining = 0;
	pkt->length = htole16(SATOPCASE_MSG_HEADER_LEN + msg_len);
	pkt->crc16 = htole16(satopcase_crc16((uint8_t *)pkt,
	    SATOPCASE_PACKET_SIZE - 2));

	DPRINTF(("%s: outgoing message:", sc->sc_dev.dv_xname));
	for (x = 0; x < SATOPCASE_PACKET_SIZE; x++)
		DPRINTF((" %02x", (((uint8_t *)pkt)[x] & 0xff)));
	DPRINTF(("\n"));

	if (wait_reply)
		sc->read_expect = pkt->msg.type;

	spi_acquire_bus(sc->sc_spi_tag, 0);
	spi_config(sc->sc_spi_tag, &sc->sc_spi_conf);
	spi_write(sc->sc_spi_tag, (uint8_t *)pkt,
	    sizeof(struct satopcase_spi_pkt));
	spi_release_bus(sc->sc_spi_tag, 0);

	if (didlock)
		rw_exit(&sc->sc_busylock);

	/*
	 * If requested, wait until we receive the packet we expected,
	 * processing (!cold) or dropping (cold) other packets along the way.
	 */
	while (sc->read_expect != 0) {
		if (cold) {
			DELAY(20);
			satopcase_intr(sc);
		} else
			tsleep(&sc, PRIBIO, "satopcase", hz / 10);

		if (!--tries) {
			DPRINTF(("%s: timed out waiting for 0x%x reply\n",
			    sc->sc_dev.dv_xname, sc->read_expect));
			sc->read_expect = 0;
			return 1;
		}
	}

	return 0;
}

void
satopcase_recv_msg(struct satopcase_softc *sc)
{
	uint16_t crc;
	uint16_t msg_crc;

#ifdef SATOPCASE_DEBUG
	satopcase_dump_read_packet(sc);
#endif

	crc = satopcase_crc16(sc->read_raw, SATOPCASE_PACKET_SIZE - 2);
	msg_crc = (sc->read_raw[SATOPCASE_PACKET_SIZE - 1] << 8) |
	    sc->read_raw[SATOPCASE_PACKET_SIZE - 2];
	if (crc != msg_crc) {
		/* Some weirdness at autoconf time is expected... */
		if (!cold) {
			printf("%s: corrupt packet (crc 0x%x != msg crc 0x%x)\n",
			    sc->sc_dev.dv_xname, crc, msg_crc);
#ifndef SATOPCASE_DEBUG
			satopcase_dump_read_packet(sc);
#endif
		}
		return;
	}

	switch (sc->read_pkt.type) {
	case SATOPCASE_PACKET_TYPE_READ:
		if (sc->read_pkt.remaining || sc->read_pkt.offset) {
			DPRINTF(("%s: remaining %d, offset %d\n",
			    sc->sc_dev.dv_xname, sc->read_pkt.remaining,
			    sc->read_pkt.offset));
		}

		switch (sc->read_pkt.device) {
		case SATOPCASE_PACKET_DEVICE_KEYBOARD:
			if (sc->sc_satckbd)
				satckbd_recv_msg(sc->sc_satckbd,
				    &sc->read_pkt.msg);
			else {
				DPRINTF(("%s: keyboard data but no keyboard\n",
				    sc->sc_dev.dv_xname));
			}
			break;
		case SATOPCASE_PACKET_DEVICE_TOUCHPAD:
			if (sc->sc_satctp)
				satctp_recv_msg(sc->sc_satctp,
				    &sc->read_pkt.msg);
			else {
				DPRINTF(("%s: touchpad data but no touchpad\n",
				    sc->sc_dev.dv_xname));
			}
			break;
		default:
			DPRINTF(("%s: unknown device for read packet: 0x%x\n",
			    sc->sc_dev.dv_xname, sc->read_pkt.device));
		}
		break;

	case SATOPCASE_PACKET_TYPE_WRITE:
		/* command response */
		if (sc->read_expect &&
		    sc->read_expect == sc->read_pkt.msg.type) {
			DPRINTF(("%s: got expected response packet 0x%x\n",
			    sc->sc_dev.dv_xname, sc->read_expect));
			sc->read_expect = 0;
		}

		switch (sc->read_pkt.device) {
		case SATOPCASE_PACKET_DEVICE_INFO:
			switch (le16toh(sc->read_pkt.msg.type)) {
			case SATOPCASE_MSG_TYPE_TP_INFO:
				if (sc->sc_satctp)
					satctp_recv_info(sc->sc_satctp,
					    &sc->read_pkt.msg);
				else {
					DPRINTF(("%s: touchpad info message "
					    "but no touchpad\n",
					    sc->sc_dev.dv_xname));
				}
				break;
			default:
				DPRINTF(("%s: unknown type for info packet: "
				    "0x%x\n", sc->sc_dev.dv_xname,
				    le16toh(sc->read_pkt.msg.type)));
			}
			break;
		case SATOPCASE_PACKET_DEVICE_TOUCHPAD:
			if (sc->sc_satctp)
				satctp_recv_msg(sc->sc_satctp,
				    &sc->read_pkt.msg);
			else {
				DPRINTF(("%s: touchpad write message "
				    "but no touchpad\n",
				    sc->sc_dev.dv_xname));
			}
			break;
		default:
			DPRINTF(("%s: unknown device for write packet "
			    "response: 0x%x\n", sc->sc_dev.dv_xname,
			    sc->read_pkt.device));
		}
		break;
	case SATOPCASE_PACKET_TYPE_ERROR:
		/*
		 * Response to bogus command, or doing a read when there is
		 * nothing to read (such as when forcing a read while cold and
		 * the corresponding GPE doesn't get serviced until !cold).
		 */
		DPRINTF(("%s: received error packet\n", sc->sc_dev.dv_xname));
		break;
	default:
		DPRINTF(("%s: unknown packet type 0x%x\n", sc->sc_dev.dv_xname,
		    sc->read_pkt.type));
	}
}

void
satopcase_dump_read_packet(struct satopcase_softc *sc)
{
	int x;

	printf("%s: received message:", sc->sc_dev.dv_xname);
	for (x = 0; x < SATOPCASE_PACKET_SIZE; x++)
		printf(" %02x", (sc->read_raw[x] & 0xff));
	printf("\n");
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
