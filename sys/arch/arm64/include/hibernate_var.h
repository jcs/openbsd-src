/*	$OpenBSD: hibernate_var.h,v 1.1 2026/09/06 18:25:22 mglocker Exp $ */

/*
 * Copyright (c) 2011 Mike Larkin <mlarkin@openbsd.org>
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

#define PIGLET_PAGE_MASK	(~((1ULL << 21) - 1))

/*
 * L0 table for resume; L1/L2/L3 low+high tables backing it.  arm64 uses
 * TTBR0 for the low-VA identity map built during unpack.
 */
#define HIBERNATE_L0_PAGE	(PAGE_SIZE * 21)
#define HIBERNATE_L1_LOW	(PAGE_SIZE * 22)
#define HIBERNATE_L1_HI		(PAGE_SIZE * 23)
#define HIBERNATE_L2_LOW	(PAGE_SIZE * 24)
#define HIBERNATE_L3_LOW	(PAGE_SIZE * 25)
#define HIBERNATE_PT1_L1_PAGE	(PAGE_SIZE * 26)

#define HIBERNATE_INFLATE_PAGE	(PAGE_SIZE * 33)

/* Pool of L2 pages per distinct L1_LOW / L1_HI slot */
#define HIBERNATE_L2_LOW_POOL		(PAGE_SIZE * 200)
#define HIBERNATE_L2_LOW_POOL_COUNT	16
#define HIBERNATE_L2_HI_POOL		(PAGE_SIZE * 216)
#define HIBERNATE_L2_HI_POOL_COUNT	16
#define HIBERNATE_PT1_L2_POOL		(PAGE_SIZE * 232)
#define HIBERNATE_PT1_L2_POOL_COUNT	16

/* 3 pages for stack */
#define HIBERNATE_STACK_PAGE	(PAGE_SIZE * 380)

/*
 * HIBERNATE_HIBALLOC_PAGE must be the last stolen page (see machdep.c).
 * On arm64 a HIGH VA inside the piglet (kernel pmap only manages TTBR1,
 * no low-VA pmap_kenter_pa() like amd64).
 */
#ifndef _LOCORE
extern vaddr_t global_piglet_va;
#endif
#define HIBERNATE_HIBALLOC_PAGE	(global_piglet_va + PAGE_SIZE * 366)

/* Use 4MB hibernation chunks */
#define HIBERNATE_CHUNK_SIZE		0x400000

#define HIBERNATE_CHUNK_TABLE_SIZE	0x200000

#define HIBERNATE_STACK_OFFSET		0x0F00

/*
 * Minimum amount of memory for hibernate support. This is used in early boot
 * when deciding if we can preallocate the piglet. If the machine does not
 * have at least HIBERNATE_MIN_MEMORY RAM, we won't support hibernate. This
 * avoids late allocation issues due to fragmented memory and failure to
 * hibernate. We need to be able to allocate 32MB contiguous memory, aligned
 * to 2MB.
 *
 * The default minimum required memory is 512MB (1ULL << 29).
 */
#define HIBERNATE_MIN_MEMORY	(1ULL << 29)
