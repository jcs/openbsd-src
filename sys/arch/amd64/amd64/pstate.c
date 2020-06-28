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
 * To maximize HWP benefit for the common cases, the OS should set:
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

extern int setperf_prio;

static int pstate_hwp = 0;
static int pstate_hwp_bias_style = -1;
enum {
	PSTATE_HWP_BIAS_EPP,
	PSTATE_HWP_BIAS_EPB,
};

/* IA32_HWP_CAPABILITIES */
union hwp_capabilities {
	uint64_t msr;
	struct {
		uint8_t highest_perf;
		uint8_t guaranteed_perf;
		uint8_t most_efficient;
		uint8_t lowest_perf;
		uint32_t reserved;
	} __packed fields;
} pstate_hwp_cap;

/* IA32_HWP_REQUEST / IA32_HWP_REQUEST_PKG */
union hwp_request {
	uint64_t msr;
	struct {
		uint8_t min_perf;
		uint8_t max_perf;
		uint8_t desired_perf;
		uint8_t epp;
		uint16_t act_win : 10;
		uint8_t package : 1;
		uint32_t reserved : 21;
	} __packed fields;
} pstate_hwp_req;

uint64_t pstate_epb;

static struct {
	int epb_min;
	int epb_max;
	int epp;
	char *label;
} pstate_epp_labels[] = {
	{ 0x00,	0x03, 0x00, "performance" },
	{ 0x04,	0x07, 0x80, "balance_performance" },
	{ 0x08,	0x0b, 0xc0, "balance_powersave" },
	{ 0x0c,	0x0f, 0xff, "powersave" },
};

const char *pstate_hwp_bias_label(int);
void pstate_commit(void);

void
pstate_init(struct cpu_info *ci)
{
	const char *cpu_device = ci->ci_dev->dv_xname;
	union hwp_request hwp_req;
	uint64_t msr;
	int16_t eppepb;

	if (rdmsr_safe(MSR_PLATFORM_INFO, &msr) != 0)
		return;

	/* power management must be enabled before reading capabilities */
	wrmsr(IA32_PM_ENABLE, 1);
	if (rdmsr(IA32_PM_ENABLE) != 1) {
		printf("%s: enabling HWP failed\n", cpu_device);
		return;
	}

	if (rdmsr_safe(IA32_HWP_CAPABILITIES, &pstate_hwp_cap.msr) != 0) {
		printf("%s: no HWP capabilities\n", cpu_device);
		/* XXX: what are we supposed to do now? */
		return;
	}

	if (ci->ci_feature_tpmflags_eax & TPM_HWP_EPP) {
		pstate_hwp_bias_style = PSTATE_HWP_BIAS_EPP;
		pstate_hwp_req.msr = rdmsr(IA32_HWP_REQUEST_PKG);
		eppepb = hwp_req.fields.epp;
	} else if (ci->ci_feature_tpmflags_ecx & TPM_EPB) {
		pstate_hwp_bias_style = PSTATE_HWP_BIAS_EPB;
		eppepb = pstate_epb = rdmsr(IA32_ENERGY_PERF_BIAS) & 0x0f;
	} else {
		printf("%s: no energy bias control\n", cpu_device);
		return;
	}

	/* XXX: should we force epb to performance by default? */

	pstate_hwp = 1;
	setperf_prio = 1;
	cpu_setperf = pstate_setperf;

	printf("%s: HWP enabled, bias %s, highest perf %d MHz, "
	    "guaranteed %d MHz, most efficient %d MHz, lowest perf %d MHz\n",
	    cpu_device, pstate_hwp_bias_label(eppepb),
	    pstate_hwp_cap.fields.highest_perf * 100,
	    pstate_hwp_cap.fields.guaranteed_perf * 100,
	    pstate_hwp_cap.fields.most_efficient * 100,
	    pstate_hwp_cap.fields.lowest_perf * 100);
}

