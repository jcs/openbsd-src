/*	$OpenBSD$ */
/*
 * Copyright (c) 2020 joshua stein <jcs@jcs.org>
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
 * "The default HWP control field values are expected to be suitable for many
 * applications. The OS can enable autonomous HWP for these common cases by:
 *
 * Setting IA32_HWP_REQUEST.Desired Performance = 0
 * (hardware autonomous selection determines the performance target).
 *
 * Set IA32_HWP_REQUEST.Activity Window = 0 (enable HW dynamic selection of
 * window size).
 *
 * To maximize HWP benefit for the common
 * cases, the OS should set:
 * IA32_HWP_REQUEST.Minimum_Performance =
 *   IA32_HWP_CAPABILITIES.Lowest_Performance and
 * IA32_HWP_REQUEST.Maximum_Performance =
 *   IA32_HWP_CAPABILITIES.Highest_Performance."
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/malloc.h>

#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/specialreg.h>
#include <machine/bus.h>

#include "acpicpu.h"

#if NACPICPU > 0
#include <dev/acpi/acpidev.h>
#endif

#define HWP_EPP_PERFORMANCE		0x00
#define HWP_EPP_BALANCE_PERFORMANCE	0x80
#define HWP_EPP_BALANCE_POWERSAVE	0xC0
#define HWP_EPP_POWERSAVE		0xFF

#define HWP_EPB_PERFORMANCE		0x00
#define HWP_EPB_BALANCE_PERFORMANCE	0x04
#define HWP_EPB_BALANCE_POWERSAVE	0x08
#define HWP_EPB_POWERSAVE		0x0C

extern int setperf_prio;
extern int perflevel;

static int pstate_hwp = 0;

static struct {
	uint16_t highest_perf;
	uint16_t guaranteed_perf;
	uint16_t most_efficient;
	uint16_t lowest_perf;
} pstate_hwp_save;

static struct {
	int epp_min;
	int epp_max;
	int epb_min;
	int epb_max;
	char *label;
} pstate_epp_labels[] = {
	{ 0x00,	0x79,	0x00,	0x03,	"performance" },
	{ 0x80, 0xbf,	0x04,	0x07,	"balance_performance" },
	{ 0xc0, 0xfe,	0x08,	0x0b,	"balance_power" },
	{ 0xff, 0xff,	0x0c,	0x0f,	"power" },
};

const char *pstate_epp_label(int, int);

void
pstate_init(struct cpu_info *ci)
{
	const char *cpu_device = ci->ci_dev->dv_xname;
	uint64_t msr;
	int16_t eppepb;

	if (rdmsr_safe(IA32_HWP_CAPABILITIES, &msr) != 0)
		return;

	if (ci->ci_feature_tpmflags_eax & TPM_HWP_EPP)
		eppepb = (rdmsr(IA32_HWP_REQUEST) >> 24) & 0xff;
	else if (ci->ci_feature_tpmflags_ecx & TPM_EPB)
		eppepb = rdmsr(IA32_ENERGY_PERF_BIAS) & 0x0f;
	else {
		printf("%s: no energy bias control\n", cpu_device);
		return;
	}

	wrmsr(IA32_PM_ENABLE, 1);
	if (rdmsr(IA32_PM_ENABLE) != 1) {
		printf("%s: enabling HWP failed\n", cpu_device);
		return;
	}

	pstate_hwp = 1;
	setperf_prio = 1;
	cpu_setperf = pstate_setperf;

	printf("%s: HWP enabled, bias ", cpu_device);

	if (ci->ci_feature_tpmflags_eax & TPM_HWP_EPP)
		printf("%s", pstate_epp_label(eppepb, 0));
	else if (ci->ci_feature_tpmflags_ecx & TPM_EPB)
		printf("%s", pstate_epp_label(eppepb, 1));

	pstate_hwp_save.highest_perf = msr & 0xff;
	pstate_hwp_save.guaranteed_perf = (msr >> 8) & 0xff;
	pstate_hwp_save.most_efficient = (msr >> 16) & 0xff;
	pstate_hwp_save.lowest_perf = (msr >> 24) & 0xff;

	printf(", highest performance %d MHz, guaranteed %d MHz, "
	    "most efficient %d MHz, lowest performance %d MHz\n",
	    pstate_hwp_save.highest_perf * 100,
	    pstate_hwp_save.guaranteed_perf * 100,
	    pstate_hwp_save.most_efficient * 100,
	    pstate_hwp_save.lowest_perf * 100);
}

const char *
pstate_epp_label(int val, int epp_epb)
{
	int i;

	if (epp_epb == 0) {
		for (i = 0; i < (sizeof(pstate_epp_labels) /
		    sizeof(pstate_epp_labels[0])); i++) {
			if (val >= pstate_epp_labels[i].epp_min &&
			    val <= pstate_epp_labels[i].epp_max)
				return pstate_epp_labels[i].label;
		}
	} else {
		for (i = 0; i < (sizeof(pstate_epp_labels) /
		    sizeof(pstate_epp_labels[0])); i++) {
			if (val >= pstate_epp_labels[i].epb_min &&
			    val <= pstate_epp_labels[i].epb_max)
				return pstate_epp_labels[i].label;
		}
	}

	return "unknown";
}


void
pstate_setperf(int level)
{
	printf("%s(%d)\n", __func__, level);
}

/* TODO: update cpuspeed in response to hwp notifications */

