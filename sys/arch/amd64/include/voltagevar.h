/* $OpenBSD$ */

/*
 * Copyright (c) 2018 joshua stein <jcs@openbsd.org>
 * All rights reserved.
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
 * CTL_VOLTAGE definitions.
 */
#define	VOLTAGE_CPU		1	/* int: CPU undervolt */
#define	VOLTAGE_CPU_NAME	"cpu"
#define	VOLTAGE_GPU		2	/* int: GPU undervolt */
#define	VOLTAGE_GPU_NAME	"gpu"
#define VOLTAGE_CPU_CACHE	3	/* int: CPU Cache undervolt */
#define VOLTAGE_CPU_CACHE_NAME	"cpu_cache"
#define VOLTAGE_SYS_AGENT	4	/* int: System Agent undervolt */
#define VOLTAGE_SYS_AGENT_NAME	"sys_agent"
#define VOLTAGE_ANALOG_IO	5	/* int: Analog I/O undervolt */
#define VOLTAGE_ANALOG_IO_NAME	"analog_io"
#define VOLTAGE_PL1_LIMIT	6	/* int: PL1 limit, in W */
#define VOLTAGE_PL1_LIMIT_NAME	"pl1_limit"
#define VOLTAGE_PL1_TIME	7	/* int: PL1 time, in msec */
#define VOLTAGE_PL1_TIME_NAME	"pl1_time"
#define VOLTAGE_PL2_LIMIT	8	/* int: PL2 limit, in W */
#define VOLTAGE_PL2_LIMIT_NAME	"pl2_limit"
#define VOLTAGE_PL2_TIME	9	/* int: PL2 time, in msec */
#define VOLTAGE_PL2_TIME_NAME	"pl2_time"
#define	VOLTAGE_MAXID		10	/* number of valid machdep ids */

#define	CTL_VOLTAGE_NAMES { \
	{ 0, 0 }, \
	{ VOLTAGE_CPU_NAME, CTLTYPE_INT }, \
	{ VOLTAGE_GPU_NAME, CTLTYPE_INT }, \
	{ VOLTAGE_CPU_CACHE_NAME, CTLTYPE_INT }, \
	{ VOLTAGE_SYS_AGENT_NAME, CTLTYPE_INT }, \
	{ VOLTAGE_ANALOG_IO_NAME, CTLTYPE_INT }, \
	{ VOLTAGE_PL1_LIMIT_NAME, CTLTYPE_INT }, \
	{ VOLTAGE_PL1_TIME_NAME, CTLTYPE_INT }, \
	{ VOLTAGE_PL2_LIMIT_NAME, CTLTYPE_INT }, \
	{ VOLTAGE_PL2_TIME_NAME, CTLTYPE_INT }, \
}

#if defined(_KERNEL)
int voltage_sysctl(int *, u_int, void *, size_t *, void *, size_t, struct proc *);
void voltage_restore(void);
#endif
