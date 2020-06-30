/*	$OpenBSD$ */
/*	$NetBSD: nvmectl.h,v 1.3 2017/04/29 00:06:40 nonaka Exp $	*/

/*-
 * Copyright (C) 2012-2013 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: head/sbin/nvmecontrol/nvmecontrol.h 314230 2017-02-25 00:09:16Z imp $
 */

#ifndef __NVMECTL_H__
#define __NVMECTL_H__

#include <sys/ioctl.h>

#include <dev/ic/nvmeio.h>
#include "nvme.h"

#define	__LOWEST_SET_BIT(__mask) ((((__mask) - 1) & (__mask)) ^ (__mask))
#define	__SHIFTOUT(__x, __mask)	(((__x) & (__mask)) / __LOWEST_SET_BIT(__mask))
#define	__SHIFTIN(__x, __mask) ((__x) * __LOWEST_SET_BIT(__mask))
#define	__SHIFTOUT_MASK(__mask) __SHIFTOUT((__mask), (__mask))

typedef void (*nvme_fn_t)(int argc, char *argv[]);

struct nvme_function {
	const char	*name;
	nvme_fn_t	fn;
	const char	*usage;
};

#define NVME_CTRLR_PREFIX	"nvme"
#define NVME_NS_PREFIX		"ns"

#if 0
#define DEVLIST_USAGE							       \
"       nvmectl devlist\n"
#endif

#define IDENTIFY_USAGE							       \
"       nvmectl identify [-x [-v]] <device>\n"

#if 0
#define PERFTEST_USAGE							       \
"       nvmectl perftest <-n num_threads> <-o read|write>\n"		       \
"                        <-s size_in_bytes> <-t time_in_seconds>\n"	       \
"                        <-i intr|wait> [-f refthread] [-p]\n"		       \
"                        <namespace id>\n"
#endif

#if 0
#define RESET_USAGE							       \
"       nvmectl reset <controller id>\n"
#endif

#define LOGPAGE_USAGE							       \
"       nvmectl logpage <-p page_id> [-b] [-v vendor] [-x] <device>\n"

#if 0
#define FIRMWARE_USAGE							       \
"       nvmectl firmware [-s slot] [-f path_to_firmware] [-a] <controller id>\n"
#endif

#define POWER_USAGE							       \
"       nvmectl power [-l] [-p new-state [-w workload-hint]] <device>\n"

#if 0
#define WDC_USAGE							       \
"       nvmecontrol wdc (cap-diag|drive-log|get-crash-dump|purge|purge-montior)\n"
#endif

#ifdef DEVLIST_USAGE
void devlist(int, char *[]);
#endif
void identify(int, char *[]);
#ifdef PERFTEST_USAGE
void perftest(int, char *[]);
#endif
#ifdef RESET_USAGE
void reset(int, char *[]);
#endif
void logpage(int, char *[]);
#ifdef FIRMWARE_USAGE
void firmware(int, char *[]);
#endif
void power(int, char *[]);
#ifdef WDC_USAGE
void wdc(int, char *[]);
#endif

int open_dev(const char *, int *, int, int);
void parse_ns_str(const char *, char *, int *);
void read_controller_data(int, struct nvm_identify_controller *);
void read_namespace_data(int, int, struct nvm_identify_namespace *);
void print_hex(void *, uint32_t);
void read_logpage(int, uint8_t, int, void *, uint32_t);
void gen_usage(struct nvme_function *) __dead;
void dispatch(int argc, char *argv[], struct nvme_function *f);
void nvme_strvis(uint8_t *, int, const uint8_t *, int);

#define __GEN_ENDIAN_ENC(bits, endian) \
static __inline void __unused \
endian ## bits ## enc(void *dst, uint ## bits ## _t u) \
{ \
	u = hto ## endian ## bits (u); \
	__builtin_memcpy(dst, &u, sizeof(u)); \
}

__GEN_ENDIAN_ENC(16, be)
__GEN_ENDIAN_ENC(32, be)
__GEN_ENDIAN_ENC(64, be)
__GEN_ENDIAN_ENC(16, le)
__GEN_ENDIAN_ENC(32, le)
__GEN_ENDIAN_ENC(64, le)
#undef __GEN_ENDIAN_ENC

#define __GEN_ENDIAN_DEC(bits, endian) \
static __inline uint ## bits ## _t __unused \
endian ## bits ## dec(const void *buf) \
{ \
	uint ## bits ## _t u; \
	__builtin_memcpy(&u, buf, sizeof(u)); \
	return endian ## bits ## toh (u); \
}

__GEN_ENDIAN_DEC(16, be)
__GEN_ENDIAN_DEC(32, be)
__GEN_ENDIAN_DEC(64, be)
__GEN_ENDIAN_DEC(16, le)
__GEN_ENDIAN_DEC(32, le)
__GEN_ENDIAN_DEC(64, le)
#undef __GEN_ENDIAN_DEC
#endif	/* __NVMECTL_H__ */
