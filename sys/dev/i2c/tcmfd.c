/* $OpenBSD$ */
/*
 * Copyright (c) 2025 joshua stein <jcs@jcs.org>
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
 * Toshiba TC3589x series I2C-based MFD devices
 * https://web.archive.org/web/20251213045316/https://toshiba.semicon-storage.com/info/TC35894FG_datasheet_en_20190514.pdf?did=30199&prodName=TC35894FG
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/task.h>
#include <sys/types.h>
#include <sys/malloc.h>

#include <dev/i2c/i2cvar.h>

#include <machine/intr.h>
#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_misc.h>
#include <dev/ofw/ofw_gpio.h>
#include <dev/ofw/ofw_pinctrl.h>
#include <dev/ofw/fdt.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wskbdvar.h>
#include <dev/wscons/wsksymdef.h>
#include <dev/wscons/wsksymvar.h>

/* keypad registers */
#define TC3589x_KBDSETTLE_REG	0x01
#define  TC3589x_KPD_SETTLE_TIME	0xA3
#define TC3589x_KBDBOUNCE	0x02
#define  TC3589x_KPD_DEBOUNCE_PERIOD	0xA3
#define TC3589x_KBDSIZE		0x03
#define TC3589x_KBCFG_LSB	0x04
#define TC3589x_KBCFG_MSB	0x05
#define  TC3589x_KBCFG_DEDICATED_KEY	0xFF
#define TC3589x_KBDIC		0x08
#define  TC3589x_EVT_INT_CLR		0x2
#define  TC3589x_KBD_INT_CLR		0x1
#define TC3589x_KBDMSK		0x09
#define  TC3589x_KBDMSK_KBDCODE	0x0C	/* mask event, unmask scan */
#define  TC3589x_KBDMSK_ALL	0x0F	/* mask everything */
#define TC3589x_KBDCODE0	0x0B
#define  TC3589x_KBDCODE_EMPTY		0x7F
#define  TC3589x_KBDCODE_MULTIKEY	0x80
#define  TC3589x_KBDCODE_COL_MASK	0x0F
#define  TC3589x_KBDCODE_ROW_MASK	0x70
#define  TC3589x_KBDCODE_ROW_SHIFT	4
#define  TC3589x_KBDCODE_NREGS		4

/* system registers */
#define TC3589x_MANFCODE	0x80
#define  TC3589x_MANFCODE_MAGIC		0x03
#define TC3589x_VERSION		0x81
#define TC3589x_RSTCTRL		0x82
#define  TC3589x_RSTCTRL_KBDRST		(1 << 1)
#define  TC3589x_RSTCTRL_ROTRST		(1 << 2)
#define  TC3589x_RSTCTRL_TIMRST		(1 << 3)
#define TC3589x_RSTINTCLR	0x84
#define TC3589x_CLKCFG		0x89
#define TC3589x_CLKEN		0x8A
#define  TC3589x_CLKEN_KPD		0x1
#define TC3589x_KBDMFS		0x8F
#define  TC3589x_KBDMFS_EN		0x1
#define TC3589x_IRQST		0x91
#define  TC3589x_IRQST_KBDIRQ		(1 << 6)
#define  TC3589x_IRQST_PORIRQ		(1 << 7)

/* IO configuration registers */
#define TC3589x_IOCFG		0xA7
#define  TC3589x_IOCFG_IG		0x08
#define TC3589x_IOPULLCFG0_LSB	0xAA
#define TC3589x_IOPULLCFG0_MSB	0xAB
#define TC3589x_IOPULLCFG1_LSB	0xAC
#define TC3589x_IOPULLCFG1_MSB	0xAD
#define TC3589x_IOPULLCFG2_LSB	0xAE
#define  TC3589x_PULLUP_ALL		0xAA

/* direct keyboard registers */
#define TC3589x_DKBDIC		0xF2
#define TC3589x_DKBDMSK		0xF3

/* matrix size */
#define TC3589x_MAX_ROWS	8
#define TC3589x_MAX_KMAP_ROWS	16	/* including Fn layer */
#define TC3589x_MAX_COLS	12

