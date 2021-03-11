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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/conf.h>
#include <sys/sysctl.h>

#include <machine/voltagevar.h>

#define MSR_ADDR_VOLTAGE	0x150
#define MSR_ADDR_UNITS		0x606
#define MSR_ADDR_TDP		0x610

static int voltage_initialized = 0;

struct voltage {
	int id;
	char *name;
	uint64_t msr;
	int val;
} voltages[] = {
	{ VOLTAGE_CPU,	     VOLTAGE_CPU_NAME,	     0x8000001000000000ULL, 0 },
	{ VOLTAGE_GPU,	     VOLTAGE_GPU_NAME,	     0x8000011000000000ULL, 0 },
	{ VOLTAGE_CPU_CACHE, VOLTAGE_CPU_CACHE_NAME, 0x8000021000000000ULL, 0 },
	{ VOLTAGE_SYS_AGENT, VOLTAGE_SYS_AGENT_NAME, 0x8000031000000000ULL, 0 },
	{ VOLTAGE_ANALOG_IO, VOLTAGE_ANALOG_IO_NAME, 0x8000041000000000ULL, 0 },
};

struct power_limit {
	int id;
	char *name;
	int limit;
	int enabled;
	int time_id;
	char *time_name;
	uint32_t time_microsecs;
} power_limits[] = {
	{ VOLTAGE_PL1_LIMIT, VOLTAGE_PL1_LIMIT_NAME, 0, 0,
	  VOLTAGE_PL1_TIME,  VOLTAGE_PL1_TIME_NAME,  0 },
	{ VOLTAGE_PL2_LIMIT, VOLTAGE_PL2_LIMIT_NAME, 0, 0,
	  VOLTAGE_PL2_TIME,  VOLTAGE_PL2_TIME_NAME,  0 },
};

void voltage_read(int);
void voltage_write(int, int);
void voltage_read_tdp(void);
void voltage_read_limits(void);
void voltage_write_limits(void);

int64_t
iexp2(int exp)
{
	int base = 2;
	int64_t result = 1;
	for (;;) {
		if (exp & 1)
		    result *= base;
		exp >>= 1;
		if (!exp)
		    break;
		base *= base;
	}

	return result;
}

void
voltageattach(int num)
{
	uint64_t msr;
	int i, pr = 0;

	if (strcmp(cpu_vendor, "GenuineIntel") != 0 || voltage_initialized ||
	    num > 1)
		return;

	if (rdmsr_safe(MSR_ADDR_VOLTAGE, &msr) != 0)
		return;

	voltage_initialized = 1;

	for (i = 0; i < nitems(voltages); i++) {
		voltage_read(i);
		if (voltages[i].val != 0) {
			if (pr)
				printf(",");
			else {
				printf("voltage:");
				pr++;
			}

			printf(" %s %s%d mV", voltages[i].name,
			    (voltages[i].val > 0 ? "+" : ""), voltages[i].val);
		}
	}
	if (pr)
		printf("\n");

	voltage_read_limits();

	printf("voltage: ");
	for (i = 0; i < nitems(power_limits); i++) {
		if (i > 0)
			printf(", ");

		if (power_limits[i].enabled) {
			printf("PL%d %dW limit %d microsecs",
			    i + 1,
			    power_limits[i].limit,
			    power_limits[i].time_microsecs);
		} else
			printf("PL%d disabled, ", i + 1);
	}
	printf("\n");
}

void
voltage_read(int idx)
{
	int rval = 0;

	if (!voltage_initialized)
		return;

	wrmsr(MSR_ADDR_VOLTAGE, voltages[idx].msr);
	rval = (rdmsr(MSR_ADDR_VOLTAGE) & 0xffe00000) >> 21;

	if (rval > 0x400)
		rval = -(0x800 - rval);

	if (rval != 0)
		/* lround(val / 1.024) */
		rval = ((((rval * 100000) / 10240) + 5) / 10) -
		    (rval < 1 ? 1 : 0);

	voltages[idx].val = rval;
}

