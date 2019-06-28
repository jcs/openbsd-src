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
 * keyboard structs
 */

enum satopcase_kbd_mods {
	KBD_MOD_CONTROL_L = 84,
	KBD_MOD_SHIFT_L,
	KBD_MOD_ALT_L,
	KBD_MOD_META_L,
	KBD_MOD_UNKNOWN,
	KBD_MOD_SHIFT_R,
	KBD_MOD_ALT_R,
	KBD_MOD_META_R,
	KBD_FN = 100,
};

enum satopcase_kbd_fn_keys {
	KBD_FN_RIGHT_END = 93,
	KBD_FN_LEFT_HOME,
	KBD_FN_DOWN_PAGEDOWN,
	KBD_FN_UP_PAGEUP,
	KBD_FN_BACKSPACE_DELETE,
	KBD_FN_RETURN_INSERT,
	KBD_FN_F1_BRIGHTNESS_DOWN,
	KBD_FN_F2_BRIGHTNESS_UP,
	KBD_FN_F5_KBD_LIGHT_DOWN,
	KBD_FN_F6_KBD_LIGHT_UP,
	KBD_FN_F7_MEDIA_PREV,
	KBD_FN_F8_MEDIA_PLAYPAUSE,
	KBD_FN_F9_MEDIA_NEXT,
	KBD_FN_F10_MUTE,
	KBD_FN_F11_VOLUME_DOWN,
	KBD_FN_F12_VOLUME_UP,
};

/* when Fn is held, translate this key to that key */
static struct satopcase_kbd_fn_trans_entry {
	keysym_t	from;
	keysym_t	to;
} satopcase_kb_fn_trans[] = {
	{ KS_Right,	KBD_FN_RIGHT_END },
	{ KS_Left,	KBD_FN_LEFT_HOME },
	{ KS_Down,	KBD_FN_DOWN_PAGEDOWN },
	{ KS_Up,	KBD_FN_UP_PAGEUP },
	{ KS_BackSpace,	KBD_FN_BACKSPACE_DELETE },
	{ KS_Return,	KBD_FN_RETURN_INSERT },
	{ KS_F1,	KBD_FN_F1_BRIGHTNESS_DOWN },
	{ KS_F2,	KBD_FN_F2_BRIGHTNESS_UP },
	{ KS_F5,	KBD_FN_F5_KBD_LIGHT_DOWN },
	{ KS_F6,	KBD_FN_F6_KBD_LIGHT_UP },
	{ KS_F7,	KBD_FN_F7_MEDIA_PREV },
	{ KS_F8,	KBD_FN_F8_MEDIA_PLAYPAUSE },
	{ KS_F9,	KBD_FN_F9_MEDIA_NEXT },
	{ KS_F10,	KBD_FN_F10_MUTE },
	{ KS_F11,	KBD_FN_F11_VOLUME_DOWN },
	{ KS_F12,	KBD_FN_F12_VOLUME_UP },
};

