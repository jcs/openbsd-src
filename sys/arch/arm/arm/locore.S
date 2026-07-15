/*	$OpenBSD: locore.S,v 1.23 2022/12/08 01:25:44 guenther Exp $	*/
/*	$NetBSD: locore.S,v 1.14 2003/04/20 16:21:40 thorpej Exp $	*/

/*
 * Copyright (C) 1994-1997 Mark Brinicombe
 * Copyright (C) 1994 Brini
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Brini.
 * 4. The name of Brini may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY BRINI ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL BRINI BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "assym.h"

#include <sys/syscall.h>
#include <sys/errno.h>

#include <machine/asm.h>
#include <machine/frame.h>
#include <arm/armreg.h>
#include <arm/sysreg.h>
#include <arm/pte.h>

/* What size should this really be ? It is only used by init_arm() */
#define INIT_ARM_STACK_SIZE	2048

.arch_extension virt

/*
 * This is for kvm_mkdb, and should be the address of the beginning
 * of the kernel text segment (not necessarily the same as kernbase).
 */

	.text
	.align	2

ENTRY_NP(kernel_text)

ASENTRY_NP(start)
	mov	r5, r1
	mov	r6, r2
	mov	r7, r3
	adr	r1, .Lstart
	ldmia	r1, {r1, r2, r8, sp}	/* Set initial stack and */

	mcr	CP15_TPIDRPRW(r8)	/* put curcpu into the TPIDRPRW */

	sub	r2, r2, r1		/* get zero init data */
	mov	r3, #0

.L1:
	str	r3, [r1], #0x0004	/* Zero the bss */
	subs	r2, r2, #4
	bgt	.L1

	mov	fp, #0x00000000		/* trace back starts here */
	mov	r1, r5
	mov	r2, r6
	mov	r3, r7
	bl	initarm			/* Off we go */

	/* init arm will return the new stack pointer. */
	mov	sp, r0

	mov	fp, #0x00000000		/* trace back starts here */
	mov	ip, sp
	stmfd	sp!, {fp, ip, lr, pc}
	sub	fp, ip, #4

	bl	main			/* call main()! */

	adr	r0, .Lmainreturned
	b	panic
	/* NOTREACHED */

.Lstart:
	.word	_edata
	.word	_end
	.word	cpu_info_primary
	.word	svcstk + INIT_ARM_STACK_SIZE

.Lmainreturned:
	.asciz	"main() returned"
	.align	2

	.globl drop_to_pl1
drop_to_pl1:
	/* Drop to PL1 */
	mrs	r0, cpsr
	and	r0, r0, #(PSR_MODE)
	teq	r0, #(PSR_HYP32_MODE)
	beq	1f			/* If we are not in HYP, we're done */
	mov	pc, lr
1:
	mrs	r0, cpsr
	bic	r0, r0, #(PSR_MODE)
	orr	r0, r0, #(PSR_SVC32_MODE)
	orr	r0, r0, #(PSR_I | PSR_F | PSR_A)
	msr	spsr_fsxc, r0
	msr	ELR_hyp, lr
	eret				/* Use the HYP LR to get into SVC */
	dsb nsh
	isb

	.globl start_mmu
start_mmu:
	/* Set ASID to zero */
	mov	r1, #0
	mcr	CP15_CONTEXTIDR(r1)
	isb

	mcr	CP15_TTBR0(r0)		/* Set TTB */
	mcr	CP15_TLBIALL(r0)	/* Flush TLB */

	/* Set the Domain Access register.  Very important! */
	mov     r0, #DOMAIN_CLIENT	/* We only use domain 0 */
	mcr	CP15_DACR(r0)
	isb

	/* Enable MMU */
	mrc	CP15_SCTLR(r0)
	orr	r0, r0, #CPU_CONTROL_MMU_ENABLE
	mcr	CP15_SCTLR(r0)
	isb

	mov	pc, lr

#ifdef MULTIPROCESSOR

	.globl cpu_hatch
cpu_hatch:
	/* Drop to PL1 */
	bl	drop_to_pl1

	/* Convert virt -> phys */
	mov	r9, pc
	and	r9, r9, #0xf0000000

	/*
	 * Map our mpidr to a slot in the hatch table, like the
	 * cpu_mpidr array in NetBSD's cpu_mpstart.  The sram mailbox
	 * is shared, so a core parked in the bootrom also grabs the
	 * next core's release; a core that does not find itself in
	 * the table yet waits here with the mmu and caches off,
	 * inert, until the primary publishes its slot.
	 */
	mrc	p15, 0, r0, c0, c0, 5	/* mpidr */
	movw	r2, #0xffff
	movt	r2, #0x00ff
	and	r0, r0, r2
1:	adr	r1, .Lcpu_hatch_mpidr
	ldr	r1, [r1]
	bic	r1, r1, #0xf0000000
	orr	r1, r1, r9
	mov	r2, #0			/* cpu index */
2:	ldr	r3, [r1], #4
	teq	r3, r0
	beq	3f
	add	r2, r2, #1
	cmp	r2, #MAXCPUS
	blt	2b
	wfe
	b	1b