const char *
pstate_hwp_bias_label(int val)
{
	int i;

	for (i = 0; i < (sizeof(pstate_epp_labels) /
	    sizeof(pstate_epp_labels[0])); i++) {
		if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPP) {
			if (val == pstate_epp_labels[i].epp)
				return pstate_epp_labels[i].label;
		} else if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPB) {
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
	int scaled;

	if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPP) {
		/*
		 * Always operate on IA32_HWP_REQUEST_PKG even though we get
		 * called once per CPU (and should be using IA32_HWP_REQUEST),
		 * otherwise the per-package sysctl values will get out of
		 * sync.
		 */
		pstate_hwp_req.msr = rdmsr(IA32_HWP_REQUEST_PKG);

		/* map 0-100 to cap.lowest-cap.highest */
		scaled = ((level * 10) / (1000 /
		    (pstate_hwp_cap.fields.highest_perf -
		    pstate_hwp_cap.fields.lowest_perf))) +
		    pstate_hwp_cap.fields.lowest_perf;
		if (scaled > pstate_hwp_cap.fields.highest_perf)
			scaled = pstate_hwp_cap.fields.highest_perf;

		pstate_hwp_req.fields.desired_perf = scaled;

		printf("%s: %s(%d) -> %d\n", curcpu()->ci_dev->dv_xname,
		    __func__, level, scaled);
		wrmsr(IA32_HWP_REQUEST_PKG, pstate_hwp_req.msr);
	}

	printf("\n");
}

void
pstate_commit(void)
{
}

/* TODO: update cpuspeed in response to hwp notifications */

int
pstate_hwp_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen, struct proc *p)
{
	uint64_t epb;
	const char *bias;
	char newbias[64];
	int newval, err, i, found = 0;

	if (namelen != 1)
		return ENOTDIR;

	if (!pstate_hwp)
		return EOPNOTSUPP;

	if (name[0] < 1 || name[0] >= HWP_MAXID)
		return EOPNOTSUPP;

	pstate_hwp_cap.msr = rdmsr(IA32_HWP_CAPABILITIES);
	printf("guaranteed_perf = %d\n", pstate_hwp_cap.fields.guaranteed_perf);

	pstate_hwp_req.msr = rdmsr(IA32_HWP_REQUEST_PKG);

	switch (name[0]) {
	case HWP_MIN_PERF:
	case HWP_MAX_PERF:
	case HWP_DESIRED_PERF:
		switch (name[0]) {
		case HWP_MIN_PERF:
			newval = pstate_hwp_req.fields.min_perf;
			break;
		case HWP_MAX_PERF:
			newval = pstate_hwp_req.fields.max_perf;
			break;
		case HWP_DESIRED_PERF:
			newval = pstate_hwp_req.fields.desired_perf;
			break;
		}

		if (newlen == 0)
			return sysctl_rdint(oldp, oldlenp, newp, newval);

		err = sysctl_int(oldp, oldlenp, newp, newlen, &newval);
		if (err)
			return err;

		switch (name[0]) {
		case HWP_MIN_PERF:
			pstate_hwp_req.fields.min_perf = newval;
			break;
		case HWP_MAX_PERF:
			pstate_hwp_req.fields.max_perf = newval;
			break;
		case HWP_DESIRED_PERF:
			pstate_hwp_req.fields.desired_perf = newval;
			break;
		}

		printf("%s: name[0] %d, newval %d [%zu], writing hwpreq 0x%llx\n",
		    curcpu()->ci_dev->dv_xname, name[0], newval, newlen,
		    pstate_hwp_req.msr);
		break;

	case HWP_EPP:
		if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPP)
			epb = pstate_hwp_req.fields.epp;
		else if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPB)
			pstate_epb = epb = rdmsr(IA32_ENERGY_PERF_BIAS) & 0x0f;

		bias = pstate_hwp_bias_label(epb);

		if (newlen == 0)
			return sysctl_rdstring(oldp, oldlenp, newp, bias);

		memcpy(newbias, bias, sizeof(newbias));
		err = sysctl_string(oldp, oldlenp, newp, newlen, newbias,
		    sizeof(newbias));
		if (err)
			return err;

		for (i = 0; i < (sizeof(pstate_epp_labels) /
		    sizeof(pstate_epp_labels[0])); i++) {
			if (strcmp(pstate_epp_labels[i].label, newbias) != 0)
				continue;

			if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPP)
				pstate_hwp_req.fields.epp =
				    pstate_epp_labels[i].epp;
			else if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPB)
				pstate_epb = pstate_epp_labels[i].epb_max;

			found = 1;
		}

		if (!found)
			return EINVAL;

		printf("%s: changing epp/epb bias to \"%s\" (0x%llx)\n",
		    curcpu()->ci_dev->dv_xname, newbias, pstate_hwp_req.msr);
		break;

	default:
		return EOPNOTSUPP;
	}

	if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPP)
		wrmsr(IA32_HWP_REQUEST_PKG, pstate_hwp_req.msr);
	else if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPB)
		wrmsr(IA32_ENERGY_PERF_BIAS, pstate_epb);

	return 0;
}