#define KC(n) KS_KEYCODE(n)
const keysym_t satopcase_keycodes_us[] = {
/*	idx		command		normal		shifted */
	KC(0),
	KC(1),
	KC(2),
	KC(3),
	KC(4),				KS_a,
	KC(5),				KS_b,
	KC(6),				KS_c,
	KC(7),				KS_d,
	KC(8),				KS_e,
	KC(9),				KS_f,
	KC(10),				KS_g,
	KC(11),				KS_h,
	KC(12),				KS_i,
	KC(13),				KS_j,
	KC(14),				KS_k,
	KC(15),				KS_l,
	KC(16),				KS_m,
	KC(17),				KS_n,
	KC(18),				KS_o,
	KC(19),				KS_p,
	KC(20),				KS_q,
	KC(21),				KS_r,
	KC(22),				KS_s,
	KC(23),				KS_t,
	KC(24),				KS_u,
	KC(25),				KS_v,
	KC(26),				KS_w,
	KC(27),				KS_x,
	KC(28),				KS_y,
	KC(29),				KS_z,
	KC(30),				KS_1,		KS_exclam,
	KC(31),				KS_2,		KS_at,
	KC(32),				KS_3,		KS_numbersign,
	KC(33),				KS_4,		KS_dollar,
	KC(34),				KS_5,		KS_percent,
	KC(35),				KS_6,		KS_asciicircum,
	KC(36),				KS_7,		KS_ampersand,
	KC(37),				KS_8,		KS_asterisk,
	KC(38),				KS_9,		KS_parenleft,
	KC(39),				KS_0,		KS_parenright,
	KC(40),				KS_Return,
	KC(41),				KS_Escape,
	KC(42),				KS_BackSpace,
	KC(43),				KS_Tab,
	KC(44),				KS_space,
	KC(45),				KS_minus,	KS_underscore,
	KC(46),				KS_equal,	KS_plus,
	KC(47),				KS_bracketleft,	KS_braceleft,
	KC(48),				KS_bracketright,KS_braceright,
	KC(49),				KS_backslash,	KS_bar,
	KC(50),
	KC(51),				KS_semicolon,	KS_colon,
	KC(52),				KS_apostrophe,	KS_quotedbl,
	KC(53),				KS_grave,	KS_asciitilde,
	KC(54),				KS_comma,	KS_less,
	KC(55),				KS_period,	KS_greater,
	KC(56),				KS_slash,	KS_question,
	KC(57),				KS_Caps_Lock,
	KC(58),				KS_F1,
	KC(59),				KS_F2,
	KC(60),				KS_F3,
	KC(61),				KS_F4,
	KC(62),				KS_F5,
	KC(63),				KS_F6,
	KC(64),				KS_F7,
	KC(65),				KS_F8,
	KC(66),				KS_F9,
	KC(67),				KS_F10,
	KC(68),				KS_F11,
	KC(69),				KS_F12,
	KC(70),
	KC(71),
	KC(72),
	KC(73),
	KC(74),
	KC(75),
	KC(76),
	KC(77),
	KC(78),
	KC(79),				KS_Right,
	KC(80),				KS_Left,
	KC(81),				KS_Down,
	KC(82),				KS_Up,
	KC(83),
	/* key codes aren't generated for modifier keys, so fake it */
	KC(KBD_MOD_CONTROL_L),		KS_Control_L,
	KC(KBD_MOD_SHIFT_L),		KS_Shift_L,
	KC(KBD_MOD_ALT_L),		KS_Alt_L,
	KC(KBD_MOD_META_L),		KS_Meta_L,
	KC(KBD_MOD_UNKNOWN),
	KC(KBD_MOD_SHIFT_R),		KS_Shift_R,
	KC(KBD_MOD_ALT_R),		KS_Alt_R,
	KC(KBD_MOD_META_R),		KS_Meta_R,
	KC(92),
	/* same for keys pressed with fn */
	KC(KBD_FN_RIGHT_END),		KS_End,
	KC(KBD_FN_LEFT_HOME),		KS_Home,
	KC(KBD_FN_DOWN_PAGEDOWN),	KS_Next,
	KC(KBD_FN_UP_PAGEUP),		KS_Prior,
	KC(KBD_FN_BACKSPACE_DELETE),	KS_Delete,
	KC(KBD_FN_RETURN_INSERT),	KS_Insert,
	KC(KBD_FN_F1_BRIGHTNESS_DOWN),	KS_Cmd_BrightnessDown,
	KC(KBD_FN_F2_BRIGHTNESS_UP),	KS_Cmd_BrightnessUp,
	KC(KBD_FN_F5_KBD_LIGHT_DOWN),
	KC(KBD_FN_F6_KBD_LIGHT_UP),
	KC(KBD_FN_F7_MEDIA_PREV),
	KC(KBD_FN_F8_MEDIA_PLAYPAUSE),
	KC(KBD_FN_F9_MEDIA_NEXT),
	KC(KBD_FN_F10_MUTE),		KS_AudioMute,
	KC(KBD_FN_F11_VOLUME_DOWN),	KS_AudioLower,
	KC(KBD_FN_F12_VOLUME_UP),	KS_AudioRaise,
};
#undef KC

