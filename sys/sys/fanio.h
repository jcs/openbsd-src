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
	unsigned int idx;
	char id[32];
	unsigned int rpm_actual;
	unsigned int rpm_min;
	unsigned int rpm_max;
	unsigned int rpm_safe;
	unsigned int rpm_target;
};

struct fan_set_rpm {
	int idx;
	int rpm;
};

#define FANIOC_QUERY_DRV	 _IOR('V', 0, struct fan_query_drv)
#define FANIOC_QUERY_FAN	 _IOR('V', 1, struct fan_query_fan)
#define FANIOC_SET_MIN		_IOWR('V', 2, struct fan_set_rpm)
#define FANIOC_SET_MAX		_IOWR('V', 3, struct fan_set_rpm)
#define FANIOC_SET_TARGET	_IOWR('V', 4, struct fan_set_rpm)

#endif /* _SYS_FANIO_H */
