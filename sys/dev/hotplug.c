/*	$OpenBSD: hotplug.c,v 1.16 2016/06/07 01:31:54 tedu Exp $	*/
/*
 * Copyright (c) 2018 joshua stein <jcs@openbsd.org>
 * Copyright (c) 2004 Alexander Yurchenko <grange@openbsd.org>
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
 * Device attachment and detachment notifications.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/fcntl.h>
#include <sys/hotplug.h>
#include <sys/ioctl.h>
#include <sys/malloc.h>
#include <sys/poll.h>
#include <sys/vnode.h>

#define HOTPLUG_MAXEVENTS	64

struct hotplug_d {
	int hd_unit;
	int evqueue_head;
	struct selinfo hotplug_sel;
	LIST_ENTRY(hotplug_d) hd_list;
};

LIST_HEAD(, hotplug_d) hotplug_d_list;
struct hotplug_d *hotplug_lookup(int);

static struct hotplug_event evqueue[HOTPLUG_MAXEVENTS];
static int evqueue_head, evqueue_count;

void filt_hotplugrdetach(struct knote *);
int  filt_hotplugread(struct knote *, long);

struct filterops hotplugread_filtops =
	{ 1, NULL, filt_hotplugrdetach, filt_hotplugread};

#define EVQUEUE_NEXT(p) (p == HOTPLUG_MAXEVENTS - 1 ? 0 : p + 1)

void hotplug_put_event(struct hotplug_event *);
int  hotplug_get_event(struct hotplug_d *hd, struct hotplug_event *, int);

void hotplugattach(int);

void
hotplugattach(int count)
{
	evqueue_head = 0;
	evqueue_count = 0;

	LIST_INIT(&hotplug_d_list);
}

void
hotplug_device_attach(enum devclass class, char *name)
{
	struct hotplug_event he;

	he.he_type = HOTPLUG_DEVAT;
	he.he_devclass = class;
	strlcpy(he.he_devname, name, sizeof(he.he_devname));
	hotplug_put_event(&he);
}

void
hotplug_device_detach(enum devclass class, char *name)
{
	struct hotplug_event he;

	he.he_type = HOTPLUG_DEVDT;
	he.he_devclass = class;
	strlcpy(he.he_devname, name, sizeof(he.he_devname));
	hotplug_put_event(&he);
}

void
hotplug_put_event(struct hotplug_event *he)
{
	struct hotplug_d *hd;

	evqueue[evqueue_head] = *he;
	evqueue_head = EVQUEUE_NEXT(evqueue_head);
	if (evqueue_count < HOTPLUG_MAXEVENTS)
		evqueue_count++;

	/*
	 * If any readers are still at the new evqueue_head, they are about to
	 * get lapped.
	 */
	LIST_FOREACH(hd, &hotplug_d_list, hd_list) {
		if (hd->evqueue_head == evqueue_head)
			hd->evqueue_head = EVQUEUE_NEXT(evqueue_head);
		selwakeup(&hd->hotplug_sel);
	}

	wakeup(&evqueue);
}

int
hotplug_get_event(struct hotplug_d *hd, struct hotplug_event *he, int peek)
{
	int s;

	if (evqueue_count == 0 || hd->evqueue_head == evqueue_count ||
	    hd->evqueue_head == evqueue_head)
		return (1);

	s = splbio();
	*he = evqueue[hd->evqueue_head];
	if (!peek)
		hd->evqueue_head = EVQUEUE_NEXT(hd->evqueue_head);
	splx(s);
	return (0);
}

int
hotplugopen(dev_t dev, int flag, int mode, struct proc *p)
{
	struct hotplug_d *hd;
	int unit = minor(dev);

	if (flag & FWRITE)
		return (EPERM);

	if ((hd = hotplug_lookup(unit)) != NULL)
		return (EBUSY);

	hd = malloc(sizeof(*hd), M_DEVBUF, M_WAITOK|M_ZERO);
	hd->hd_unit = unit;

	/*
	 * Set head as far back as possible so each reader gets all historical
	 * events.
	 */
	if (evqueue_count < HOTPLUG_MAXEVENTS)
		hd->evqueue_head = 0;
	else
		hd->evqueue_head = EVQUEUE_NEXT(evqueue_head);

	LIST_INSERT_HEAD(&hotplug_d_list, hd, hd_list);

	return (0);
}

struct hotplug_d *
hotplug_lookup(int unit)
{
	struct hotplug_d *hd;

	LIST_FOREACH(hd, &hotplug_d_list, hd_list)
		if (hd->hd_unit == unit)
			return (hd);
	return (NULL);
}

int
hotplugclose(dev_t dev, int flag, int mode, struct proc *p)
{
	struct hotplug_d *hd;

	hd = hotplug_lookup(minor(dev));
	if (hd == NULL)
		return (EINVAL);

	LIST_REMOVE(hd, hd_list);
	free(hd, M_DEVBUF, sizeof(*hd));

	return (0);
}

int
hotplugread(dev_t dev, struct uio *uio, int flags)
{
	struct hotplug_d *hd;
	struct hotplug_event he;
	int error;

	hd = hotplug_lookup(minor(dev));
	if (hd == NULL)
		return (ENXIO);

	if (uio->uio_resid != sizeof(he))
		return (EINVAL);

again:
	if (hotplug_get_event(hd, &he, 0) == 0)
		return (uiomove(&he, sizeof(he), uio));
	if (flags & IO_NDELAY)
		return (EAGAIN);

	error = tsleep(&evqueue, PRIBIO | PCATCH, "htplev", 0);
	if (error)
		return (error);
	goto again;
}

int
hotplugioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	struct hotplug_d *hd;

	hd = hotplug_lookup(minor(dev));
	if (hd == NULL)
		return (ENXIO);

	switch (cmd) {
	case FIOASYNC:
		/* ignore */
	case FIONBIO:
		/* handled in the upper fs layer */
		break;
	default:
		return (ENOTTY);
	}

	return (0);
}

int
hotplugpoll(dev_t dev, int events, struct proc *p)
{
	struct hotplug_d *hd;
	struct hotplug_event he;
	int revents = 0;

	hd = hotplug_lookup(minor(dev));
	if (hd == NULL)
		return (POLLERR);

	if (events & (POLLIN | POLLRDNORM)) {
		if (hotplug_get_event(hd, &he, 1) == 0)
			revents |= events & (POLLIN | POLLRDNORM);
		else
			selrecord(p, &hd->hotplug_sel);
	}

	return (revents);
}

int
hotplugkqfilter(dev_t dev, struct knote *kn)
{
	struct hotplug_d *hd;
	struct klist *klist;
	int s;

	hd = hotplug_lookup(minor(dev));
	if (hd == NULL)
		return (EINVAL);

	switch (kn->kn_filter) {
	case EVFILT_READ:
		klist = &hd->hotplug_sel.si_note;
		kn->kn_fop = &hotplugread_filtops;
		break;
	default:
		return (EINVAL);
	}

	kn->kn_hook = hd;

	s = splbio();
	SLIST_INSERT_HEAD(klist, kn, kn_selnext);
	splx(s);
	return (0);
}

void
filt_hotplugrdetach(struct knote *kn)
{
	struct hotplug_d *hd = kn->kn_hook;
	int s;

	s = splbio();
	SLIST_REMOVE(&hd->hotplug_sel.si_note, kn, knote, kn_selnext);
	splx(s);
}

int
filt_hotplugread(struct knote *kn, long hint)
{
	struct hotplug_d *hd = kn->kn_hook;
	struct hotplug_event he;

	if (hotplug_get_event(hd, &he, 1) != 0)
		return (0);

	return (1);
}