/* translation from KEY_* codes in "linux,keymap" table to our keysyms */
#define KC(n) KS_KEYCODE(n)
static const keysym_t tcmfd_keydesc_us[] = {
/*	pos		normal		shifted */
	KC(1),		KS_Escape,
	KC(2),		KS_1,		KS_exclam,
	KC(3),		KS_2,		KS_at,
	KC(4),		KS_3,		KS_numbersign,
	KC(5),		KS_4,		KS_dollar,
	KC(6),		KS_5,		KS_percent,
	KC(7),		KS_6,		KS_asciicircum,
	KC(8),		KS_7,		KS_ampersand,
	KC(9),		KS_8,		KS_asterisk,
	KC(10),		KS_9,		KS_parenleft,
	KC(11),		KS_0,		KS_parenright,
	KC(12),		KS_minus,	KS_underscore,
	KC(13),		KS_equal,	KS_plus,
	KC(14),		KS_Delete, /*KS_BackSpace, */
	KC(15),		KS_Tab,
	KC(16),		KS_q,
	KC(17),		KS_w,
	KC(18),		KS_e,
	KC(19),		KS_r,
	KC(20),		KS_t,
	KC(21),		KS_y,
	KC(22),		KS_u,
	KC(23),		KS_i,
	KC(24),		KS_o,
	KC(25),		KS_p,
	KC(26),		KS_bracketleft,	KS_braceleft,
	KC(27),		KS_bracketright, KS_braceright,
	KC(28),		KS_Return,
	KC(29),		KS_Cmd1,	KS_Control_L,
	KC(30),		KS_a,
	KC(31),		KS_s,
	KC(32),		KS_d,
	KC(33),		KS_f,
	KC(34),		KS_g,
	KC(35),		KS_h,
	KC(36),		KS_j,
	KC(37),		KS_k,
	KC(38),		KS_l,
	KC(39),		KS_semicolon,	KS_colon,
	KC(40),		KS_apostrophe,	KS_quotedbl,
	KC(41),		KS_grave,	KS_asciitilde,
	KC(42),		KS_Shift_L,
	KC(43),		KS_backslash,	KS_bar,
	KC(44),		KS_z,
	KC(45),		KS_x,
	KC(46),		KS_c,
	KC(47),		KS_v,
	KC(48),		KS_b,
	KC(49),		KS_n,
	KC(50),		KS_m,
	KC(51),		KS_comma,	KS_less,
	KC(52),		KS_period,	KS_greater,
	KC(53),		KS_slash,	KS_question,
	KC(54),		KS_Shift_R,
	KC(56),		KS_Cmd2,	KS_Alt_L,
	KC(57),		KS_space,
	KC(58),		KS_Caps_Lock,
	KC(59),		KS_Cmd_Screen0,	KS_f1,
	KC(60),		KS_Cmd_Screen1,	KS_f2,
	KC(61),		KS_Cmd_Screen2,	KS_f3,
	KC(62),		KS_Cmd_Screen3,	KS_f4,
	KC(63),		KS_Cmd_Screen4,	KS_f5,
	KC(64),		KS_Cmd_Screen5,	KS_f6,
	KC(65),		KS_Cmd_Screen6,	KS_f7,
	KC(66),		KS_Cmd_Screen7,	KS_f8,
	KC(67),		KS_Cmd_Screen8,	KS_f9,
	KC(68),		KS_Cmd_Screen9,	KS_f10,
	KC(85),		KS_grave,	KS_asciitilde,
	KC(89),		KS_backslash,	KS_underscore,
	KC(97),		KS_Control_R,
	KC(100),	KS_Alt_R,
	KC(103),	KS_Up,
	KC(105),	KS_Left,
	KC(106),	KS_Right,
	KC(108),	KS_Down,
	KC(110),	KS_Insert,
	KC(111),	KS_Delete,
	KC(124),	KS_backslash,	KS_bar,
	KC(139),	KS_Menu,
	KC(464),	KS_Mode_switch,

	/* fn layer keysyms */
	KC(300),	KS_Cmd_ScrollBack, KS_Prior,
	KC(301),	KS_Home,
	KC(302),	KS_End,
	KC(303),	KS_Cmd_ScrollFwd, KS_Next,
};

static const keysym_t tcmfd_keydesc_jp[] = {
/*	pos		normal		shifted */
	KC(3),		KS_2,		KS_quotedbl,
	KC(7),		KS_6,		KS_ampersand,
	KC(8),		KS_7,		KS_apostrophe,
	KC(9),		KS_8,		KS_parenleft,
	KC(10),		KS_9,		KS_parenright,
	KC(11),		KS_0,
	KC(12),		KS_minus,	KS_equal,
	KC(26),		KS_at,		KS_grave,
	KC(27),		KS_bracketleft,	KS_braceleft,
	KC(39),		KS_semicolon,	KS_plus,
	KC(40),		KS_colon,	KS_asterisk,
	KC(41),		KS_asciicircum,	KS_asciitilde,
	KC(43),		KS_bracketright, KS_braceright,
	KC(85),		KS_Zenkaku_Hankaku,
};

