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

#include <sys/param.h>
#include <sys/device.h>
#include <sys/systm.h>

#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_pinctrl.h>
#include <dev/ofw/ofw_clock.h>
#include <dev/ofw/ofw_misc.h>
#include <dev/ofw/ofw_power.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

/* GRF_LVDS_CON0 register bits, from U-Boot rk3036_lcdc */
#define	GRF_LVDS_CON0		0x0150
#define	 LVDS_CON0_DATA_SEL	(1 << 0)  /* bit 0 - 0=LCDC, 1=EBC */
#define	 LVDS_CON0_FORMAT_MASK	(3 << 1)  /* bits 2:1 - format select */
#define	 LVDS_CON0_FORMAT_SHIFT	1
#define	 LVDS_CON0_MSBSEL	(1 << 3)  /* bit 3 - MSB select (1=D7) */
#define	 LVDS_CON0_MODE_EN	(1 << 6)  /* bit 6 - LVDS mode enable */
#define	 LVDS_CON0_TTL_EN	(1 << 7)  /* bit 7 - TTL mode enable */
#define	 LVDS_CON0_LANE0_EN	(1 << 8)  /* bit 8 - MIPI PHY lane0 enable */
#define	 LVDS_CON0_FORCEX_EN	(1 << 9)  /* bit 9 - force TX enable */
#define	 LVDS_CON0_WRITE_MASK	(0x3ff << 16)

#define	LVDS_FORMAT_VESA_24		0	/* LVDS_8BIT_1 */
#define	LVDS_FORMAT_JEIDA_24		2	/* LVDS_8BIT_3 */
#define	LVDS_FORMAT_JEIDA_18		3	/* LVDS_6BIT */

/* MIPI/LVDS PHY registers */

#define	MIPIPHY_REG0		0x0000	/* ANALOG reg 0x00 */
#define	 BANDGAP_POWER_DOWN	(1 << 7)
#define	 LANE_EN_CK		(1 << 6)
#define	 LANE_EN_3		(1 << 5)
#define	 LANE_EN_2		(1 << 4)
#define	 LANE_EN_1		(1 << 3)
#define	 LANE_EN_0		(1 << 2)
#define	 POWER_WORK_ENABLE	(1 << 0)
#define	 POWER_WORK_DISABLE	(2 << 0)

#define	MIPIPHY_REG1		0x0004	/* ANALOG reg 0x01 */
#define	 REG_DA_SYNCRST		(1 << 2)
#define	 REG_DA_LDOPD		(1 << 1)
#define	 REG_DA_PLLPD		(1 << 0)

#define	MIPIPHY_REG3		0x000c	/* ANALOG reg 0x03 */
#define	 REG_FBDIV_HI(val)	(((val) >> 8) << 5)
#define	 REG_PREDIV(val)	((val) & 0x1f)

#define	MIPIPHY_REG4		0x0010	/* ANALOG reg 0x04 */
#define	 REG_FBDIV_LO(val)	((val) & 0xff)

#define	MIPIPHY_REG8		0x0020	/* ANALOG reg 0x08 */
#define	 SAMPLE_CLK_DIR_REVERSE	(1 << 4)
#define	 PLL_OUTPUT_DIV_BY_1	(0 << 5)

#define	MIPIPHY_REG1E		0x0078	/* ANALOG reg 0x1e */
#define	 PLL_MODE_SEL_LVDS	(0 << 5)
#define	 PLL_MODE_SEL_MIPI	(1 << 5)
#define	 PLL_MODE_SEL_MASK	(3 << 5)

#define	MIPIPHY_REGE0		0x0380	/* LVDS reg 0x00 */
#define	 LVDS_DIG_RESET_ENABLE	0
#define	 LVDS_DIG_RESET_DISABLE	(1 << 2)

#define	MIPIPHY_REGE1		0x0384	/* LVDS reg 0x01 */
#define	 LVDS_DIG_INTERNAL_ENABLE (1 << 7)

#define	MIPIPHY_REGE3		0x038c	/* LVDS reg 0x03 */
#define	 REG_MIPI_EN		(1 << 0)
#define	 REG_LVDS_EN		(1 << 1)
#define	 REG_TTL_EN		(1 << 2)

#define	MIPIPHY_REGE8		0x03a0	/* LVDS reg 0x08 */

#define	MIPIPHY_REGEB		0x03ac	/* LVDS reg 0x0b */
#define	 REG_LANE0_EN		(1 << 7)
#define	 REG_LANE1_EN		(1 << 6)
#define	 REG_LANE2_EN		(1 << 5)
#define	 REG_LANE3_EN		(1 << 4)
#define	 REG_LANECLK_EN		(1 << 3)
#define	 REG_PLL_POWER_OFF	(1 << 2)
#define	 REG_BANDGAP_POWER_DOWN	(1 << 0)

#define	PHY_READ(sc, reg) regmap_read_4((sc)->sc_phy, (reg))
#define	PHY_WRITE(sc, reg, val) regmap_write_4((sc)->sc_phy, (reg), (val))

struct rklvds_softc;

struct rklvds_connector {
	struct drm_connector	base;
	struct rklvds_softc	*sc;
};

struct rklvds_softc {
	struct device		sc_dev;
	int			sc_node;

	struct regmap		*sc_grf;
	struct regmap		*sc_phy;
	uint32_t		sc_phy_phandle;
	int			sc_phy_node;

	int			sc_data_width;
	int			sc_format;

	/* DRM */
	struct drm_encoder	sc_encoder;
	struct rklvds_connector	sc_connector;
	struct drm_panel	*sc_panel;
	struct device_ports	sc_ports;
	int			sc_activated;
};

#define	to_rklvds_encoder(x)	container_of(x, struct rklvds_softc, sc_encoder)
#define	to_rklvds_connector(x)	container_of(x, struct rklvds_connector, base)

int	rklvds_match(struct device *, void *, void *);
void	rklvds_attach(struct device *, struct device *, void *);
int	rklvds_activate(struct device *, int);

void	rklvds_phy_power_on(struct rklvds_softc *);
void	rklvds_phy_power_off(struct rklvds_softc *);

int	rklvds_ep_activate(void *, struct endpoint *, void *);
void	*rklvds_ep_get_cookie(void *, struct endpoint *);

void	rklvds_encoder_enable(struct drm_encoder *);
void	rklvds_encoder_disable(struct drm_encoder *);

enum drm_connector_status rklvds_connector_detect(struct drm_connector *, bool);
void	rklvds_connector_destroy(struct drm_connector *);
int	rklvds_connector_get_modes(struct drm_connector *);

const struct cfattach rklvds_ca = {
	sizeof(struct rklvds_softc), rklvds_match, rklvds_attach, NULL,
	rklvds_activate
};

struct cfdriver rklvds_cd = {
	NULL, "rklvds", DV_DULL
};

struct drm_encoder_funcs rklvds_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

struct drm_encoder_helper_funcs rklvds_encoder_helper_funcs = {
	.enable = rklvds_encoder_enable,
	.disable = rklvds_encoder_disable,
};

struct drm_connector_funcs rklvds_connector_funcs = {
	.detect = rklvds_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = rklvds_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

struct drm_connector_helper_funcs rklvds_connector_helper_funcs = {
	.get_modes = rklvds_connector_get_modes,
};

int
rklvds_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return (OF_is_compatible(faa->fa_node, "rockchip,rk3126-lvds") ||
	    OF_is_compatible(faa->fa_node, "rockchip,rk3128-lvds"));
}

void
rklvds_attach(struct device *parent, struct device *self, void *aux)
{
	struct rklvds_softc *sc = (struct rklvds_softc *)self;
	struct fdt_attach_args *faa = aux;
	int phy_node, len, nregs, idx;
	uint32_t grf, reg[8];
	char mapping[32];

	sc->sc_node = faa->fa_node;

	grf = OF_getpropint(sc->sc_node, "rockchip,grf", 0);
	sc->sc_grf = regmap_byphandle(grf);
	if (sc->sc_grf == NULL) {
		printf(": no grf\n");
		return;
	}

	sc->sc_phy_phandle = OF_getpropint(sc->sc_node, "phys", 0);
	if (sc->sc_phy_phandle == 0) {
		printf(": no phys property\n");
		return;
	}

	phy_node = OF_getnodebyphandle(sc->sc_phy_phandle);
	if (phy_node == 0) {
		printf(": can't find phy node\n");
		return;
	}
	sc->sc_phy_node = phy_node;

	power_domain_enable(phy_node);
	clock_enable(phy_node, "ref");
	clock_enable(phy_node, "pclk");
	clock_enable(phy_node, "pclk_host");
	reset_deassert(phy_node, "apb");

	/* parse data-mapping (vesa-24, jeida-18, jeida-24) */
	memset(mapping, 0, sizeof(mapping));
	if (OF_getprop(sc->sc_node, "rockchip,data-mapping", mapping,
	    sizeof(mapping)) <= 0)
		OF_getprop(sc->sc_node, "data-mapping", mapping,
		    sizeof(mapping));

	if (strcmp(mapping, "jeida-18") == 0) {
		sc->sc_format = LVDS_FORMAT_JEIDA_18;
		sc->sc_data_width = 18;
	} else if (strcmp(mapping, "jeida-24") == 0 ||
	    strcmp(mapping, "jeida") == 0) {
		sc->sc_format = LVDS_FORMAT_JEIDA_24;
		sc->sc_data_width = 24;
	} else if (strcmp(mapping, "vesa-24") == 0) {
		sc->sc_format = LVDS_FORMAT_VESA_24;
		sc->sc_data_width = 24;
	} else {
		/* default to JEIDA 24-bit for RK3126/RK3128 */
		sc->sc_format = LVDS_FORMAT_JEIDA_24;
		sc->sc_data_width = 24;
	}

	/* override data-width if explicitly specified (check both variants) */
	sc->sc_data_width = OF_getpropint(sc->sc_node, "rockchip,data-width",
	    OF_getpropint(sc->sc_node, "data-width", sc->sc_data_width));

	/* adjust format based on actual data width */
	if (sc->sc_data_width == 18 && sc->sc_format == LVDS_FORMAT_JEIDA_24)
		sc->sc_format = LVDS_FORMAT_JEIDA_18;

	printf(": LVDS %d-bit %s\n", sc->sc_data_width,
	    sc->sc_format == LVDS_FORMAT_JEIDA_24 ? "JEIDA" :
	    sc->sc_format == LVDS_FORMAT_JEIDA_18 ? "JEIDA-18" : "VESA");

	sc->sc_ports.dp_node = faa->fa_node;
	sc->sc_ports.dp_cookie = sc;
	sc->sc_ports.dp_ep_activate = rklvds_ep_activate;
	sc->sc_ports.dp_ep_get_cookie = rklvds_ep_get_cookie;
	device_ports_register(&sc->sc_ports, EP_DRM_ENCODER);
}

int
rklvds_activate(struct device *self, int act)
{
	struct rklvds_softc *sc = (struct rklvds_softc *)self;
	int node = sc->sc_phy_node;

	if (node == 0)
		return 0;

	switch (act) {
	case DVACT_SUSPEND:
		clock_disable(node, "pclk_host");
		clock_disable(node, "pclk");
		clock_disable(node, "ref");
		power_domain_disable(node);
		break;
	case DVACT_RESUME:
		power_domain_enable(node);
		clock_enable(node, "ref");
		clock_enable(node, "pclk");
		clock_enable(node, "pclk_host");
		reset_deassert(node, "apb");
		break;
	}

	return 0;
}

void
rklvds_phy_power_on(struct rklvds_softc *sc)
{
	uint32_t val;
	/* PLL dividers: prediv=2, fbdiv=28: 24MHz / 2 * 28 = 336 MHz */
	uint16_t fbdiv = 28;
	uint8_t prediv = 2;

	if (sc->sc_phy == NULL) {
		sc->sc_phy = regmap_byphandle(sc->sc_phy_phandle);
		if (sc->sc_phy == NULL) {
			printf("%s: can't find phy regmap\n",
			    sc->sc_dev.dv_xname);
			return;
		}
	}

	/* configure GRF for LVDS */
	val = LVDS_CON0_WRITE_MASK;	/* enable writes to all bits */
	val |= LVDS_CON0_FORCEX_EN;	/* force TX enable */
	val |= LVDS_CON0_LANE0_EN;	/* enable lane 0 */
	val |= LVDS_CON0_MODE_EN;	/* enable LVDS mode */
	val |= LVDS_CON0_MSBSEL;	/* MSB select = D7 */
	val |= (sc->sc_format << LVDS_CON0_FORMAT_SHIFT);
	regmap_write_4(sc->sc_grf, GRF_LVDS_CON0, val);

	/* PHY power-on sequence */

	/* disable digital internal first */
	val = PHY_READ(sc, MIPIPHY_REGE1);
	val &= ~LVDS_DIG_INTERNAL_ENABLE;
	PHY_WRITE(sc, MIPIPHY_REGE1, val);

	/* set PLL prediv and fbdiv */
	PHY_WRITE(sc, MIPIPHY_REG3, REG_PREDIV(prediv) | REG_FBDIV_HI(fbdiv));
	PHY_WRITE(sc, MIPIPHY_REG4, REG_FBDIV_LO(fbdiv));

	/* XXX: magic value from Rockchip U-Boot */
	PHY_WRITE(sc, MIPIPHY_REGE8, 0xfc);

	/* set MSB_SEL and DIG_INTER_RST in REGE0 */
	val = PHY_READ(sc, MIPIPHY_REGE0);
	val |= (1 << 0);		/* MSB_SEL */
	val |= LVDS_DIG_RESET_DISABLE;	/* DIG_INTER_RST (bit 2) */
	PHY_WRITE(sc, MIPIPHY_REGE0, val);

	/* power up PLL and LDO (clear SYNC_RST, LDO_PWR_DOWN, PLL_PWR_DOWN) */
	val = PHY_READ(sc, MIPIPHY_REG1);
	val &= ~(REG_DA_SYNCRST | REG_DA_LDOPD | REG_DA_PLLPD);
	PHY_WRITE(sc, MIPIPHY_REG1, val);

	/* enable analog lanes and bandgap */
	PHY_WRITE(sc, MIPIPHY_REG0,
	    LANE_EN_CK | LANE_EN_3 | LANE_EN_2 | LANE_EN_1 | LANE_EN_0 |
	    POWER_WORK_ENABLE);

	/* enable lanes, power PLL (clear PLL_PWR_OFF and BANDGAP_PWR_DOWN) */
	PHY_WRITE(sc, MIPIPHY_REGEB,
	    REG_LANE0_EN | REG_LANE1_EN | REG_LANE2_EN | REG_LANE3_EN |
	    REG_LANECLK_EN);

	/* enable LVDS mode (disable MIPI and TTL) */
	val = PHY_READ(sc, MIPIPHY_REGE3);
	val &= ~(REG_MIPI_EN | REG_TTL_EN);
	val |= REG_LVDS_EN;
	PHY_WRITE(sc, MIPIPHY_REGE3, val);

	/* wait for PLL to stabilize */
	delay(2000);

	/* enable digital internal */
	val = PHY_READ(sc, MIPIPHY_REGE1);
	val |= LVDS_DIG_INTERNAL_ENABLE;
	PHY_WRITE(sc, MIPIPHY_REGE1, val);
}

void
rklvds_phy_power_off(struct rklvds_softc *sc)
{
	uint32_t val;

	if (sc->sc_phy == NULL)
		return;

	/* disable LVDS lanes in analog part */
	val = PHY_READ(sc, MIPIPHY_REG0);
	val &= ~(LANE_EN_CK | LANE_EN_3 | LANE_EN_2 | LANE_EN_1 | LANE_EN_0);
	PHY_WRITE(sc, MIPIPHY_REG0, val);

	/* power down LDO and PLL */
	val = PHY_READ(sc, MIPIPHY_REG1);
	val |= REG_DA_LDOPD | REG_DA_PLLPD;
	PHY_WRITE(sc, MIPIPHY_REG1, val);

	/* disable power work */
	val = PHY_READ(sc, MIPIPHY_REG0);
	val &= ~0x03;
	val |= POWER_WORK_DISABLE;
	PHY_WRITE(sc, MIPIPHY_REG0, val);

	/* power down bandgap */
	val |= BANDGAP_POWER_DOWN;
	PHY_WRITE(sc, MIPIPHY_REG0, val);

	/* disable LVDS lanes */
	val = PHY_READ(sc, MIPIPHY_REGEB);
	val &= ~(REG_LANE0_EN | REG_LANE1_EN | REG_LANE2_EN | REG_LANE3_EN |
	    REG_LANECLK_EN);
	PHY_WRITE(sc, MIPIPHY_REGEB, val);

	/* disable LVDS digital logic */
	val = PHY_READ(sc, MIPIPHY_REGE1);
	val &= ~LVDS_DIG_INTERNAL_ENABLE;
	PHY_WRITE(sc, MIPIPHY_REGE1, val);

	/* power off PLL and bandgap in LVDS part */
	val = PHY_READ(sc, MIPIPHY_REGEB);
	val |= REG_PLL_POWER_OFF | REG_BANDGAP_POWER_DOWN;
	PHY_WRITE(sc, MIPIPHY_REGEB, val);

	/* disable LVDS mode in GRF */
	regmap_write_4(sc->sc_grf, GRF_LVDS_CON0, LVDS_CON0_WRITE_MASK);
}

/*
 * DRM encoder functions
 */
void
rklvds_encoder_enable(struct drm_encoder *encoder)
{
	struct rklvds_softc *sc = to_rklvds_encoder(encoder);

	rklvds_phy_power_on(sc);

	if (sc->sc_panel) {
		drm_panel_prepare(sc->sc_panel);
		drm_panel_enable(sc->sc_panel);
	}
}

void
rklvds_encoder_disable(struct drm_encoder *encoder)
{
	struct rklvds_softc *sc = to_rklvds_encoder(encoder);

	if (sc->sc_panel) {
		drm_panel_disable(sc->sc_panel);
		drm_panel_unprepare(sc->sc_panel);
	}

	rklvds_phy_power_off(sc);
}

/*
 * DRM connector functions
 */
enum drm_connector_status
rklvds_connector_detect(struct drm_connector *connector, bool force)
{
	/* LVDS panels are always connected */
	return connector_status_connected;
}

void
rklvds_connector_destroy(struct drm_connector *connector)
{
	drm_connector_cleanup(connector);
}

int
rklvds_connector_get_modes(struct drm_connector *connector)
{
	struct rklvds_connector *rk_connector = to_rklvds_connector(connector);
	struct rklvds_softc *sc = rk_connector->sc;

	if (sc->sc_panel)
		return drm_panel_get_modes(sc->sc_panel, connector);

	return 0;
}

/*
 * Endpoint activation - called when VOP connects
 */
int
rklvds_ep_activate(void *cookie, struct endpoint *ep, void *arg)
{
	struct rklvds_softc *sc = cookie;
	struct drm_device *ddev = arg;
	struct drm_crtc *crtc = NULL;
	struct endpoint *rep;
	struct endpoint *panel_ep, *panel_rep;
	int error;

	if (sc->sc_activated)
		return 0;

	/* get CRTC from remote endpoint (VOP) */
	rep = endpoint_remote(ep);
	if (rep && rep->ep_type == EP_DRM_CRTC)
		crtc = endpoint_get_cookie(rep);
	if (crtc == NULL)
		return EINVAL;

	/* find panel on port@1 */
	panel_ep = endpoint_byreg(&sc->sc_ports, 1, 0);
	if (panel_ep == NULL)
		panel_ep = endpoint_byreg(&sc->sc_ports, 1, -1);
	if (panel_ep != NULL) {
		panel_rep = endpoint_remote(panel_ep);
		if (panel_rep && panel_rep->ep_type == EP_DRM_PANEL)
			sc->sc_panel = endpoint_get_cookie(panel_rep);
	}

	if (sc->sc_panel == NULL) {
		printf("%s: no panel found\n", sc->sc_dev.dv_xname);
		return ENODEV;
	}

	/* initialize encoder */
	sc->sc_encoder.possible_crtcs = drm_crtc_mask(crtc);
	drm_encoder_init(ddev, &sc->sc_encoder, &rklvds_encoder_funcs,
	    DRM_MODE_ENCODER_LVDS, NULL);
	drm_encoder_helper_add(&sc->sc_encoder, &rklvds_encoder_helper_funcs);

	/* initialize connector */
	sc->sc_connector.sc = sc;
	drm_connector_init(ddev, &sc->sc_connector.base,
	    &rklvds_connector_funcs, DRM_MODE_CONNECTOR_LVDS);
	drm_connector_helper_add(&sc->sc_connector.base,
	    &rklvds_connector_helper_funcs);

	/* attach connector to encoder */
	error = drm_connector_attach_encoder(&sc->sc_connector.base,
	    &sc->sc_encoder);
	if (error) {
		printf("%s: failed to attach connector: %d\n",
		    sc->sc_dev.dv_xname, error);
		return error;
	}

	sc->sc_activated = 1;

	return 0;
}

void *
rklvds_ep_get_cookie(void *cookie, struct endpoint *ep)
{
	struct rklvds_softc *sc = cookie;

	return &sc->sc_encoder;
}