3:	mov	r10, r2			/* cpu index survives the flushes */

	/*
	 * Toss whatever the caches, branch predictor, and TLB held
	 * before we were reset.  A core released from reset can still
	 * hold stale valid and dirty lines from its previous life
	 * which would be served, or worse written back, once the
	 * cache is enabled.
	 */
	bl	armv7_dcache_l1inv_all
	mov	r0, #0
	mcr	CP15_ICIALLU		/* invalidate icache */
	mcr	CP15_BPIALL		/* invalidate branch predictor */
	mcr	CP15_TLBIALL(r0)	/* invalidate tlb */
	clrex
	dsb	sy
	isb	sy

	/*
	 * Join the coherency domain before the MMU and caches come on.
	 */
	mrc	CP15_ACTLR(r0)
	orr	r0, r0, #CORTEXA9_AUXCTL_SMP
	mcr	CP15_ACTLR(r0)
	isb

	/* Set up CPU info from our slot in the hatch table */
	adr	r0, .Lcpu_hatch_ci
	ldr	r0, [r0]
	bic	r0, r0, #0xf0000000
	orr	r0, r0, r9
	ldr	r0, [r0, r10, lsl #2]
	ldr	r1, [r0, #CI_SELF]
	mcr	CP15_TPIDRPRW(r1)
	ldr	r8, [r0, #CI_TTBR0]

	/* Enable the mmu */
	adr	r0, .Lcpu_hatch_ttb
	ldr	r0, [r0]
	bic	r0, r0, #0xf0000000
	orr	r0, r0, r9
	ldr	r0, [r0]
	bl	start_mmu

	/*
	 * Enable the caches now so everything we touch is coherent
	 * with the other cores before we enter C code.
	 */
	mrc	CP15_SCTLR(r0)
	orr	r0, r0, #CPU_CONTROL_DC_ENABLE
	orr	r0, r0, #(CPU_CONTROL_IC_ENABLE | CPU_CONTROL_BPRD_ENABLE)
	mcr	CP15_SCTLR(r0)
	isb

	adr	r1, .Lcpu_hatch_virt
	ldr	pc, [r1]

cpu_hatch_virt:
	mcr	CP15_TTBR0(r8)		/* Set TTB */
	mcr	CP15_TLBIALL(r8)	/* Flush TLB */
	dsb	sy
	isb	sy

	mrc	CP15_TPIDRPRW(r0)
	ldr	r1, [r0, #CI_PL1_STKEND]
	mov	sp, r1

	adr	r1, .Lcpu_start_secondary
	ldr	pc, [r1]

	b	.

.Lcpu_hatch_virt:
	.word	cpu_hatch_virt
.Lcpu_start_secondary:
	.word	cpu_start_secondary
.Lcpu_hatch_ci:
	.word	cpu_hatch_ci
.Lcpu_hatch_ttb:
	.word	cpu_hatch_ttb
.Lcpu_hatch_mpidr:
	.word	cpu_hatch_mpidr

	.data
	.align	2
	.global cpu_hatch_ci
cpu_hatch_ci:
	.fill	MAXCPUS, 4, 0
	.global cpu_hatch_ttb
cpu_hatch_ttb:
	.word	0
	.global cpu_hatch_mpidr
cpu_hatch_mpidr:
	.fill	MAXCPUS, 4, 0xffffffff

#endif

	.bss
	.align	3
svcstk:
	.space	INIT_ARM_STACK_SIZE

	.text
	.align	2

#ifndef OFW
	/* OFW based systems will use OF_boot() */
	
.Lcpufuncs:	
	.word	cpufuncs

ENTRY_NP(cpu_reset)
	mrs     r2, cpsr
	bic	r2, r2, #(PSR_MODE)
	orr     r2, r2, #(PSR_SVC32_MODE)
	orr	r2, r2, #(PSR_I | PSR_F)
	msr     cpsr_c, r2

	ldr	r4, .Lcpu_reset_address
	ldr	r4, [r4]

	ldr	r0, .Lcpufuncs
	mov	lr, pc
	ldr	pc, [r0, #CF_IDCACHE_WBINV_ALL]

	/*
 	 * MMU & IDC off, 32 bit program & data space
	 * Hurl ourselves into the ROM
	 */
	mov	r0, #(CPU_CONTROL_32BP_ENABLE | CPU_CONTROL_32BD_ENABLE)
	mov	r2, #0
	mcr	CP15_SCTLR(r0)
	mcr	CP15_TLBIALL(r2) 	/* nail I+D TLB */
	mov	pc, r4

	/*
	 * _cpu_reset_address contains the address to branch to, to complete
	 * the cpu reset after turning the MMU off
	 * This variable is provided by the hardware specific code
	 */
.Lcpu_reset_address:
	.word	cpu_reset_address

#endif	/* OFW */

#ifdef DDB
/*
 * setjump + longjmp
 */
ENTRY(setjmp)
	stmia	r0, {r4-r14}
	mov	r0, #0x00000000
	mov	pc, lr

ENTRY(longjmp)
	ldmia	r0, {r4-r14}
	mov	r0, #0x00000001
	mov	pc, lr
#endif /* DDB */

	.data
	.align	2
	.global esym
esym:		.word	end

ENTRY_NP(abort)
	b	abort