void
voltage_write(int idx, int val)
{
	uint64_t step = val;
	uint64_t msr;

	if (!voltage_initialized)
		return;

	voltage_read(idx);

	printf("voltage: %s: %s%d mV -> %s%d mV\n", voltages[idx].name,
	    (voltages[idx].val > 0 ? "+" : ""), voltages[idx].val,
	    (val > 0 ? "+" : ""), val);

	if (val != 0)
		/* lround(val * 1.024) */
		step = ((((val * 102400) / 10000) + 5) / 10) -
		    (val < 1 ? 1 : 0);

	msr = voltages[idx].msr | (0xffe00000 & ((step & 0xfff) << 21));

	/* set write bit */
	msr |= 0x0000000100000000;

	wrmsr(MSR_ADDR_VOLTAGE, msr);

	/* re-read to make sure we're always showing what the cpu says */
	voltage_read(idx);
}

uint32_t
voltage_power_level_microsecs(uint64_t value, int time_unit)
{
	/* TODO */
	return 0;
}

void
voltage_read_limits(void)
{
	uint64_t limit, units;
	int power_unit, time_unit;

	if (!voltage_initialized)
		return;

	limit = rdmsr(MSR_ADDR_TDP);
	units = rdmsr(MSR_ADDR_UNITS);

	power_unit = iexp2(units & 0xf);
	time_unit = iexp2((units >> 16) & 0xf);

	if ((limit >> 63) & 0x1) {
		/* TODO: tdp locked, block changes? */
	}

	/* PL1 */
	power_limits[0].limit = (limit & 0x7fff) / power_unit;
	power_limits[0].enabled = !!((limit >> 47) & 1);
	power_limits[0].time_microsecs =
	    voltage_power_level_microsecs(limit >> 48, time_unit);

	/* PL2 */
	power_limits[1].limit = ((limit >> 32) & 0x7fff) / power_unit;
	power_limits[1].enabled = !!((limit >> 15) & 1);
	power_limits[1].time_microsecs =
	    voltage_power_level_microsecs(limit >> 16, time_unit);
}

void
voltage_write_limits(void)
{
	printf("%s: todo\n", __func__);
}

int
voltage_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen, struct proc *p)
{
	int i, newval, err;

	if (namelen != 1)
		return (ENOTDIR);

	if (!voltage_initialized)
		return EOPNOTSUPP;

	switch (name[0]) {
	case VOLTAGE_CPU:
	case VOLTAGE_GPU:
	case VOLTAGE_CPU_CACHE:
	case VOLTAGE_SYS_AGENT:
	case VOLTAGE_ANALOG_IO:
		for (i = 0; i < nitems(voltages); i++) {
			if (name[0] != voltages[i].id)
				continue;

			newval = voltages[i].val;

			err = sysctl_int(oldp, oldlenp, newp, newlen, &newval);
			if (err)
				return err;

			if (newlen > 0) {
				if (newval != voltages[i].val)
					voltage_write(i, newval);
				return 0;
			}

			return sysctl_rdint(oldp, oldlenp, newp,
			    voltages[i].val);
		}

		return EOPNOTSUPP;

	case VOLTAGE_PL1_LIMIT:
	case VOLTAGE_PL2_LIMIT:
		for (i = 0; i < nitems(power_limits); i++) {
			if (name[0] != power_limits[i].id)
				continue;

			newval = power_limits[i].limit;

			err = sysctl_int(oldp, oldlenp, newp, newlen, &newval);
			if (err)
				return err;

			if (newlen > 0) {
				if (newval != power_limits[i].limit) {
					power_limits[i].limit = newval;
					voltage_write_limits();
				}
				return 0;
			}

			return sysctl_rdint(oldp, oldlenp, newp,
			    power_limits[i].limit);
		}

		return EOPNOTSUPP;

	case VOLTAGE_PL1_TIME:
	case VOLTAGE_PL2_TIME:
		for (i = 0; i < nitems(power_limits); i++) {
			if (name[0] != power_limits[i].time_id)
				continue;

			newval = power_limits[i].time_microsecs;

			err = sysctl_int(oldp, oldlenp, newp, newlen, &newval);
			if (err)
				return err;

			if (newval != power_limits[i].time_microsecs) {
				power_limits[i].time_microsecs = newval;
				voltage_write_limits();
				return 0;
			}

			return sysctl_rdint(oldp, oldlenp, newp,
			    power_limits[i].time_microsecs);
		}

		return EOPNOTSUPP;
	}

	return EOPNOTSUPP;
}

void
voltage_restore(void)
{
	int i;

	if (!voltage_initialized)
		return;

	for (i = 0; i < nitems(voltages); i++)
		voltage_write(i, voltages[i].val);

	voltage_write_limits();
}