/* fn layer */
static const struct {
	keysym_t from;
	keysym_t to;
} tcmfd_fn_trans[] = {
	{ 103,		300 },	/* Up -> Page Up */
	{ 105,		301 },	/* Left -> Home */
	{ 106,		302 },	/* Right -> End */
	{ 108,		303 },	/* Down -> Page Down */
};

#define KBD_MAP(name, base, map) \
	{ name, base, sizeof(map) / sizeof(keysym_t), map }
static const struct wscons_keydesc tcmfd_keydesctab[] = {
	KBD_MAP(KB_US, 0, tcmfd_keydesc_us),
	KBD_MAP(KB_JP, KB_US, tcmfd_keydesc_jp),
	{ 0 },
};
struct wskbd_mapdata tcmfd_keymapdata = {
	tcmfd_keydesctab,
	KB_US,
};

#define MAXKBDEVENTS	(TC3589x_MAX_ROWS * TC3589x_MAX_COLS)
#define KDOWN		0x0000
#define KUP		0x8000
#define CODEMASK	0x7fff

#ifdef WSDISPLAY_COMPAT_RAWKBD
/* translation table from our raw keycodes to AT set 1 XT scancodes */
static const u_int8_t tcmfd_trtab[256] = {
       0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, /* 000 - 007 */
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, /* 008 - 015 */
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, /* 016 - 023 */
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, /* 024 - 031 */
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, /* 032 - 039 */
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, /* 040 - 047 */
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,    0, /* 048 - 055 */
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, /* 056 - 063 */
    0x40, 0x41, 0x42, 0x43, 0x44,    0,    0,    0, /* 064 - 071 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 072 - 079 */
       0,    0,    0,    0,    0, 0x29,    0, 0x57, /* 080 - 087 */
    0x58, 0x73,    0,    0, 0x79,    0, 0x7b,    0, /* 088 - 095 */
       0, 0x9d,    0,    0, 0xb8,    0, 0xc7, 0xc8, /* 096 - 103 */
    0xc9, 0xcb, 0xcd, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, /* 104 - 111 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 112 - 119 */
       0,    0,    0,    0, 0x7d,    0,    0,    0, /* 120 - 127 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 128 - 135 */
       0,    0,    0, 0xdd,    0,    0,    0,    0, /* 136 - 143 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 144 - 151 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 152 - 159 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 160 - 167 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 168 - 175 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 176 - 183 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 184 - 191 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 192 - 199 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 200 - 207 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 208 - 215 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 216 - 223 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 224 - 231 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 232 - 239 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 240 - 247 */
       0,    0,    0,    0,    0,    0,    0,    0, /* 248 - 255 */
};
#endif

struct tcmfd_softc {
	struct device sc_dev;
	i2c_tag_t sc_tag;
	i2c_addr_t sc_addr;
	uint32_t sc_gpio[4];
	struct taskq *sc_taskq;
	struct task sc_task;
	void *sc_irq;
	int sc_bus_held;
	int sc_node;
	int sc_poll;

	struct {
		int node;
		int rows;
		int cols;
		uint16_t keymap[TC3589x_MAX_KMAP_ROWS][TC3589x_MAX_COLS];
		uint16_t states[TC3589x_MAX_ROWS + 1];
		struct device *wskbddev;
		int rawkbd;
		int debounce_pd;
		int fn_down;
		int fn_code;
		uint16_t fn_states[TC3589x_MAX_ROWS];
	} keyboard;
};

int	tcmfd_match(struct device *, void *, void *);
void	tcmfd_attach(struct device *, struct device *, void *);
int	tcmfd_activate(struct device *, int);
int	tcmfd_intr(void *);
void	tcmfd_task(void *);

int	tcmfd_reg_read(struct tcmfd_softc *, uint8_t);
int	tcmfd_reg_write(struct tcmfd_softc *, uint8_t, uint8_t);
int	tcmfd_set_bits(struct tcmfd_softc *sc, uint8_t, uint8_t, uint8_t);