int
pstate_hwp_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen, struct proc *p)
{
	uint64_t hwp_req, eppepb;
	const char *bias;
	char newbias[64];
	int newval;
	int err;

	if (namelen != 1)
		return ENOTDIR;

	if (!pstate_hwp)
		return EOPNOTSUPP;

	if (name[0] < 1 || name[0] >= HWP_MAXID)
		return EOPNOTSUPP;

	hwp_req = rdmsr(IA32_HWP_REQUEST);

	switch (name[0]) {
	case HWP_MIN_PERF:
	case HWP_MAX_PERF:
	case HWP_DESIRED_PERF:
		err = sysctl_int(oldp, oldlenp, newp, newlen, &newval);
		if (err)
			return err;

		printf("%s: name[0] %d, newval %d, hwpreq 0x%llx\n", __func__,
		    name[0], newval, hwp_req);

		switch (name[0]) {
		case HWP_MIN_PERF:
			if (newlen == 0)
				return sysctl_rdint(oldp, oldlenp, newp,
				    hwp_req & 0xff);
			hwp_req |= (newval & 0xff);
			break;
		case HWP_MAX_PERF:
			if (newlen == 0)
				return sysctl_rdint(oldp, oldlenp, newp,
				    (hwp_req >> 8) & 0xff);
			hwp_req |= ((newval & 0xff) << 8);
			break;
		case HWP_DESIRED_PERF:
			if (newlen == 0)
				return sysctl_rdint(oldp, oldlenp, newp,
				    (hwp_req >> 16) & 0xff);
			hwp_req |= ((newval & 0xff) << 16);
			break;
		}

		printf("%s: name[0] %d, newval %d, writing hwpreq 0x%llx\n",
		    __func__, name[0], newval, hwp_req);

		wrmsr(IA32_HWP_REQUEST, hwp_req);
		return 0;

	case HWP_EPP:
		if (newlen == 0) {
			/* XXX: is curcpu() ok? */
			if (curcpu()->ci_feature_tpmflags_eax & TPM_HWP_EPP) {
				eppepb = (hwp_req >> 24) & 0xff;
				bias = pstate_epp_label(eppepb, 0);
			} else if (curcpu()->ci_feature_tpmflags_ecx & TPM_EPB)
			{
				eppepb = rdmsr(IA32_ENERGY_PERF_BIAS) & 0x0f;
				bias = pstate_epp_label(eppepb, 1);
			}

			return sysctl_rdstring(oldp, oldlenp, newp, bias);
		}

		err = sysctl_string(oldp, oldlenp, newp, newlen, newbias,
		    sizeof(newbias));
		if (err)
			return err;

		printf("need to change epp/epb bias to \"%s\"\n", newbias);

		return 0;
	}

	return EOPNOTSUPP;
}
