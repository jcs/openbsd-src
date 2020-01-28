/*	$OpenBSD: atactl.c,v 1.46 2015/08/20 22:02:20 deraadt Exp $	*/
/*	$NetBSD: atactl.c,v 1.4 1999/02/24 18:49:14 jwise Exp $	*/

/*-
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Ken Hornstein.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * nvmectl(8) - a program to control NVMe devices.
 */

#include <sys/param.h>	/* DEV_BSIZE */
#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>

#include <dev/ic/nvmereg.h>
#include <dev/ic/nvmeio.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcidevs.h>
#include <dev/pci/pcidevs_data.h>

const char pci_vendor_unknown[] = "Unknown Vendor";

struct command {
	const char *cmd_name;
	void (*cmd_func)(int, char *[]);
};

int	fd;				/* file descriptor for device */

extern char *__progname;		/* from crt0.o */

int	main(int, char *[]);
__dead void usage(void);
char *	nvme_str(char *, int);
void	nvme_command(struct nvme_ioctl_command *);

void	device_identify(int, char *[]);
void	device_power(int, char *[]);
void	device_setpower(int, char *[]);

struct command commands[] = {
	{ "identify",		device_identify },
	{ "power",		device_power },
	{ "setpower",		device_setpower },
	{ NULL,			NULL },
};

int
main(int argc, char *argv[])
{
	struct command	*cmdp;

	if (argc < 2)
		usage();

	/*
	 * Open the device
	 */
	if ((fd = opendev(argv[1], O_RDWR, OPENDEV_PART, NULL)) == -1)
		err(1, "%s", argv[1]);

	/* Skip program name and device name. */
	if (argc != 2) {
		argv += 2;
		argc -= 2;
	} else {
		argv[1] = "identify";
		argv += 1;
		argc -= 1;
	}

	/* Look up and call the command. */
	for (cmdp = commands; cmdp->cmd_name != NULL; cmdp++)
		if (strcmp(argv[0], cmdp->cmd_name) == 0)
			break;
	if (cmdp->cmd_name == NULL)
		errx(1, "unknown command: %s", argv[0]);

	(cmdp->cmd_func)(argc, argv);

	return (0);
}

__dead void
usage(void)
{

	fprintf(stderr, "usage: %s device [command [arg]]\n", __progname);
	exit(1);
}

char *
nvme_str(char *src, int len)
{
	src[len - 1] = '\0';
	return src;
}

void
nvme_command(struct nvme_ioctl_command *cmd)
{
	if (ioctl(fd, NVMEIOCCOMMAND, cmd) == -1)
		err(1, "NVMEIOCCOMMAND failed");
}

const char *
pci_vendor(u_int16_t id)
{
	const struct pci_known_vendor *pkv;

	for (pkv = pci_known_vendors; pkv->vendorname != NULL; pkv++) {
		if (pkv->vendor == id)
			return pkv->vendorname;
	}

	return pci_vendor_unknown;
}

void
device_identify(int argc, char *argv[])
{
	struct nvme_ioctl_command req;
	struct nvm_identify_controller id;

	if (argc != 1)
		goto usage;

	memset(&req, 0, sizeof(req));

	req.cmd.opcode = NVM_ADMIN_IDENTIFY;
	req.cmd.cdw10 = htole32(1);
	req.buf = (caddr_t)&id;
	req.len = sizeof(id);

	nvme_command(&req);

	printf("Vendor:            %s (%04X)\n", pci_vendor(id.vid), id.vid);
	printf("Subsystem Vendor:  %s (%04X)\n", pci_vendor(id.ssvid), id.ssvid);
	printf("Serial Number:     %s\n", nvme_str(id.sn, sizeof(id.sn)));
	printf("Model Number:      %s\n", nvme_str(id.mn, sizeof(id.mn)));
	printf("Firmware Revision: %s\n", nvme_str(id.fr, sizeof(id.fr)));
	printf("Controller ID:     0x%04X\n", id.cntlid);

	return;

usage:
	fprintf(stderr, "usage: %s device %s\n", __progname, argv[0]);
	exit(1);
}

void
device_power(int argc, char *argv[])
{
	struct nvme_ioctl_command req;
	struct nvm_identify_controller id;
	int i, curpower;

	if (argc != 1)
		goto usage;

	memset(&req, 0, sizeof(req));
	req.cmd.opcode = NVM_ADMIN_GET_FEATURES;
	req.cmd.cdw10 = htole32(NVM_FEAT_POWER_MANAGEMENT);
	nvme_command(&req);
	curpower = req.res.cdw0;

	memset(&req, 0, sizeof(req));
	req.cmd.opcode = NVM_ADMIN_IDENTIFY;
	req.cmd.cdw10 = htole32(1);
	req.buf = (caddr_t)&id;
	req.len = sizeof(id);
	nvme_command(&req);

	printf("Power States Supported: %d\n", id.npss + 1);
	printf(" #  Max pwr  Op Enter Lat Exit Lat  RRT RRL RWT RWL Idle Pwr Act Pwr\n");
	printf("--  -------- -- --------- --------- --- --- --- --- -------- --------\n");
	for (i = 0; i <= id.npss; i++) {
		struct nvm_identify_psd *psd = &id.psd[i];
		int mpower, apower, ipower;
		uint8_t mps, nops, aps;

		mps = (psd->mps_nops >> NVM_PSD_MPS_SHIFT) & NVM_PSD_MPS_MASK;
		nops = (psd->mps_nops >> NVM_PSD_NOPS_SHIFT) & NVM_PSD_NOPS_MASK;
		aps = (psd->apw_aps >> NVM_PSD_APS_SHIFT) & NVM_PSD_APS_MASK;

		mpower = psd->mp;
		if (mps == 0)
			mpower *= 100;
		ipower = psd->idlp;
		if (psd->ips == 1)
			ipower *= 100;
		apower = psd->actp;
		if (aps == 1)
			apower *= 100;

		printf("%2d%c %2d.%04dW %c  %3d.%03dms %3d.%03dms %-3d %-3d %-3d %-3d %2d.%04dW %2d.%04dW\n",
		       i, curpower == i ? '*' : ' ',
		       mpower / 10000, mpower % 10000,
		       nops ? '-' : 'Y', psd->enlat / 1000, psd->enlat % 1000,
		       psd->exlat / 1000, psd->exlat % 1000, psd->rrt, psd->rrl,
		       psd->rwt, psd->rwl, ipower / 10000, ipower % 10000,
		       apower / 10000, apower % 10000);
	}

	return;

usage:
	fprintf(stderr, "usage: %s device %s\n", __progname, argv[0]);
	exit(1);
}

void
device_setpower(int argc, char *argv[])
{
	struct nvme_ioctl_command req;
	const char *errstr;
	int level;

	if (argc != 2)
		goto usage;

	level = strtonum(argv[1], 0, 32, &errstr);
	if (errstr != NULL) {
		warn("invalid power state %s: %s", argv[1], errstr);
		goto usage;
	}

	printf("Setting power level to %d\n", level);

	memset(&req, 0, sizeof(req));
	req.cmd.opcode = NVM_ADMIN_SET_FEATURES;
	req.cmd.cdw10 = htole32(NVM_FEAT_POWER_MANAGEMENT); // | (1u << 31));
	req.cmd.cdw11 = htole32(level);
	nvme_command(&req);

	device_power(1, argv);

	return;

usage:
	fprintf(stderr, "usage: %s device %s <power level>\n", __progname,
	    argv[0]);
	exit(1);
}