void	tcmfd_process_keys(struct tcmfd_softc *);
int	tcmfd_keyboard_attach(struct tcmfd_softc *);
int	tcmfd_keyboard_read_keys(struct tcmfd_softc *, u_int *, int *);
int	tcmfd_keyboard_is_ghost_key(struct tcmfd_softc *, int, int, uint16_t *,
	    uint16_t *);
int	tcmfd_keyboard_fn_lookup(int);
void	tcmfd_keyboard_enable(void *);

int	tcmfd_keyboard_wsenable(void *, int);
void	tcmfd_keyboard_set_leds(void *, int);
int	tcmfd_keyboard_ioctl(void *, u_long, caddr_t, int, struct proc *);
void	tcmfd_keyboard_cngetc(void *, u_int *, int *);
void	tcmfd_keyboard_cnpollc(void *, int);

struct wskbd_accessops tcmfd_keyboard_accessops = {
	tcmfd_keyboard_wsenable,
	tcmfd_keyboard_set_leds,
	tcmfd_keyboard_ioctl,
};

struct wskbd_consops tcmfd_keyboard_consops = {
	tcmfd_keyboard_cngetc,
	tcmfd_keyboard_cnpollc,
};

const struct cfattach tcmfd_ca = {
	sizeof(struct tcmfd_softc), tcmfd_match, tcmfd_attach, NULL,
	tcmfd_activate
};

struct cfdriver tcmfd_cd = {
	NULL, "tcmfd", DV_TTY
};

int
tcmfd_match(struct device *parent, void *match, void *aux)
{
	struct i2c_attach_args *ia = aux;

	if (strcmp(ia->ia_name, "toshiba,tc35894") == 0)
		return 1;

	return 0;
}

