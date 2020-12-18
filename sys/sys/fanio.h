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

#ifndef _SYS_FANIO_H_
#define _SYS_FANIO_H_

struct fan_query_drv {
	char id[32];
	int nfans;
};

struct fan_query_fan {
	int idx;
	char id[32];
	int rpm_act;
	int rpm_min;
	int rpm_max;
	int rpm_saf;
	int rpm_tgt;
};

struct fan_g_act {
	int idx;
	int rpm;
};

struct fan_g_min {
	int idx;
	int rpm;
};

struct fan_g_max {
	int idx;
	int rpm;
};

struct fan_g_saf {
	int idx;
	int rpm;
};

struct fan_g_tgt {
	int idx;
	int rpm;
};

struct fan_s_min {
	int idx;
	int rpm;
};

struct fan_s_max {
	int idx;
	int rpm;
};

struct fan_s_tgt {
	int idx;
	int rpm;
};

#define FANIOC_QUERY_DRV	 _IOR('V', 0, struct fan_query_drv)
#define FANIOC_QUERY_FAN	_IOWR('V', 1, struct fan_query_fan)
#define FANIOC_G_ACT		_IOWR('V', 2, struct fan_g_act)
#define FANIOC_G_MIN		_IOWR('V', 3, struct fan_g_min)
#define FANIOC_G_MAX		_IOWR('V', 4, struct fan_g_max)
#define FANIOC_G_SAF		_IOWR('V', 5, struct fan_g_saf)
#define FANIOC_G_TGT		_IOWR('V', 6, struct fan_g_tgt)
#define FANIOC_S_MIN		_IOWR('V', 7, struct fan_s_min)
#define FANIOC_S_MAX		_IOWR('V', 8, struct fan_s_max)
#define FANIOC_S_TGT		_IOWR('V', 9, struct fan_s_tgt)

#endif /* _SYS_FANIO_H */
