/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2021-2026 Axiado Corporation (or its affiliates).
 *
 * Each core is released directly into this _start entry point
 * without setting up a stack pointer, and the ARM generic timer counter
 * frequency isn't a build-time constant on this platform (it can be
 * re-clocked via a dev-cfg PLL divider). Both need handling before any
 * other code runs: board_cntfrq_init() needs a real C stack frame, and
 * CNTFRQ_EL0 must be programmed on every CPU while still at its highest
 * EL, before secondary CPUs drop to EL2/EL1 and enter the spin-table wait
 * in the normal _start flow below (timer_init() alone only runs on the
 * boot CPU, far too late for CPU1-3).
 */

.equ AXIADO_BOOT0_STACK_SHIFT, 9	/* 512 bytes per CPU */

	/*
	 * Preserve any boot arguments the previous stage passed in x0-x3
	 * (e.g. save_boot_params() may consume x0 as an FDT/atags pointer)
	 * across the sequence below.
	 */
	mov	x4, x0
	mov	x5, x1
	mov	x6, x2
	mov	x7, x3

	mrs	x0, mpidr_el1
	and	x0, x0, #0xff			/* Aff0 = core number */
	lsl	x0, x0, #AXIADO_BOOT0_STACK_SHIFT
	adr	x1, axiado_boot0_stack_top
	sub	sp, x1, x0

	branch_if_not_highest_el x0, 1f
	bl	board_cntfrq_init
1:
	mov	x0, x4
	mov	x1, x5
	mov	x2, x6
	mov	x3, x7
	b	reset

.align 4
axiado_boot0_stack:
	.space	(4 << AXIADO_BOOT0_STACK_SHIFT)
axiado_boot0_stack_top:
