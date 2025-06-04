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
 * Rockchip Innosilicon MIPI/LVDS D-PHY
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_misc.h>
#include <dev/ofw/fdt.h>

struct rklvdsphy_softc {
	struct device		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
};

int	rklvdsphy_match(struct device *, void *, void *);
void	rklvdsphy_attach(struct device *, struct device *, void *);

const struct cfattach rklvdsphy_ca = {
	sizeof(struct rklvdsphy_softc), rklvdsphy_match, rklvdsphy_attach
};

struct cfdriver rklvdsphy_cd = {
	NULL, "rklvdsphy", DV_DULL
};

int
rklvdsphy_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return OF_is_compatible(faa->fa_node, "rockchip,rk3128-dsi-dphy");
}

void
rklvdsphy_attach(struct device *parent, struct device *self, void *aux)
{
	struct rklvdsphy_softc *sc = (struct rklvdsphy_softc *)self;
	struct fdt_attach_args *faa = aux;
	int idx;

	if (faa->fa_nreg < 1) {
		printf(": no registers\n");
		return;
	}

	sc->sc_iot = faa->fa_iot;

	idx = OF_getindex(faa->fa_node, "dphy", "reg-names");
	if (idx < 0)
		idx = 0;
	if (idx >= faa->fa_nreg) {
		printf(": dphy reg index out of range\n");
		return;
	}

	if (bus_space_map(sc->sc_iot, faa->fa_reg[idx].addr,
	    faa->fa_reg[idx].size, 0, &sc->sc_ioh)) {
		printf(": can't map registers\n");
		return;
	}

	printf("\n");

	regmap_register(faa->fa_node, sc->sc_iot, sc->sc_ioh,
	    faa->fa_reg[idx].size);
}
