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

#include <dev/acpi/acpireg.h>
#include <dev/acpi/acpivar.h>
#include <dev/acpi/acpidev.h>
#include <dev/acpi/amltypes.h>
#include <dev/acpi/dsdt.h>

#include <dev/ic/ispivar.h>
#include <dev/spi/spivar.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsmousevar.h>

#define SATOPCASE_PACKET_SIZE	256

struct satopcase_spi_pkt {
	uint8_t				type;
#define SATOPCASE_PACKET_TYPE_READ	0x20
#define SATOPCASE_PACKET_TYPE_WRITE	0x40
#define SATOPCASE_PACKET_TYPE_ERROR	0x80
	uint8_t				device;
#define SATOPCASE_PACKET_DEVICE_KEYBOARD 0x01
#define SATOPCASE_PACKET_DEVICE_TOUCHPAD 0x02
#define SATOPCASE_PACKET_DEVICE_INFO	0xd0
	uint16_t			offset;
	uint16_t			remaining;
	uint16_t			length;
	union {
		struct satopcase_spi_msg {
			uint16_t	type;
		#define SATOPCASE_MSG_TYPE_KBD_DATA	0x0110
		#define SATOPCASE_MSG_TYPE_TP_DATA	0x0210
		#define SATOPCASE_MSG_TYPE_TP_INFO	0x1020
		#define SATOPCASE_MSG_TYPE_TP_MT	0x0252
			uint8_t		type2;
		#define SATOPCASE_MSG_TYPE2_TP_INFO	0x02
			uint8_t		counter;
			uint16_t	response_length;
			uint16_t	length;
		#define SATOPCASE_MSG_HEADER_LEN	8
			union {
				struct satckbd_data {
					uint8_t		_unused;
					uint8_t		modifiers;
				#define SATCKBD_DATA_MODS 8
					uint8_t		_unused2;
				#define SATCKBD_DATA_KEYS 5
					uint8_t		pressed[SATCKBD_DATA_KEYS];
					uint8_t		overflow;
					uint8_t		fn;
					uint16_t	crc16;
				} __packed kbd_data;
				struct satctp_data {
					uint8_t		_unused[1];
					uint8_t		button;
					uint8_t		_unused2[28];
					uint8_t		fingers;
				#define SATCTP_MAX_FINGERS 16
					uint8_t		clicked2;
					uint8_t		_unused3[16];
					uint8_t		finger_data[];
				} __packed tp_data;
				struct satctp_info_cmd {
					uint16_t	crc16;
				} __packed tp_info_cmd;
				struct satctp_info {
					uint8_t		_unused[105];
					uint16_t	model;
					uint8_t		_unused2[3];
					uint16_t	crc16;
				} __packed tp_info;
				struct satctp_mt_cmd {
					uint16_t	mode;
				#define SATCTP_MT_CMD_MT_MODE 0x0102
					uint16_t	crc16;
				} __packed tp_mt_cmd;
				uint8_t	data[238];
			};
		} __packed		msg;
		uint8_t			data[246];
	};
	uint16_t			crc16;
} __packed;


/*
 * autoconf
 */

struct satopcase_attach_args {
	struct satopcase_softc	*sa_satopcase;
	char			*sa_name;
};

struct satopcase_softc {
	struct device		sc_dev;
	spi_tag_t		sc_spi_tag;
	struct ispi_gpe_intr 	sc_gpe_intr;
	void			*sc_ih;
	struct spi_config 	sc_spi_conf;

	struct rwlock		sc_busylock;

	uint8_t			sc_pkt_counter;
	union {
		struct satopcase_spi_pkt sc_read_pkt;
		uint8_t		sc_read_raw[SATOPCASE_PACKET_SIZE];
	};
	int			sc_last_read_error;

	/* from _DSM */
	uint64_t		spi_sclk_period;
	uint64_t		spi_word_size;
	uint64_t		spi_bit_order;
	uint64_t		spi_spo;
	uint64_t		spi_sph;
	uint64_t		spi_cs_delay;
	uint64_t		reset_a2r_usec;
	uint64_t		reset_rec_usec;

	struct satckbd_softc	*sc_satckbd;
	struct satctp_softc	*sc_satctp;
};

int	satopcase_send_msg(struct satopcase_softc *, struct satopcase_spi_pkt *,
	    int, int);

/*
 * satckbd - keyboard driver
 */

struct satckbd_softc {
	struct device		sc_dev;
	struct satopcase_softc	*sc_satopcase;

	struct device		*sc_wskbddev;

#ifdef WSDISPLAY_COMPAT_RAWKBD
	int			sc_rawkbd;
#endif
	int			kbd_keys_down[SATCKBD_DATA_KEYS +
				    SATCKBD_DATA_MODS];
};

void	satckbd_recv_msg(struct satckbd_softc *, struct satopcase_spi_msg *);


/*
 * satctp - touchpad driver
 */

struct satctp_limit {
	int limit;
	int min;
	int max;
};
struct satctp_dev_type {
	uint16_t model;
	struct satctp_limit l_pressure;	/* finger pressure */
	struct satctp_limit l_width;	/* finger width */
	struct satctp_limit l_x;
	struct satctp_limit l_y;
	struct satctp_limit l_orientation;
};
struct satctp_softc {
	struct device		sc_dev;
	struct satopcase_softc	*sc_satopcase;

	struct device		*sc_wsmousedev;

	struct satctp_dev_type	*dev_type;
	struct mtpoint		frame[SATCTP_MAX_FINGERS];
};

void	satctp_recv_msg(struct satctp_softc *, struct satopcase_spi_msg *);
void	satctp_recv_info(struct satctp_softc *sc, struct satopcase_spi_msg *);


/*
 * lookup table for satopcase_crc16
 * CRC16-ARC, poly 0x8005
 */
static const uint16_t crc16_table[256] = {
	0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
	0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
	0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
	0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
	0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
	0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
	0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
	0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
	0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
	0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
	0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
	0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
	0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
	0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
	0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
	0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
	0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
	0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
	0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
	0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
	0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
	0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
	0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
	0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
	0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
	0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
	0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
	0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
	0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
	0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
	0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
	0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040,
};

