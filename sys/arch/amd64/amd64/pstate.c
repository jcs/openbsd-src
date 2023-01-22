/*	$OpenBSD$ */
/*
 * Intel Hardware-Controlled Performance States (HWP)
 *
 * Copyright (c) 2020-2026 joshua stein <jcs@jcs.org>
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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/malloc.h>
#include <sys/rwlock.h>
#include <sys/sched.h>

#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/specialreg.h>
#include <machine/bus.h>

extern int setperf_prio;

int pstate_hwp = 0;
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

static struct rwlock pstate_lock = RWLOCK_INITIALIZER("hwp");

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
static void pstate_write_curcpu(void);
static void pstate_apply_all(void);
static void pstate_ap_init(struct cpu_info *);

void
pstate_init(struct cpu_info *ci)
{
	const char *cpu_device = ci->ci_dev->dv_xname;
	CPU_INFO_ITERATOR cii;
	struct cpu_info *ci2;
	uint64_t msr;
	int eppepb;

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
		return;
	}

	if (ci->ci_feature_tpmflags_eax & TPM_HWP_EPP)
		pstate_hwp_bias_style = PSTATE_HWP_BIAS_EPP;
	else if (ci->ci_feature_tpmflags_ecx & TPM_EPB)
		pstate_hwp_bias_style = PSTATE_HWP_BIAS_EPB;
	else {
		printf("%s: no energy bias control\n", cpu_device);
		return;
	}

	/* preserve firmware defaults */
	if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPP) {
		pstate_hwp_req.msr = rdmsr(IA32_HWP_REQUEST);
		pstate_hwp_req.fields.package = 0;
		pstate_hwp_req.fields.reserved = 0;
		eppepb = pstate_hwp_req.fields.epp;
	} else {
		pstate_hwp_req.msr = 0;
		pstate_epb = rdmsr(IA32_ENERGY_PERF_BIAS) & 0x0f;
		eppepb = pstate_epb;
	}

	pstate_hwp_req.fields.min_perf = pstate_hwp_cap.fields.lowest_perf;
	pstate_hwp_req.fields.max_perf = pstate_hwp_cap.fields.highest_perf;
	pstate_hwp_req.fields.desired_perf = 0;
	pstate_hwp_req.fields.act_win = 0;

	pstate_hwp = 1;

	/* prevent acpicpu from claiming cpu_setperf */
	setperf_prio = 31;

	pstate_write_curcpu();
	CPU_INFO_FOREACH(cii, ci2) {
		if (CPU_IS_PRIMARY(ci2))
			continue;
		ci2->cpu_setup = pstate_ap_init;
	}

	printf("%s: HWP bias %s, %d-%d MHz (guaranteed %d MHz, "
	    "most efficient %d MHz)\n", cpu_device,
	    pstate_hwp_bias_label(eppepb),
	    pstate_hwp_cap.fields.lowest_perf * 100,
	    pstate_hwp_cap.fields.highest_perf * 100,
	    pstate_hwp_cap.fields.guaranteed_perf * 100,
	    pstate_hwp_cap.fields.most_efficient * 100);
}

static void
pstate_write_curcpu(void)
{
	wrmsr(IA32_HWP_REQUEST, pstate_hwp_req.msr);
	if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPB)
		wrmsr(IA32_ENERGY_PERF_BIAS, pstate_epb);
}

static void
pstate_ap_init(struct cpu_info *ci)
{
	if (!pstate_hwp)
		return;
	wrmsr(IA32_PM_ENABLE, 1);
	pstate_write_curcpu();
}

static void
pstate_apply_all(void)
{
	CPU_INFO_ITERATOR cii;
	struct cpu_info *ci;

	if (!pstate_hwp)
		return;

	CPU_INFO_FOREACH(cii, ci) {
		sched_peg_curproc(ci);
		pstate_write_curcpu();
		sched_unpeg_curproc();
	}
}

void
pstate_resume(struct cpu_info *ci)
{
	uint64_t msr;

	if (!pstate_hwp)
		return;

	if (rdmsr_safe(MSR_PLATFORM_INFO, &msr) != 0) {
		pstate_hwp = 0;
		return;
	}

	wrmsr(IA32_PM_ENABLE, 1);
	if (rdmsr(IA32_PM_ENABLE) != 1) {
		printf("%s: re-enabling HWP failed\n", ci->ci_dev->dv_xname);
		pstate_hwp = 0;
		return;
	}

	rw_enter_write(&pstate_lock);
	pstate_apply_all();
	rw_exit_write(&pstate_lock);
}

const char *
pstate_hwp_bias_label(int val)
{
	int i;

	for (i = 0; i < nitems(pstate_epp_labels); i++) {
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

int
pstate_hwp_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen, struct proc *p)
{
	char newbias[32];
	const char *bias;
	int newval, err, i;

	if (namelen != 1)
		return ENOTDIR;

	if (!pstate_hwp)
		return EOPNOTSUPP;

	if (name[0] < 1 || name[0] >= HWP_MAXID)
		return EOPNOTSUPP;

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

		err = sysctl_int_bounded(oldp, oldlenp, newp, newlen, &newval,
		    0, 0xff);
		if (err)
			return err;

		rw_enter_write(&pstate_lock);
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
		pstate_apply_all();
		rw_exit_write(&pstate_lock);
		return 0;

	case HWP_EPP:
		if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPP)
			bias = pstate_hwp_bias_label(pstate_hwp_req.fields.epp);
		else
			bias = pstate_hwp_bias_label(pstate_epb);

		if (newlen == 0)
			return sysctl_rdstring(oldp, oldlenp, newp, bias);

		strlcpy(newbias, bias, sizeof(newbias));
		err = sysctl_string(oldp, oldlenp, newp, newlen, newbias,
		    sizeof(newbias));
		if (err)
			return err;

		for (i = 0; i < nitems(pstate_epp_labels); i++) {
			if (strcmp(pstate_epp_labels[i].label, newbias) != 0)
				continue;

			rw_enter_write(&pstate_lock);
			if (pstate_hwp_bias_style == PSTATE_HWP_BIAS_EPP)
				pstate_hwp_req.fields.epp =
				    pstate_epp_labels[i].epp;
			else
				pstate_epb = pstate_epp_labels[i].epb_max;
			pstate_apply_all();
			rw_exit_write(&pstate_lock);
			return 0;
		}
		return EINVAL;
	}

	return EOPNOTSUPP;
}
