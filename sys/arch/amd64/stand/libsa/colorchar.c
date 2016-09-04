/* $OpenBSD$ */

/*
 * Copyright (c) 2016 joshua stein <jcs@openbsd.org>
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

#include <lib/libsa/colorchar.h>

#include "libsa.h"

void
colorchar(int c, int bg, int fg)
{
	register int rv;
	int row, col;

	/* find cursor */
	__asm volatile(DOINT(0x10)
		: "=d" (rv)
		: "a" (0x0300),
		  "b" (0)
		: "%ecx", "cc");

	row = (rv >> 8) & 0xff;
	col = (rv & 0xff);

	/* write character with bg/fg attributes */
	__asm volatile(DOINT(0x10)
		:
		: "a" (0x0900 + (0x00ff & c)),
		  "b" ((bg << 4) + fg),
		  "c" (1)
		: "%edx", "cc");

	/* manually advance cursor position */
	col++;
	if (col > 79) {
		col = 0;
		row++;
	}
	__asm volatile(DOINT(0x10)
		:
		: "a" (0x0200),
		  "b" (0),
		  "d" ((row << 8) + col)
		: "%ecx", "cc");
}
