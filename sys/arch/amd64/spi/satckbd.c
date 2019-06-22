/* $OpenBSD$ */
/*
 * Apple SPI keyboard driver for Apple "topcase" devices
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

#include <dev/wscons/wsconsio.h>
#ifdef WSDISPLAY_COMPAT_RAWKBD
#include <dev/wscons/wskbdraw.h>
#endif
#include <dev/wscons/wskbdvar.h>
#include <dev/wscons/wsksymdef.h>
#include <dev/wscons/wsksymvar.h>

#include "satopcasevar.h"

/* #define SATCKBD_DEBUG */

#ifdef SATCKBD_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

enum satckbd_mods {
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

enum satckbd_fn_keys {
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
static struct satckbd_fn_trans_entry {
	keysym_t	from;
	keysym_t	to;
} satckbd_fn_trans[] = {
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
const keysym_t satckbd_keycodes_us[] = {
/*	idx			command		normal		shifted */
	KC(0),
	KC(1),
	KC(2),
	KC(3),
	KC(4),					KS_a,
	KC(5),					KS_b,
	KC(6),					KS_c,
	KC(7),					KS_d,
	KC(8),					KS_e,
	KC(9),					KS_f,
	KC(10),					KS_g,
	KC(11),					KS_h,
	KC(12),					KS_i,
	KC(13),					KS_j,
	KC(14),					KS_k,
	KC(15),					KS_l,
	KC(16),					KS_m,
	KC(17),					KS_n,
	KC(18),					KS_o,
	KC(19),					KS_p,
	KC(20),					KS_q,
	KC(21),					KS_r,
	KC(22),					KS_s,
	KC(23),					KS_t,
	KC(24),					KS_u,
	KC(25),					KS_v,
	KC(26),					KS_w,
	KC(27),					KS_x,
	KC(28),					KS_y,
	KC(29),					KS_z,
	KC(30),					KS_1,		KS_exclam,
	KC(31),					KS_2,		KS_at,
	KC(32),					KS_3,		KS_numbersign,
	KC(33),					KS_4,		KS_dollar,
	KC(34),					KS_5,		KS_percent,
	KC(35),					KS_6,		KS_asciicircum,
	KC(36),					KS_7,		KS_ampersand,
	KC(37),					KS_8,		KS_asterisk,
	KC(38),					KS_9,		KS_parenleft,
	KC(39),					KS_0,		KS_parenright,
	KC(40),					KS_Return,
	KC(41),					KS_Escape,
	KC(42),					KS_Delete,
	KC(43),					KS_Tab,
	KC(44),					KS_space,
	KC(45),					KS_minus,	KS_underscore,
	KC(46),					KS_equal,	KS_plus,
	KC(47),					KS_bracketleft,	KS_braceleft,
	KC(48),					KS_bracketright,KS_braceright,
	KC(49),					KS_backslash,	KS_bar,
	KC(50),
	KC(51),					KS_semicolon,	KS_colon,
	KC(52),					KS_apostrophe,	KS_quotedbl,
	KC(53),					KS_grave,	KS_asciitilde,
	KC(54),					KS_comma,	KS_less,
	KC(55),					KS_period,	KS_greater,
	KC(56),					KS_slash,	KS_question,
	KC(57),					KS_Caps_Lock,
	KC(58),			KS_Cmd_Screen0,	KS_F1,
	KC(59),			KS_Cmd_Screen1,	KS_F2,
	KC(60),			KS_Cmd_Screen2,	KS_F3,
	KC(61),			KS_Cmd_Screen3,	KS_F4,
	KC(62),			KS_Cmd_Screen4,	KS_F5,
	KC(63),			KS_Cmd_Screen5,	KS_F6,
	KC(64),			KS_Cmd_Screen6,	KS_F7,
	KC(65),			KS_Cmd_Screen7,	KS_F8,
	KC(66),			KS_Cmd_Screen8,	KS_F9,
	KC(67),			KS_Cmd_Screen9,	KS_F10,
	KC(68),			KS_Cmd_Screen10,KS_F11,
	KC(69),			KS_Cmd_Screen11,KS_F12,
	KC(70),
	KC(71),
	KC(72),
	KC(73),
	KC(74),
	KC(75),
	KC(76),
	KC(77),
	KC(78),
	KC(79),					KS_Right,
	KC(80),					KS_Left,
	KC(81),					KS_Down,
	KC(82),					KS_Up,
	KC(83),
	/* key codes aren't generated for modifier keys, so fake it */
	KC(KBD_MOD_CONTROL_L),	KS_Cmd1,	KS_Control_L,
	KC(KBD_MOD_SHIFT_L),			KS_Shift_L,
	KC(KBD_MOD_ALT_L),	KS_Cmd2,	KS_Alt_L,
	KC(KBD_MOD_META_L),			KS_Meta_L,
	KC(KBD_MOD_UNKNOWN),
	KC(KBD_MOD_SHIFT_R),			KS_Shift_R,
	KC(KBD_MOD_ALT_R),	KS_Cmd2,	KS_Alt_R,
	KC(KBD_MOD_META_R),			KS_Meta_R,
	KC(92),
	/* same for keys pressed with fn */
	KC(KBD_FN_RIGHT_END),			KS_End,
	KC(KBD_FN_LEFT_HOME),			KS_Home,
	KC(KBD_FN_DOWN_PAGEDOWN), KS_Cmd_ScrollFwd, KS_Next,
	KC(KBD_FN_UP_PAGEUP),	KS_Cmd_ScrollBack, KS_Prior,
	KC(KBD_FN_BACKSPACE_DELETE),		KS_KP_Delete,
	KC(KBD_FN_RETURN_INSERT),		KS_Insert,
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
const unsigned char satckbd_raw_keycodes_us[] = {
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

int	satckbd_match(struct device *, void *, void *);
void	satckbd_attach(struct device *, struct device *, void *);
int	satckbd_activate(struct device *, int);

int	satckbd_enable(void *, int);
void	satckbd_wskbd_setleds(void *, int);
int	satckbd_ioctl(void *, u_long, caddr_t, int, struct proc *);
void	satckbd_cnbell(void *, u_int, u_int, u_int);
void	satckbd_cngetc(void *, u_int *, int *);
void	satckbd_cnpollc(void *, int);

void	satckbd_set_caps_light(void *);

/* for keyboard backlight control */
void	satckbd_set_backlight(void *);
extern int (*wskbd_get_backlight)(struct wskbd_backlight *);
extern int (*wskbd_set_backlight)(struct wskbd_backlight *);
int	satckbd_wskbd_get_backlight(struct wskbd_backlight *);
int	satckbd_wskbd_set_backlight(struct wskbd_backlight *);

struct cfattach satckbd_ca = {
	sizeof(struct satckbd_softc),
	satckbd_match,
	satckbd_attach,
	NULL,
	satckbd_activate,
};

struct cfdriver satckbd_cd = {
	NULL, "satckbd", DV_DULL
};

struct wskbd_accessops satckbd_accessops = {
	satckbd_enable,
	satckbd_wskbd_setleds,
	satckbd_ioctl,
};

struct wskbd_consops satckbd_consops = {
	satckbd_cngetc,
	satckbd_cnpollc,
	satckbd_cnbell,
};

struct wscons_keydesc satckbd_keydesctab[] = {
	{ KB_US, 0, sizeof(satckbd_keycodes_us) / sizeof(keysym_t),
	    satckbd_keycodes_us },
	{ 0, 0, 0, 0 },
};

struct wskbd_mapdata satckbd_mapdata = {
	satckbd_keydesctab, KB_US,
};

int
satckbd_match(struct device *parent, void *match, void *aux)
{
	struct satopcase_attach_args *sa = aux;

	if (strcmp(sa->sa_name, "satckbd") == 0)
		return 1;

	return 0;
}

void
satckbd_attach(struct device *parent, struct device *self, void *aux)
{
	struct satckbd_softc *sc = (struct satckbd_softc *)self;
	struct satopcase_attach_args *sa = aux;
	struct wskbddev_attach_args wkaa;

	sc->sc_satopcase = sa->sa_satopcase;

	printf("\n");

	memset(&wkaa, 0, sizeof(wkaa));
	wkaa.console = 0;
	wkaa.keymap = &satckbd_mapdata;
	wkaa.accessops = &satckbd_accessops;
	wkaa.accesscookie = sc;
	sc->sc_wskbddev = config_found(self, &wkaa, wskbddevprint);

	task_set(&sc->sc_task_caps_light, satckbd_set_caps_light, sc);

	sc->backlight = SATCKBD_BACKLIGHT_LEVEL_MIN;
	task_set(&sc->sc_task_backlight, satckbd_set_backlight, sc);
	wskbd_get_backlight = satckbd_wskbd_get_backlight;
	wskbd_set_backlight = satckbd_wskbd_set_backlight;
}

int
satckbd_activate(struct device *self, int act)
{
	struct satckbd_softc *sc = (struct satckbd_softc *)self;

	switch (act) {
	case DVACT_WAKEUP:
		/* caps lock LED is turned off at suspend */
		if (sc->leds & WSKBD_LED_CAPS)
			task_add(systq, &sc->sc_task_caps_light);
	}

	return config_activate_children(self, act);
}

int
satckbd_enable(void *v, int power)
{
	return 0;
}

void
satckbd_wskbd_setleds(void *v, int leds)
{
	struct satckbd_softc *sc = v;

	DPRINTF(("%s: %s(0x%x)\n", sc->sc_dev.dv_xname, __func__, leds));

	if (sc->leds == leds)
		return;

	sc->leds = leds;
	task_add(systq, &sc->sc_task_caps_light);
}

void
satckbd_set_caps_light(void *v)
{
	struct satckbd_softc *sc = v;
	struct satopcase_spi_pkt pkt;

	DPRINTF(("%s: %s: sending caps cmd %s\n", sc->sc_dev.dv_xname, __func__,
	    ((sc->leds & WSKBD_LED_CAPS) ? "on" : "off")));

	memset(&pkt, 0, sizeof(pkt));
	pkt.device = SATOPCASE_PACKET_DEVICE_KEYBOARD;
	pkt.msg.type = htole16(SATOPCASE_MSG_TYPE_KBD_CAPS_LIGHT);
	pkt.msg.kbd_capslock_light_cmd.on_off =
	    htole16((sc->leds & WSKBD_LED_CAPS) ? SATCKBD_CAPSLOCK_LIGHT_ON :
	    SATCKBD_CAPSLOCK_LIGHT_OFF);

	satopcase_send_msg(sc->sc_satopcase, &pkt,
	    sizeof(struct satckbd_capslock_light_cmd), 0);
}

int
satckbd_ioctl(void *v, u_long cmd, caddr_t data, int flag, struct proc *p)
{
#ifdef WSDISPLAY_COMPAT_RAWKBD
	struct satckbd_softc *sc = v;
#endif

	switch (cmd) {
	case WSKBDIO_GTYPE:
		*(int *)data = WSKBD_TYPE_USB; /* XXX */
		return 0;
	case WSKBDIO_GETLEDS:
		*(int *)data = sc->leds;
		return 0;
	case WSKBDIO_SETLEDS:
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
satckbd_cnbell(void *v, u_int pitch, u_int period, u_int volume)
{
	DPRINTF(("%s\n", __func__));
}

void
satckbd_cngetc(void *v, u_int *type, int *data)
{
	DPRINTF(("%s\n", __func__));
}

void
satckbd_cnpollc(void *v, int on)
{
	DPRINTF(("%s\n", __func__));
}

void
satckbd_set_backlight(void *v)
{
	struct satckbd_softc *sc = v;
	struct satopcase_spi_pkt pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.device = SATOPCASE_PACKET_DEVICE_KEYBOARD;
	pkt.msg.type = SATOPCASE_MSG_TYPE_KBD_BACKLIGHT;
	pkt.msg.kbd_backlight_cmd.const1 = htole16(SATCKBD_BACKLIGHT_CONST1);
	pkt.msg.kbd_backlight_cmd.level =
	    (sc->backlight <= SATCKBD_BACKLIGHT_LEVEL_MIN ? 0 :
	    htole16(sc->backlight));
	pkt.msg.kbd_backlight_cmd.on_off =
	    htole16(sc->backlight <= SATCKBD_BACKLIGHT_LEVEL_MIN ?
	    SATCKBD_BACKLIGHT_OFF : SATCKBD_BACKLIGHT_ON);

	satopcase_send_msg(sc->sc_satopcase, &pkt,
	    sizeof(struct satckbd_backlight_cmd), 0);
}

int
satckbd_wskbd_get_backlight(struct wskbd_backlight *kbl)
{
	struct satckbd_softc *sc = satckbd_cd.cd_devs[0];

	DPRINTF(("%s: %s\n", sc->sc_dev.dv_xname, __func__));

        if (sc == NULL)
                return 0;

	kbl->min = SATCKBD_BACKLIGHT_LEVEL_MIN;
	kbl->max = SATCKBD_BACKLIGHT_LEVEL_MAX;
	kbl->curval = sc->backlight;

	return 0;
}

int
satckbd_wskbd_set_backlight(struct wskbd_backlight *kbl)
{
	struct satckbd_softc *sc = satckbd_cd.cd_devs[0];
	int value = kbl->curval;

	DPRINTF(("%s: %s -> %d\n", sc->sc_dev.dv_xname, __func__, value));

	if (sc == NULL)
		return -1;

	if (value < SATCKBD_BACKLIGHT_LEVEL_MIN)
		value = SATCKBD_BACKLIGHT_LEVEL_MIN;
	if (value > SATCKBD_BACKLIGHT_LEVEL_MAX)
		value = SATCKBD_BACKLIGHT_LEVEL_MAX;

	sc->backlight = value;
	task_add(systq, &sc->sc_task_backlight);

	return 0;
}

void
satckbd_proc_key(struct satckbd_softc *sc, int key, int fn, int event_type)
{
	struct wscons_keymap wkm;
	int x, trans = 0;

	DPRINTF(("%s: key %s: %d (fn %d)\n", sc->sc_dev.dv_xname,
	    (event_type == WSCONS_EVENT_KEY_DOWN ? "down" : "up"), key, fn));

	if (fn) {
		wskbd_get_mapentry(&satckbd_mapdata, key, &wkm);

		for (x = 0; x < nitems(satckbd_fn_trans); x++) {
			struct satckbd_fn_trans_entry e = satckbd_fn_trans[x];
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

		c = satckbd_raw_keycodes_us[key];
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
satckbd_recv_msg(struct satckbd_softc *sc, struct satopcase_spi_msg *msg)
{
	struct satckbd_data *kbd_msg = &msg->kbd_data;
	int pressed[SATCKBD_DATA_KEYS + SATCKBD_DATA_MODS] = { 0 };
	int x, y, tdown;

	if (le16toh(msg->type) != SATOPCASE_MSG_TYPE_KBD_DATA) {
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
	for (x = 0; x < SATCKBD_DATA_KEYS; x++)
		pressed[x] = kbd_msg->pressed[x];
	for (x = 0; x < SATCKBD_DATA_MODS; x++)
		pressed[SATCKBD_DATA_KEYS + x] =
		    (kbd_msg->modifiers & (1 << x)) ?
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
			satckbd_proc_key(sc, sc->kbd_keys_down[x], kbd_msg->fn,
			    WSCONS_EVENT_KEY_UP);
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
			satckbd_proc_key(sc, pressed[x], kbd_msg->fn,
			    WSCONS_EVENT_KEY_DOWN);
	}

	memcpy(sc->kbd_keys_down, pressed, sizeof(sc->kbd_keys_down));
}