void
tcmfd_attach(struct device *parent, struct device *self, void *aux)
{
	struct tcmfd_softc *sc = (struct tcmfd_softc *)self;
	struct i2c_attach_args *ia = aux;
	struct wskbddev_attach_args waa;
	int tnode, len, nkeys, i, row, col;
	uint32_t *keys;
	uint8_t reg;

	sc->sc_tag = ia->ia_tag;
	sc->sc_addr = ia->ia_addr;
	sc->sc_node = *(int *)ia->ia_cookie;

	OF_getpropintarray(sc->sc_node, "reset-gpios", sc->sc_gpio,
	    sizeof(sc->sc_gpio));

	printf("\n");

	sc->sc_taskq = taskq_create(sc->sc_dev.dv_xname, 1, IPL_TTY, 0);
	task_set(&sc->sc_task, tcmfd_task, sc);

	/* find keypad child node */
	for (tnode = OF_child(sc->sc_node); tnode != 0;
	    tnode = OF_peer(tnode)) {
		if (OF_is_compatible(tnode, "toshiba,tc3589x-keypad")) {
			sc->keyboard.node = tnode;
			break;
		}
	}

	if (!sc->keyboard.node) {
		printf("%s: no keypad child node\n", sc->sc_dev.dv_xname);
		return;
	}

	/* verify params */
	sc->keyboard.rows = OF_getpropint(sc->keyboard.node,
	    "keypad,num-rows", 0);
	sc->keyboard.cols = OF_getpropint(sc->keyboard.node,
	    "keypad,num-columns", 0);

	if (sc->keyboard.rows < 1 || sc->keyboard.rows > TC3589x_MAX_ROWS ||
	    sc->keyboard.cols < 1 || sc->keyboard.cols > TC3589x_MAX_COLS) {
		printf("%s: invalid rows %d cols %d\n", sc->sc_dev.dv_xname,
		    sc->keyboard.rows, sc->keyboard.cols);
		return;
	}

	sc->keyboard.debounce_pd = OF_getpropint(sc->keyboard.node,
	    "debounce-delay-ms", 0);
	if (sc->keyboard.debounce_pd) {
		sc->keyboard.debounce_pd *= 16;
		if (sc->keyboard.debounce_pd > 0xff)
			sc->keyboard.debounce_pd = TC3589x_KPD_DEBOUNCE_PERIOD;
	} else
		sc->keyboard.debounce_pd = TC3589x_KPD_DEBOUNCE_PERIOD;

	sc->sc_poll = 1;

	pinctrl_byname(sc->sc_node, "default");

	/* tc3589x hardware reset: assert for 1ms, wait 1ms after deassert */
	if (sc->sc_gpio[0]) {
		gpio_controller_config_pin(sc->sc_gpio, GPIO_CONFIG_OUTPUT);
		delay(10);
		gpio_controller_set_pin(sc->sc_gpio, 1);
		delay(1000);
		gpio_controller_set_pin(sc->sc_gpio, 0);
		delay(1000);
	}

	/* wait for power-on-reset to complete */
	for (i = 0; i < 10; i++) {
		reg = tcmfd_reg_read(sc, TC3589x_IRQST);
		if (reg & TC3589x_IRQST_PORIRQ) {
			tcmfd_reg_write(sc, TC3589x_RSTINTCLR, 0x01);
			break;
		}
		delay(1000);
	}

	/* clear direct key interrupt */
	tcmfd_reg_write(sc, TC3589x_DKBDIC, 0x01);

	/*
	 * "After power-on-reset, the general call functionality can be used
	 * only after having read out the manufacturer code (0x80) and the
	 * software version number (0x81) of the TC35894FG."
	 */
	reg = tcmfd_reg_read(sc, TC3589x_MANFCODE);
	if (reg != TC3589x_MANFCODE_MAGIC) {
		printf("%s: invalid manfcode 0x%x\n", sc->sc_dev.dv_xname, reg);
		return;
	}
	tcmfd_reg_read(sc, TC3589x_VERSION);

	/* put timer, rotary, and keyboard into reset */
	tcmfd_reg_write(sc, TC3589x_RSTCTRL,
	    TC3589x_RSTCTRL_TIMRST | TC3589x_RSTCTRL_ROTRST |
	    TC3589x_RSTCTRL_KBDRST);

	/* clear reset interrupt */
	tcmfd_reg_write(sc, TC3589x_RSTINTCLR, 0x1);

	/* mask direct keyboard interrupts */
	tcmfd_reg_write(sc, TC3589x_DKBDMSK, 0x03);

	/* parse linux,keymap: each entry is (row << 24 | col << 16 | code) */
	len = OF_getproplen(sc->keyboard.node, "linux,keymap");
	if (len > 0) {
		nkeys = len / sizeof(uint32_t);
		keys = malloc(len, M_DEVBUF, M_WAITOK);
		OF_getpropintarray(sc->keyboard.node, "linux,keymap", keys,
		    len);
		for (i = 0; i < nkeys; i++) {
			row = (keys[i] >> 24) & 0xff;
			col = (keys[i] >> 16) & 0xff;
			if (row < TC3589x_MAX_KMAP_ROWS &&
			    col < TC3589x_MAX_COLS)
				sc->keyboard.keymap[row][col] =
				    keys[i] & 0xffff;
		}
		free(keys, M_DEVBUF, len);
	}

	/* find fn key */
	sc->keyboard.fn_code = -1;
	for (i = 1; i < nitems(tcmfd_keydesc_us); i++) {
		if (tcmfd_keydesc_us[i] == KS_Mode_switch) {
			sc->keyboard.fn_code = tcmfd_keydesc_us[i - 1] & 0xfff;
			break;
		}
	}

	/* pull the keypad module out of reset */
	tcmfd_set_bits(sc, TC3589x_RSTCTRL, TC3589x_RSTCTRL_KBDRST, 0x0);

	/* configure KBDMFS */
	tcmfd_set_bits(sc, TC3589x_KBDMFS, 0x0, TC3589x_KBDMFS_EN);

	/* configure clock */
	tcmfd_reg_write(sc, TC3589x_CLKCFG, 0x43 /* XXX: magic */);
	tcmfd_set_bits(sc, TC3589x_CLKEN, 0x0, TC3589x_CLKEN_KPD);

	/* clear pending IRQs */
	tcmfd_set_bits(sc, TC3589x_RSTINTCLR, 0x0, 0x1);

	/* init kbd hardware */
	tcmfd_reg_write(sc, TC3589x_KBDSIZE,
	    (sc->keyboard.rows << TC3589x_KBDCODE_ROW_SHIFT) |
	    sc->keyboard.cols);

	tcmfd_reg_write(sc, TC3589x_KBCFG_LSB, TC3589x_KBCFG_DEDICATED_KEY);
	tcmfd_reg_write(sc, TC3589x_KBCFG_MSB, TC3589x_KBCFG_DEDICATED_KEY);

	tcmfd_reg_write(sc, TC3589x_KBDSETTLE_REG, TC3589x_KPD_SETTLE_TIME);
	tcmfd_reg_write(sc, TC3589x_KBDBOUNCE, sc->keyboard.debounce_pd);

	tcmfd_set_bits(sc, TC3589x_IOCFG, 0x0, TC3589x_IOCFG_IG);

	/* configure pull-up resistors for all row gpios */
	tcmfd_reg_write(sc, TC3589x_IOPULLCFG0_LSB, TC3589x_PULLUP_ALL);
	tcmfd_reg_write(sc, TC3589x_IOPULLCFG0_MSB, TC3589x_PULLUP_ALL);

	/* configure pull-up resistors for all column gpios */
	tcmfd_reg_write(sc, TC3589x_IOPULLCFG1_LSB, TC3589x_PULLUP_ALL);
	tcmfd_reg_write(sc, TC3589x_IOPULLCFG1_MSB, TC3589x_PULLUP_ALL);
	tcmfd_reg_write(sc, TC3589x_IOPULLCFG2_LSB, TC3589x_PULLUP_ALL);

	/* flush events and clear all pending keyboard interrupts */
	for (i = 0; i < TC3589x_KBDCODE_NREGS; i++)
		tcmfd_reg_read(sc, TC3589x_KBDCODE0 + i);
	tcmfd_reg_write(sc, TC3589x_KBDIC,
	    TC3589x_EVT_INT_CLR | TC3589x_KBD_INT_CLR);

	sc->sc_irq = fdt_intr_establish(sc->sc_node, IPL_TTY, tcmfd_intr,
	    sc, sc->sc_dev.dv_xname);
	if (sc->sc_irq == NULL)
		printf("%s: can't establish interrupt\n", sc->sc_dev.dv_xname);

	wskbd_cnattach(&tcmfd_keyboard_consops, sc, &tcmfd_keymapdata);
	waa.console = 1;
	waa.keymap = &tcmfd_keymapdata;
	waa.accessops = &tcmfd_keyboard_accessops;
	waa.accesscookie = sc;
	sc->keyboard.wskbddev = config_found((void *)sc, &waa, wskbddevprint);

	/* enable scan interrupts for KBDCODE mode, mask event interrupts */
	tcmfd_reg_write(sc, TC3589x_KBDMSK, TC3589x_KBDMSK_KBDCODE);
}

