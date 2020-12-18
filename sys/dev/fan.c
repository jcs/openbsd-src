/*	$OpenBSD$ */

/*
 * Copyright (c) 2020 Marcus Glocker <mglocker@openbsd.org>
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
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/poll.h>
#include <sys/device.h>
#include <sys/vnode.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/conf.h>
#include <sys/fanio.h>
#include <dev/fan_if.h>

struct fan_softc {
	struct device		 dev;
	void			*hw_hdl;
	struct fan_hw_if	*hw_if;		/* driver functions */
#define FAN_OPEN 0x1
	uint8_t			 sc_open;
};

int	fan_match(struct device *, void *, void *);
void	fan_attach(struct device*, struct device *, void *);
int	fan_detach(struct device *, int);
int	fan_activate(struct device *, int);
int	fan_print(void *, const char *);

const struct cfattach fan_ca = {
	sizeof(struct fan_softc), fan_match, fan_attach, fan_detach,
	fan_activate
};

struct cfdriver fan_cd = {
	NULL, "fan", DV_DULL
};

int
fan_match(struct device *parent, void *match, void *aux)
{
	return 1;
}

void
fan_attach(struct device *parent, struct device *self, void *aux)
{
	struct fan_softc *sc = (void *)self;
	struct fan_attach_args *sa = aux;

	printf("\n");
	sc->hw_if = sa->hwif;
	sc->hw_hdl = sa->hdl;
}

int
fan_detach(struct device *self, int flags)
{
	return 0;
}

int
fan_activate(struct device *self, int act)
{
        //struct fan_softc *sc = (struct fan_softc *)self;

        return 0;
}

int
fan_submatch(struct device *parent, void *match, void *aux)
{
	struct cfdata *cf = match;

	return (cf->cf_driver == &fan_cd);
}

struct device *
fan_attach_mi(struct fan_hw_if *rhwp, void *hdlp, struct device *dev)
{
	struct fan_attach_args arg;

	arg.hwif = rhwp;
	arg.hdl = hdlp;
	return config_found_sm(dev, &arg, fan_print, fan_submatch);
}

int
fan_print(void *aux, const char *pnp)
{
	if (pnp != NULL)
		printf("fan at %s", pnp);

	return UNCONF;
}

int
fanopen(dev_t dev, int flags, int fmt, struct proc *p)
{
	struct fan_softc *sc;
	int unit;

	printf("fanopen\n");

	unit = FAN_UNIT(dev);
	sc = fan_cd.cd_devs[unit];
	if (unit >= fan_cd.cd_ndevs || sc == NULL || sc->hw_if == NULL)
		return (ENXIO);

	if (sc->sc_open & FAN_OPEN)
		return EBUSY;

	sc->sc_open |= FAN_OPEN;

	if (sc->hw_if->open != NULL)
		return (sc->hw_if->open(sc->hw_hdl));

	return 0;
}

int
fanclose(dev_t dev, int flags, int fmt, struct proc *p)
{
	struct fan_softc *sc;
	int unit, r;

	printf("fanclose\n");

	unit = FAN_UNIT(dev);
	sc = fan_cd.cd_devs[unit];
	if (sc == NULL)
		return ENXIO;

	if (sc->hw_if->close != NULL)
		r = sc->hw_if->close(sc->hw_hdl);

	sc->sc_open &= ~FAN_OPEN;

	return r;
}

int
fanioctl(dev_t dev, u_long cmd, caddr_t data, int flags, struct proc *p)
{
	struct fan_softc *sc;
	int unit, error;

	printf("fanioctl\n");

	unit = FAN_UNIT(dev);
	sc = fan_cd.cd_devs[unit];
	if (sc == NULL)
		return ENXIO;

	error = EOPNOTSUPP;
	switch (cmd) {
	case FANIOC_QUERY_DRV:
		if (sc->hw_if->query_drv)
			error = sc->hw_if->query_drv(sc->hw_hdl,
			    (struct fan_query_drv *)data);
		break;
	case FANIOC_QUERY_FAN:
		if (sc->hw_if->query_fan)
			error = sc->hw_if->query_fan(sc->hw_hdl,
			    (struct fan_query_fan *)data);
		break;
	case FANIOC_G_ACT:
		if (sc->hw_if->g_act)
			error = sc->hw_if->g_act(sc->hw_hdl,
			    (struct fan_g_act *)data);
		break;
	case FANIOC_G_MIN:
		if (sc->hw_if->g_min)
			error = sc->hw_if->g_min(sc->hw_hdl,
			    (struct fan_g_min *)data);
		break;
	case FANIOC_G_MAX:
		if (sc->hw_if->g_max)
			error = sc->hw_if->g_max(sc->hw_hdl,
			    (struct fan_g_max *)data);
		break;
	case FANIOC_G_SAF:
		if (sc->hw_if->g_saf)
			error = sc->hw_if->g_saf(sc->hw_hdl,
			    (struct fan_g_saf *)data);
		break;
	case FANIOC_G_TGT:
		if (sc->hw_if->g_tgt)
			error = sc->hw_if->g_tgt(sc->hw_hdl,
			    (struct fan_g_tgt *)data);
		break;
	case FANIOC_S_MIN:
		if (sc->hw_if->s_min)
			error = sc->hw_if->s_min(sc->hw_hdl,
			    (struct fan_s_min *)data);
		break;
	case FANIOC_S_MAX:
		if (sc->hw_if->s_max)
			error = sc->hw_if->s_max(sc->hw_hdl,
			    (struct fan_s_max *)data);
		break;
	case FANIOC_S_TGT:
		if (sc->hw_if->s_tgt)
			error = sc->hw_if->s_tgt(sc->hw_hdl,
			    (struct fan_s_tgt *)data);
		break;
	default:
		error = ENOTTY;
	}

	return error;
}
