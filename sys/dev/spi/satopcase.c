/* $OpenBSD$ */
/*
 * Apple SPI "topcase" driver
 *
 * Copyright (c) 2015-2018 joshua stein <jcs@openbsd.org>
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
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/stdint.h>

#include <dev/spi/spivar.h>

/* #define SATOPCASE_DEBUG */

#define SATOPCASE_DEBUG
#ifdef SATOPCASE_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

struct satopcase_softc {
	struct device	sc_dev;
	spi_tag_t	sc_tag;
	void		*sc_ih;
};

int	satopcase_match(struct device *, void *, void *);
void	satopcase_attach(struct device *, struct device *, void *);
int	satopcase_detach(struct device *, int);
int	satopcase_intr(void *);

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

int
satopcase_match(struct device *parent, void *match, void *aux)
{
	struct spi_attach_args *sa = aux;

	if (strcmp(sa->sa_name, "satopcase") == 0)
		return (1);

	return (0);
}

void
satopcase_attach(struct device *parent, struct device *self, void *aux)
{
	struct satopcase_softc *sc = (struct satopcase_softc *)self;
	struct spi_attach_args *sa = aux;

	sc->sc_tag = sa->sa_tag;

	if (sa->sa_intr) {
		printf(" %s", spi_intr_string(sc->sc_tag, sa->sa_intr));

		sc->sc_ih = spi_intr_establish(sc->sc_tag, sa->sa_intr,
		    IPL_TTY, satopcase_intr, sc, sc->sc_dev.dv_xname);
		if (sc->sc_ih == NULL)
			printf(", can't establish interrupt");
	}

	printf("\n");
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

	printf("%s: %s!\n", sc->sc_dev.dv_xname, __func__);

	return (1);
}