int
tcmfd_activate(struct device *self, int act)
{
	struct tcmfd_softc *sc = (struct tcmfd_softc *)self;

	switch (act) {
	case DVACT_SUSPEND:
		if (sc->sc_irq != NULL)
			fdt_intr_disable(sc->sc_irq);
		task_del(sc->sc_taskq, &sc->sc_task);
		break;
	case DVACT_RESUME:
		if (sc->sc_irq != NULL)
			fdt_intr_enable(sc->sc_irq);
		break;
	}

	return 0;
}

int
tcmfd_set_bits(struct tcmfd_softc *sc, uint8_t reg, uint8_t mask, uint8_t val)
{
	int ret;

	ret = tcmfd_reg_read(sc, reg);
	if (ret < 0)
		return ret;

	ret &= ~mask;
	ret |= val;

	return tcmfd_reg_write(sc, reg, ret);
}

int
tcmfd_reg_read(struct tcmfd_softc *sc, uint8_t reg)
{
	uint8_t cmd = reg, val;
	int flags = (sc->sc_poll || sc->sc_bus_held) ? I2C_F_POLL : 0;
	int error;

	if (!sc->sc_bus_held)
		iic_acquire_bus(sc->sc_tag, flags);
	error = iic_exec(sc->sc_tag, I2C_OP_READ_WITH_STOP, sc->sc_addr,
	    &cmd, sizeof(cmd), &val, sizeof(val), I2C_F_POLL);
	if (!sc->sc_bus_held)
		iic_release_bus(sc->sc_tag, flags);

	if (error) {
		printf("%s: can't read register 0x%02x\n", sc->sc_dev.dv_xname,
		    reg);
		return -1;
	}

	return val;
}

int
tcmfd_reg_write(struct tcmfd_softc *sc, uint8_t reg, uint8_t val)
{
	uint8_t cmd = reg;
	int flags = sc->sc_poll ? I2C_F_POLL : 0;
	int error;

	if (!sc->sc_bus_held)
		iic_acquire_bus(sc->sc_tag, flags);
	error = iic_exec(sc->sc_tag, I2C_OP_WRITE_WITH_STOP, sc->sc_addr, &cmd,
	    sizeof(cmd), &val, sizeof(val), flags);
	if (!sc->sc_bus_held)
		iic_release_bus(sc->sc_tag, flags);

	if (error) {
		printf("%s: can't write register 0x%02x\n",
		    sc->sc_dev.dv_xname, reg);
		return -1;
	}

	return 0;
}

