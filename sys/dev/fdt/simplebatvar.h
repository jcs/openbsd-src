/* $OpenBSD$ */
/*
 * Copyright (c) 2026 joshua stein <jcs@jcs.org>
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

#ifndef _SIMPLEBATVAR_H_
#define _SIMPLEBATVAR_H_

struct simplebat_attach_args {
	int	sa_node;
	void	*sa_cookie;
	int	(*sa_voltage)(void *);	/* returns mV */
	int	(*sa_current)(void *);	/* returns mA, signed */
	int	(*sa_status)(void *);	/* returns state defined below */
	int	(*sa_present)(void *);	/* returns 1 if present */
};

#define SIMPLEBAT_IDLE			0
#define SIMPLEBAT_DEAD_CHARGE		1
#define SIMPLEBAT_TRICKLE_CHARGE	2
#define SIMPLEBAT_CHARGING		3
#define SIMPLEBAT_CHARGE_DONE		4
#define SIMPLEBAT_DISCHARGING		5
#define SIMPLEBAT_FAULT			6
#define SIMPLEBAT_UNKNOWN		7

#endif
