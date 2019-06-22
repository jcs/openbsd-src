/* $OpenBSD$ */
/*
 * Intel Low Power Subsystem
 *
 * Copyright (c) 2015-2017 joshua stein <jcs@openbsd.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
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

/* 13.3: I2C Additional Registers Summary */
#define LPSS_RESETS		0x204
#define  LPSS_RESETS_FUNC	(1 << 0) | (1 << 1)
#define  LPSS_RESETS_IDMA	(1 << 2)
#define LPSS_ACTIVELTR		0x210
#define LPSS_IDLELTR		0x214
#define LPSS_CAPS		0x2fc
#define  LPSS_CAPS_NO_IDMA	(1 << 8)
#define  LPSS_CAPS_TYPE_SHIFT	4
#define  LPSS_CAPS_TYPE_MASK	(0xf << LPSS_CAPS_TYPE_SHIFT)
#define  LPSS_CAPS_TYPE_I2C	0
#define  LPSS_CAPS_TYPE_UART	1
#define  LPSS_CAPS_TYPE_SPI	2
#define  LPSS_CAPS_CS_EN_SHIFT	9
#define  LPSS_CAPS_CS_EN_MASK	(0xf << LPSS_CAPS_CS_EN_SHIFT)
#define LPSS_CS_CONTROL_SW_MODE	(1 << 0)
#define LPSS_CS_CONTROL_CS_HIGH	(1 << 1)