int
tcmfd_intr(void *arg)
{
	struct tcmfd_softc *sc = arg;

	/* if we can bus lock, process keys now so we are responsive */
	if (iic_acquire_bus(sc->sc_tag, I2C_F_TRYLOCK) == 0) {
		sc->sc_bus_held = 1;
		tcmfd_process_keys(sc);
		sc->sc_bus_held = 0;
		iic_release_bus(sc->sc_tag, 0);
	} else {
		/* busy; defer i2c transactions to a non-interrupt context */
		task_add(sc->sc_taskq, &sc->sc_task);
	}

	return 1;
}

void
tcmfd_task(void *arg)
{
	struct tcmfd_softc *sc = arg;

	iic_acquire_bus(sc->sc_tag, I2C_F_POLL);
	sc->sc_bus_held = 1;
	tcmfd_process_keys(sc);
	sc->sc_bus_held = 0;
	iic_release_bus(sc->sc_tag, I2C_F_POLL);
}

void
tcmfd_process_keys(struct tcmfd_softc *sc)
{
	int status;

	for (;;) {
		status = tcmfd_reg_read(sc, TC3589x_IRQST);
		if (status < 0 || !(status & TC3589x_IRQST_KBDIRQ))
			return;

		tcmfd_keyboard_read_keys(sc, NULL, NULL);
	}
}

/*
 * If a key at (row, col) has another held key in the same row and another held
 * key in the same column, it could be a ghost key press
 */
int
tcmfd_keyboard_is_ghost_key(struct tcmfd_softc *sc, int row, int col,
    uint16_t *prev_states, uint16_t *new_states)
{
	int r;

	/* any other key currently held in the same row? */
	if ((sc->keyboard.states[row] & ~(1 << col)) == 0)
		return 0;

	/* any key currently held in the same column in another row? */
	for (r = 0; r < sc->keyboard.rows; r++) {
		if (r == row)
			continue;
		if ((sc->keyboard.states[r] | prev_states[r] |
		    new_states[r]) & (1 << col))
			return 1;
	}

	return 0;
}

