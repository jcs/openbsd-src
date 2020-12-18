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

#ifndef _SYS_DEV_FAN_IF_H
#define _SYS_DEV_FAN_IF_H

#define FAN_UNIT(x) (minor(x))

struct fan_hw_if {
	/* open */
	int (*open)(void *);

	/* close */
	int (*close)(void *);

	/* ioctl */
	int (*query_drv)(void *, struct fan_query_drv *);
	int (*query_fan)(void *, struct fan_query_fan *);
	int (*g_act)(void *, struct fan_g_act *);
	int (*g_min)(void *, struct fan_g_min *);
	int (*g_max)(void *, struct fan_g_max *);
	int (*g_saf)(void *, struct fan_g_saf *);
	int (*g_tgt)(void *, struct fan_g_tgt *);
	int (*s_min)(void *, struct fan_s_min *);
	int (*s_max)(void *, struct fan_s_max *);
	int (*s_tgt)(void *, struct fan_s_tgt *);
};

struct fan_attach_args {
	void *hwif;
	void *hdl;
};

struct device *fan_attach_mi(struct fan_hw_if *, void *, struct device *);

#endif /* _SYS_DEV_FAN_IF_H */
