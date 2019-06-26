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

/* #define SATOPCASE_DEBUG */
#define SATOPCASE_DEBUG

#ifdef SATOPCASE_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

#define SATOPCASE_PACKET_SIZE	256

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

struct satopcase_kbd_message {
	uint8_t		_unused;
	uint8_t		modifiers;
#define KBD_MSG_MODS	8
	uint8_t		_unused2;
#define KBD_MSG_KEYS	5
	uint8_t		pressed[KBD_MSG_KEYS];
	uint8_t		overflow;
	uint8_t		fn;
	uint16_t	crc16;
};

struct satopcase_spi_packet {
	uint8_t				type;
#define PACKET_TYPE_READ		0x20
#define PACKET_TYPE_WRITE		0x40
	uint8_t				device;
#define PACKET_DEVICE_KEYBOARD		0x01
#define PACKET_DEVICE_TOUCHPAD		0x02
#define PACKET_DEVICE_INFO		0xd0
	uint16_t			offset;
	uint16_t			remaining;
	uint16_t			length;
	union {
		struct satopcase_spi_message {
			uint16_t	 type;
			uint8_t		 zero;
			uint8_t		 counter;
			uint16_t	 response_len;
			uint16_t	 length;
			union {
				struct satopcase_kbd_message keyboard;
				uint8_t	data[238];
			};
		}			message;
		uint8_t			data[246];
	};
	uint16_t			crc16;
} __packed;

struct satopcase_softc {
	struct device	sc_dev;
	spi_tag_t	sc_spi_tag;
	struct ispi_gpe_intr sc_gpe_intr;
	void		*sc_ih;

	struct spi_config sc_spi_conf;

	union {
		struct satopcase_spi_packet sc_read_packet;
		uint8_t	sc_read_raw[SATOPCASE_PACKET_SIZE];
	};

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
#ifdef WSDISPLAY_COMPAT_RAWKBD
	int		sc_rawkbd;
#endif
	int		kbd_keys_down[KBD_MSG_KEYS + KBD_MSG_MODS];

	const keysym_t	sc_kcodes;
	const keysym_t	sc_xt_kcodes;
	int		sc_ksize;
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

	return (0);
}

int
satopcase_intr(void *arg)
{
	struct satopcase_softc *sc = arg;

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));

	spi_acquire_bus(sc->sc_spi_tag, 0);
	spi_config(sc->sc_spi_tag, &sc->sc_spi_conf);
	spi_read(sc->sc_spi_tag, sc->sc_read_raw, sizeof(sc->sc_read_raw));
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

	DPRINTF(("%s: kbd msg, states:", sc->sc_dev.dv_xname));
	for (x = 0; x < nitems(pressed); x++)
		DPRINTF((" %02d", pressed[x]));
	DPRINTF(("\n"));

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