int
tcmfd_keyboard_read_keys(struct tcmfd_softc *sc, u_int *type, int *data)
{
	int col, row, i, s, code, nkeys = 0, nevents = 0;
	uint16_t new_states[TC3589x_MAX_ROWS], old_states[TC3589x_MAX_ROWS];
	uint16_t ibuf[MAXKBDEVENTS];
	uint8_t k;

	memset(new_states, 0, sizeof(new_states));

	/* read currently pressed keys from KBDCODE snapshot registers */
	for (i = 0; i < TC3589x_KBDCODE_NREGS; i++) {
		k = tcmfd_reg_read(sc, TC3589x_KBDCODE0 + i);
		if (k == TC3589x_KBDCODE_EMPTY)
			continue;

		nkeys++;
		col = k & TC3589x_KBDCODE_COL_MASK;
		row = (k & TC3589x_KBDCODE_ROW_MASK) >>
		    TC3589x_KBDCODE_ROW_SHIFT;
		new_states[row] |= (1 << col);
	}

	/* clear scan interrupt (reading all KBDCODE regs also clears RSINT) */
	tcmfd_reg_write(sc, TC3589x_KBDIC, TC3589x_KBD_INT_CLR);

	memcpy(old_states, sc->keyboard.states, sizeof(old_states));

	/* pass 1: key releases */
	for (row = 0; row < sc->keyboard.rows; row++) {
		for (col = 0; col < sc->keyboard.cols; col++) {
			if (!(sc->keyboard.states[row] & (1 << col)))
				continue;
			if (new_states[row] & (1 << col))
				continue;

			code = sc->keyboard.keymap[row][col];
			if (code == 0)
				continue;

			sc->keyboard.states[row] &= ~(1 << col);

			if (code == sc->keyboard.fn_code) {
				sc->keyboard.fn_down = 0;
				continue;
			}

			if (sc->keyboard.fn_states[row] & (1 << col)) {
				sc->keyboard.fn_states[row] &= ~(1 << col);
				for (i = 0; i < nitems(tcmfd_fn_trans); i++) {
					if (tcmfd_fn_trans[i].from == code) {
						code = tcmfd_fn_trans[i].to;
						break;
					}
				}
			}

			if (type != NULL) {
				*type = WSCONS_EVENT_KEY_UP;
				*data = code;
				return 1;
			}
			ibuf[nevents++] = code | KUP;
		}
	}

	/* pass 2: key presses with per-key ghost check */
	for (row = 0; row < sc->keyboard.rows; row++) {
		for (col = 0; col < sc->keyboard.cols; col++) {
			if (sc->keyboard.states[row] & (1 << col))
				continue;
			if (!(new_states[row] & (1 << col)))
				continue;

			code = sc->keyboard.keymap[row][col];
			if (code == 0)
				continue;

			if (tcmfd_keyboard_is_ghost_key(sc, row, col,
			    old_states, new_states))
				continue;

			sc->keyboard.states[row] |= (1 << col);

			if (code == sc->keyboard.fn_code) {
				sc->keyboard.fn_down = 1;
				continue;
			}

			if (sc->keyboard.fn_down) {
				for (i = 0; i < nitems(tcmfd_fn_trans); i++) {
					if (tcmfd_fn_trans[i].from == code) {
						sc->keyboard.fn_states[row] |=
						    (1 << col);
						code = tcmfd_fn_trans[i].to;
						break;
					}
				}
			}

			if (type != NULL) {
				*type = WSCONS_EVENT_KEY_DOWN;
				*data = code;
				return 1;
			}
			ibuf[nevents++] = code | KDOWN;
		}
	}

#ifdef WSDISPLAY_COMPAT_RAWKBD
	if (sc->keyboard.rawkbd) {
		u_char cbuf[MAXKBDEVENTS * 2];
		int j;
		u_int8_t c;

		for (i = j = 0; i < nevents; i++) {
			if ((ibuf[i] & CODEMASK) >= nitems(tcmfd_trtab))
				continue;
			c = tcmfd_trtab[ibuf[i] & CODEMASK];
			if (c == 0)
				continue;
			if (c & 0x80)
				cbuf[j++] = 0xe0;
			cbuf[j] = c & 0x7f;
			if (ibuf[i] & KUP)
				cbuf[j] |= 0x80;
			j++;
		}
		s = spltty();
		wskbd_rawinput(sc->keyboard.wskbddev, cbuf, j);
		splx(s);

		return nevents;
	}
#endif

	s = spltty();
	for (i = 0; i < nevents; i++) {
		wskbd_input(sc->keyboard.wskbddev, ibuf[i] & KUP ?
		    WSCONS_EVENT_KEY_UP : WSCONS_EVENT_KEY_DOWN,
		    ibuf[i] & CODEMASK);
	}
	splx(s);

	return nevents;
}

void
tcmfd_keyboard_cngetc(void *cookie, u_int *type, int *data)
{
	struct tcmfd_softc *sc = cookie;

	while (tcmfd_keyboard_read_keys(sc, type, data) == 0)
		delay(10000);
}

void
tcmfd_keyboard_cnpollc(void *cookie, int on)
{
	struct tcmfd_softc *sc = cookie;

	sc->sc_poll = on;

	tcmfd_reg_write(sc, TC3589x_KBDMSK,
	    on ? TC3589x_KBDMSK_ALL : TC3589x_KBDMSK_KBDCODE);
}

int
tcmfd_keyboard_wsenable(void *cookie, int on)
{
	return 0;
}

void
tcmfd_keyboard_set_leds(void *cookie, int on)
{
}

int
tcmfd_keyboard_ioctl(void *cookie, u_long cmd, caddr_t data, int flag,
    struct proc *p)
{
#ifdef WSDISPLAY_COMPAT_RAWKBD
	struct tcmfd_softc *sc = cookie;
#endif

	switch (cmd) {
	case WSKBDIO_GTYPE:
		/* maybe we are the spiritual successor */
		*(int *)data = WSKBD_TYPE_ZAURUS;
		return 0;
	case WSKBDIO_SETLEDS:
		return 0;
	case WSKBDIO_GETLEDS:
		*(int *)data = 0;
		return 0;
#ifdef WSDISPLAY_COMPAT_RAWKBD
	case WSKBDIO_SETMODE:
		sc->keyboard.rawkbd = *(int *)data == WSKBD_RAW;
		return (0);
#endif
	}

	return -1;
}