#ifdef WSDISPLAY_COMPAT_RAWKBD
const unsigned char satopcase_raw_keycodes_us[] = {
	0,
	0,
	0,
	0,
	RAWKEY_a,
	RAWKEY_b,
	RAWKEY_c,
	RAWKEY_d,
	RAWKEY_e,
	RAWKEY_f,
	RAWKEY_g,
	RAWKEY_h,
	RAWKEY_i,
	RAWKEY_j,
	RAWKEY_k,
	RAWKEY_l,
	RAWKEY_m,
	RAWKEY_n,
	RAWKEY_o,
	RAWKEY_p,
	RAWKEY_q,
	RAWKEY_r,
	RAWKEY_s,
	RAWKEY_t,
	RAWKEY_u,
	RAWKEY_v,
	RAWKEY_w,
	RAWKEY_x,
	RAWKEY_y,
	RAWKEY_z,
	RAWKEY_1,
	RAWKEY_2,
	RAWKEY_3,
	RAWKEY_4,
	RAWKEY_5,
	RAWKEY_6,
	RAWKEY_7,
	RAWKEY_8,
	RAWKEY_9,
	RAWKEY_0,
	RAWKEY_Return,
	RAWKEY_Escape,
	RAWKEY_BackSpace,
	RAWKEY_Tab,
	RAWKEY_space,
	RAWKEY_minus,
	RAWKEY_equal,
	RAWKEY_bracketleft,
	RAWKEY_bracketright,
	RAWKEY_backslash,
	0,
	RAWKEY_semicolon,
	RAWKEY_apostrophe,
	RAWKEY_grave,
	RAWKEY_comma,
	RAWKEY_period,
	RAWKEY_slash,
	RAWKEY_Caps_Lock,
	RAWKEY_f1,
	RAWKEY_f2,
	RAWKEY_f3,
	RAWKEY_f4,
	RAWKEY_f5,
	RAWKEY_f6,
	RAWKEY_f7,
	RAWKEY_f8,
	RAWKEY_f9,
	RAWKEY_f10,
	RAWKEY_f11,
	RAWKEY_f12,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	RAWKEY_Right,
	RAWKEY_Left,
	RAWKEY_Down,
	RAWKEY_Up,
	0,
	RAWKEY_Control_L,
	RAWKEY_Shift_L,
	RAWKEY_Alt_L,
	0xdb, /* RAWKEY_Meta_L, */
	0,
	RAWKEY_Shift_R,
	RAWKEY_Alt_R,
	0xdc, /* RAWKEY_Meta_R, */
	0,
	RAWKEY_End,
	RAWKEY_Home,
	RAWKEY_Next,
	RAWKEY_Prior,
	RAWKEY_Delete,
	RAWKEY_Insert,
	0, /* KS_Cmd_BrightnessDown, */
	0, /* KS_Cmd_BrightnessUp, */
	0,
	0,
	0,
	0,
	0,
	RAWKEY_AudioMute,
	RAWKEY_AudioLower,
	RAWKEY_AudioRaise,
};
#endif /* WSDISPLAY_COMPAT_RAWKBD */


/*
 * touchpad structs
 */

/* most of this came from dev/usb/ubcmtp.c */

#define SATOPCASE_TP_FINGER_ORIENT	16384
#define SATOPCASE_TP_SN_PRESSURE	45
#define SATOPCASE_TP_SN_WIDTH		25
#define SATOPCASE_TP_SN_COORD		250
#define SATOPCASE_TP_SN_ORIENT		10

struct satopcase_tp_finger {
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
#define SATOPCASE_TP_DEFAULT_PRESSURE	40
	uint16_t	multi;
	uint16_t	crc16;
} __packed __attribute((aligned(2)));

struct satopcase_tp_limit {
	int limit;
	int min;
	int max;
};

