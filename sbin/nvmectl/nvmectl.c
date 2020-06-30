/*	$OpenBSD$ */
/*	$NetBSD: nvmectl.c,v 1.3 2017/04/29 00:06:40 nonaka Exp $	*/

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
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/ioccom.h>
#include <sys/stat.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvmectl.h"

static struct nvme_function funcs[] = {
#ifdef DEVLIST_USAGE
	{"devlist",	devlist,	DEVLIST_USAGE},
#endif
	{"identify",	identify,	IDENTIFY_USAGE},
#ifdef PERFTEST_USAGE
	{"perftest",	perftest,	PERFTEST_USAGE},
#endif
#ifdef RESET_USAGE
	{"reset",	reset,		RESET_USAGE},
#endif
	{"logpage",	logpage,	LOGPAGE_USAGE},
#ifdef FIRMWARE_USAGE
	{"firmware",	firmware,	FIRMWARE_USAGE},
#endif
	{"power",	power,		POWER_USAGE},
#ifdef WDC_USAGE
	{"wdc",		wdc,		WDC_USAGE},
#endif
	{NULL,		NULL,		NULL},
};

static void
usage(const struct nvme_function *f)
{
	fprintf(stderr, "usage:\n");
	while (f->name != NULL) {
		fprintf(stderr, "%s", f->usage);
		f++;
	}
	exit(1);
}

void
dispatch(int argc, char *argv[], struct nvme_function *tbl)
{
	struct nvme_function *f = tbl;

	if (argv[1] == NULL) {
		usage(tbl);
		return;
	}

	while (f->name != NULL) {
		if (strcmp(argv[1], f->name) == 0)
			f->fn(argc-1, &argv[1]);
		f++;
	}

	fprintf(stderr, "Unknown command: %s\n", argv[1]);
	usage(tbl);
}

static void
print_bytes(void *data, uint32_t length)
{
	uint32_t	i, j;
	uint8_t		*p, *end;

	end = (uint8_t *)data + length;

	for (i = 0; i < length; i++) {
		p = (uint8_t *)data + (i*16);
		printf("%03x: ", i*16);
		for (j = 0; j < 16 && p < end; j++)
			printf("%02x ", *p++);
		if (p >= end)
			break;
		printf("\n");
	}
	printf("\n");
}

static void
print_dwords(void *data, uint32_t length)
{
	uint32_t	*p;
	uint32_t	i, j;

	p = (uint32_t *)data;
	length /= sizeof(uint32_t);

	for (i = 0; i < length; i+=8) {
		printf("%03x: ", i*4);
		for (j = 0; j < 8; j++)
			printf("%08x ", p[i+j]);
		printf("\n");
	}

	printf("\n");
}

void
print_hex(void *data, uint32_t length)
{
	if (length >= sizeof(uint32_t) || length % sizeof(uint32_t) == 0)
		print_dwords(data, length);
	else
		print_bytes(data, length);
}

void
read_controller_data(int fd, struct nvm_identify_controller *cdata)
{
	struct nvme_pt_command	pt;

	memset(&pt, 0, sizeof(pt));
	pt.cmd.opcode = NVM_ADMIN_IDENTIFY;
	pt.cmd.cdw10 = 1;
	pt.buf = cdata;
	pt.len = sizeof(*cdata);
	pt.is_read = 1;

	if (ioctl(fd, NVME_PASSTHROUGH_CMD, &pt) < 0)
		err(1, "identify request failed");

	if (nvme_completion_is_error(&pt.cpl))
		errx(1, "identify request returned error");
}

void
read_namespace_data(int fd, int nsid, struct nvm_identify_namespace *nsdata)
{
	struct nvme_pt_command	pt;

	memset(&pt, 0, sizeof(pt));
	pt.cmd.opcode = NVM_ADMIN_IDENTIFY;
	pt.cmd.nsid = nsid;
	pt.buf = nsdata;
	pt.len = sizeof(*nsdata);
	pt.is_read = 1;

	if (ioctl(fd, NVME_PASSTHROUGH_CMD, &pt) < 0)
		err(1, "identify request failed");

	if (nvme_completion_is_error(&pt.cpl))
		errx(1, "identify request returned error");
}

int
open_dev(const char *str, int *fd, int show_error, int exit_on_error)
{
	*fd = open(str, O_RDWR);
	if (*fd < 0) {
		if (show_error)
			warn("could not open %s", str);
		if (exit_on_error)
			exit(1);
		else
			return (errno);
	}

	return (0);
}

void
parse_ns_str(const char *ns_str, char *ctrlr_str, int *nsid)
{
	char	*nsloc;

	/*
	 * Pull the namespace id from the string. +2 skips past the "ns" part
	 *  of the string.  Don't search past 10 characters into the string,
	 *  otherwise we know it is malformed.
	 */
	nsloc = strstr(ns_str, NVME_NS_PREFIX);
	if (nsloc > ns_str + 10)
		nsloc = NULL;
	if (nsloc != NULL)
		*nsid = strtol(nsloc + 2, NULL, 10);
	if (nsloc == NULL || (*nsid == 0 && errno != 0))
		errx(1, "invalid namespace ID '%s'", ns_str);

	/*
	 * The controller string will include only the nvmX part of the
	 *  nvmeXnsY string.
	 */
	snprintf(ctrlr_str, nsloc - ns_str + 1, "%s", ns_str);
}

void
nvme_strvis(u_char *dst, int dlen, const u_char *src, int slen)
{
#define STRVIS_ISWHITE(x) ((x) == ' ' || (x) == '\0' || (x) == (u_char)'\377')
	/* Trim leading and trailing blanks and NULs. */
	while (slen > 0 && STRVIS_ISWHITE(src[0]))
		++src, --slen;
	while (slen > 0 && STRVIS_ISWHITE(src[slen - 1]))
		--slen;

	while (slen > 0) {
		if (*src < 0x20 || *src >= 0x80) {
			/* non-printable characters */
			dlen -= 4;
			if (dlen < 1)
				break;
			*dst++ = '\\';
			*dst++ = ((*src & 0300) >> 6) + '0';
			*dst++ = ((*src & 0070) >> 3) + '0';
			*dst++ = ((*src & 0007) >> 0) + '0';
		} else if (*src == '\\') {
			/* quote characters */
			dlen -= 2;
			if (dlen < 1)
				break;
			*dst++ = '\\';
			*dst++ = '\\';
		} else {
			/* normal characters */
			if (--dlen < 1)
				break;
			*dst++ = *src;
		}
		++src, --slen;
	}

	*dst++ = 0;
}

int
main(int argc, char *argv[])
{

	if (argc < 2)
		usage(funcs);

	dispatch(argc, argv, funcs);

	return (0);
}