static struct satopcase_tp_dev_type {
	uint16_t model;
	struct satopcase_tp_limit l_pressure;	/* finger pressure */
	struct satopcase_tp_limit l_width;	/* finger width */
	struct satopcase_tp_limit l_x;
	struct satopcase_tp_limit l_y;
	struct satopcase_tp_limit l_orientation;
} satopcase_tp_devices[] = {
	{
		/* MacBook10,1 */
		0x0417,
		{ SATOPCASE_TP_SN_PRESSURE, 0, 300 },
		{ SATOPCASE_TP_SN_WIDTH, 0, 2048 },
		{ SATOPCASE_TP_SN_COORD, -5087, 5579 },
		{ SATOPCASE_TP_SN_COORD, -182, 6089 },
		{ SATOPCASE_TP_SN_ORIENT, -SATOPCASE_TP_FINGER_ORIENT,
		    SATOPCASE_TP_FINGER_ORIENT },
	},
};


/*
 * packet structs
 */

#define SATOPCASE_PACKET_SIZE	256

struct satopcase_spi_pkt {
	uint8_t				type;
#define PACKET_TYPE_READ		0x20
#define PACKET_TYPE_WRITE		0x40
#define PACKET_TYPE_ERROR		0x80
	uint8_t				device;
#define PACKET_DEVICE_KEYBOARD		0x01
#define PACKET_DEVICE_TOUCHPAD		0x02
#define PACKET_DEVICE_INFO		0xd0
	uint16_t			offset;
	uint16_t			remaining;
	uint16_t			length;
	union {
		struct satopcase_spi_msg {
			uint16_t	type;
		#define MSG_TYPE_KBD_DATA 0x0110
		#define MSG_TYPE_TP_DATA 0x0210
		#define MSG_TYPE_TP_INFO 0x1020
		#define MSG_TYPE_TP_MT	0x0252
			uint8_t		type2;
		#define MSG_TYPE2_TP_INFO 0x02
			uint8_t		counter;
			uint16_t	response_length;
			uint16_t	length;
		#define MSG_HEADER_LEN	8
			union {
				struct satopcase_kbd_data {
					uint8_t		_unused;
					uint8_t		modifiers;
				#define KBD_DATA_MODS	8
					uint8_t		_unused2;
				#define KBD_DATA_KEYS	5
					uint8_t		pressed[KBD_DATA_KEYS];
					uint8_t		overflow;
					uint8_t		fn;
					uint16_t	crc16;
				} __packed kbd_data;
				struct satopcase_tp_data {
					uint8_t		_unused[1];
					uint8_t		button;
					uint8_t		_unused2[28];
					uint8_t		fingers;
				#define TP_MAX_FINGERS	16
					uint8_t		clicked2;
					uint8_t		_unused3[16];
					struct satopcase_tp_finger finger_data[0];
				} __packed tp_data;
				struct satopcase_tp_info_cmd {
					uint16_t	crc16;
				} __packed tp_info_cmd;
				struct satopcase_tp_info {
					uint8_t		_unused[105];
					uint16_t	model;
					uint8_t		_unused2[3];
					uint16_t	crc16;
				} __packed tp_info;
				struct satopcase_tp_mt_cmd {
					uint16_t	mode;
				#define TP_MT_CMD_MT_MODE 0x0102
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
	union {
		struct satopcase_spi_pkt sc_write_pkt;
		uint8_t		sc_write_raw[SATOPCASE_PACKET_SIZE];
	};

	/* from _DSM */
	uint64_t		spi_sclk_period;
	uint64_t		spi_word_size;
	uint64_t		spi_bit_order;
	uint64_t		spi_spo;
	uint64_t		spi_sph;
	uint64_t		spi_cs_delay;
	uint64_t		reset_a2r_usec;
	uint64_t		reset_rec_usec;

	struct device		*sc_wskbddev;
#ifdef WSDISPLAY_COMPAT_RAWKBD
	int			sc_rawkbd;
#endif
	int			kbd_keys_down[KBD_DATA_KEYS + KBD_DATA_MODS];

	struct device		*sc_wsmousedev;
	struct satopcase_tp_dev_type *tp_dev_type;
	struct mtpoint		frame[TP_MAX_FINGERS];

	const keysym_t		sc_kcodes;
	const keysym_t		sc_xt_kcodes;
	int			sc_ksize;
};


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
