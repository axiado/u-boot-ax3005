// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2021-2026 Axiado Corporation (or its affiliates).
 */

#include <config.h>
#include <dm.h>
#include <init.h>
#include <asm/global_data.h>
#include <asm/armv8/mmu.h>
#include <asm/io.h>
#include <asm/spin_table.h>
#include <asm/system.h>
#include <fdt_support.h>
#include <env.h>
#include <command.h>
#include <net.h>
#include <linux/types.h>
#include <asm/system.h>

/* Defined in ax_devcfg.c */
u32 ax_counter_freq_from_devcfg(void);

DECLARE_GLOBAL_DATA_PTR;

static struct mm_region axiado_ax3005_mem_map[] = {
	{ /* Peripherals including UART */
	  .virt = 0x00000000UL,
	  .phys = 0x00000000UL,
	  .size = 0x4A000000UL, /* 0 to 0x4A000000: peripherals */
	  .attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) | PTE_BLOCK_NON_SHARE |
		   PTE_BLOCK_PXN | PTE_BLOCK_UXN },
	{ .virt = 0x80000000UL,
	  .phys = 0x80000000UL,
	  .size = 0x80000000UL,
	  .attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_INNER_SHARE },
	{
		0,
	}
};

struct mm_region *mem_map = axiado_ax3005_mem_map;

int board_fit_config_name_match(const char *name)
{
	return 0;
}

/*
 * ft_board_setup - restore cpu-release-addr after relocation
 *
 * arch_fixup_fdt() / spin_table_update_dt() overwrites cpu-release-addr
 * with U-Boot's relocated address.  Restore the pre-relocation physical
 * address so secondary cores spin on the correct location.
 */
int ft_board_setup(void *blob, struct bd_info *bd)
{
	int cpus_offset, offset;
	const char *prop;
	int ret;
	u64 cpu_release_addr = (u64)&spin_table_cpu_release_addr - gd->reloc_off;

	cpus_offset = fdt_path_offset(blob, "/cpus");
	if (cpus_offset < 0)
		return 0;

	for (offset = fdt_first_subnode(blob, cpus_offset); offset >= 0;
	     offset = fdt_next_subnode(blob, offset)) {
		prop = fdt_getprop(blob, offset, "device_type", NULL);
		if (!prop || strcmp(prop, "cpu"))
			continue;

		prop = fdt_getprop(blob, offset, "enable-method", NULL);
		if (!prop || strcmp(prop, "spin-table"))
			continue;

		ret = fdt_setprop_u64(blob, offset, "cpu-release-addr",
				      cpu_release_addr);
		if (ret) {
			printf("WARNING: Failed to restore cpu-release-addr\n");
			return ret;
		}
	}

	/*
	 * The arch-timer DT node carries a "clock-frequency" property, which Linux
	 * honors in preference to CNTFRQ_EL0. It may be re-clocked via a dev-cfg
	 * PLL divider, so override it with the actual rate or the kernel's
	 * timekeeping/delays will be wrong.
	 */
	offset = fdt_node_offset_by_compatible(blob, -1, "arm,armv8-timer");
	if (offset >= 0) {
		u32 freq = ax_counter_freq_from_devcfg();

		ret = fdt_setprop_u32(blob, offset, "clock-frequency", freq);
		if (ret)
			printf("WARNING: Failed to set timer clock-frequency\n");
		else
			printf("fdt board setup: timer clock-frequency = %u Hz\n", freq);
	}

	return 0;
}

int dram_init(void)
{
	gd->ram_size = get_ram_size((void *)CFG_SYS_SDRAM_BASE,
				    CFG_SYS_SDRAM_SIZE);
	return 0;
}

int dram_init_banksize(void)
{
	gd->bd->bi_dram[0].start = CFG_SYS_SDRAM_BASE;
	gd->bd->bi_dram[0].size = CFG_SYS_SDRAM_SIZE;
	return 0;
}

void raw_write_cntfrq_el0(u32 cntfrq_el0)
{
	__asm__ __volatile__("msr CNTFRQ_EL0, %0\n\t" : : "r" (cntfrq_el0) : "memory");
}

static u32 raw_read_cntfrq_el0(void)
{
	u32 cntfrq_el0;

	__asm__ __volatile__("mrs %0, CNTFRQ_EL0\n\t" : "=r" (cntfrq_el0) : : "memory");
	return cntfrq_el0;
}

/*
 * timer_init - program CNTFRQ_EL0 from the live counter clock and enable
 * the AX3005 platform system timer.
 * SYS_TIMER_CTRL (0x48016000) is the AX3005 system timer control register —
 * writing SYS_TIMER_ENABLE starts the counter that feeds the ARM generic timer.
 */
int timer_init(void)
{
	u32 freq = ax_counter_freq_from_devcfg();

	if (current_el() == 3) {
		raw_write_cntfrq_el0(freq);
	}

	writel(SYS_TIMER_ENABLE, (uintptr_t)SYS_TIMER_CTRL);
	return 0;
}

int board_init(void)
{
	/* Console is up by now (console_init_f ran in board_f), so this is a
	 * visible confirmation of the timer handoff: the dev-cfg-derived rate vs.
	 * what CNTFRQ_EL0 actually holds (the latter only updates if timer_init
	 * ran at EL3). timer_init itself runs pre-console, so it can't print. */
	printf("board_init: dev-cfg counter freq = %u Hz, CNTFRQ_EL0 = %u Hz (EL%u)\n",
	       ax_counter_freq_from_devcfg(), raw_read_cntfrq_el0(), current_el());
	return 0;
}

void reset_cpu(void)
{
}

#define AX3005_SCRATCHPAD_ADDR 0x33120804 /* AX3005_CSR_BASE_ADRS_IOCTL_1 */

#define WATCHDOG_ENABLE 1
#define WATCHDOG_DISABLE 0

/* action settings */
#define ACTION_NC 0 /* no action*/
#define ACTION_RESET_CPU 1 /* reset CPU */
#define ACTION_RESET_SOC 2 /* reset SOC */
#define ACTION_BOOT_ALTE_LOT 3 /*  reset and boot from alternative lot */
#define ACTION_RESET_GOLDEN 4 /* reset with golden image */

/* boot stages */
#define BS_PRE_LOAD 0 /* pre-load */
#define BS_UBOOT 1 /* u-boot */
#define BS_KERNEL 2 /* kernel */
#define BS_INIT_RAM_FS 3 /* initramfs */
#define BS_ROOT_FS 4 /* rootfs */

#define TIMEOUT_NC 0
#define TIMEOUT_60_SECONDS 60

typedef union {
	u32 raw; // access all 32 bits at once
	struct {
		u32 counter : 16;
		u32 timeout : 8;
		u32 boot_stage : 3;
		u32 action : 4;
		u32 enable : 1;
	} bits;
} scratch_pat_t;

/**
 * @brief set PAD register value at u-boot prompt
 *
 * @param void
 *
 * @return None
 */
void set_pad_in_uboot_state(void)
{
	scratch_pat_t pad_val;
	pad_val.raw = readl(AX3005_SCRATCHPAD_ADDR);
	pad_val.bits.enable = WATCHDOG_DISABLE;
	pad_val.bits.action = ACTION_NC;
	pad_val.bits.boot_stage = BS_UBOOT;
	pad_val.bits.timeout = TIMEOUT_NC;
	pad_val.bits.counter += 1;
	printf("At u-boot: write the value : 0x%x to PAD Register: 0x%x\n",
	       pad_val.raw, AX3005_SCRATCHPAD_ADDR);
	writel(pad_val.raw, AX3005_SCRATCHPAD_ADDR);
}

/*
 * board_late_init - runs at the end of U-Boot init, just before the
 * autoboot/console prompt. Stamp the scratchpad to indicate U-Boot has
 * booted (boot_stage = BS_UBOOT) and bump the liveness counter.
 * Requires CONFIG_BOARD_LATE_INIT=y in the board defconfig.
 */
int board_late_init(void)
{
	set_pad_in_uboot_state();
	return 0;
}

/**
 * @brief set PAD register value before jumping to kernel
 *
 * @param void
 *
 * @return none
 */
void set_pad_before_kernel_state(void)
{
	scratch_pat_t pad_val;
	pad_val.raw = readl(AX3005_SCRATCHPAD_ADDR);
	pad_val.bits.enable = WATCHDOG_ENABLE;
	pad_val.bits.action = ACTION_NC;
	pad_val.bits.boot_stage = BS_KERNEL;
	pad_val.bits.timeout = TIMEOUT_60_SECONDS;
	pad_val.bits.counter += 1;
	printf("Before jumping to kernel: write the value : 0x%x to PAD Register: 0x%x\n",
	       pad_val.raw, AX3005_SCRATCHPAD_ADDR);
	writel(pad_val.raw, AX3005_SCRATCHPAD_ADDR);
}

/*
 * board_preboot_os - board hook called from boot_selected_os(), the last
 * board-level callback before U-Boot jumps into the OS. Covers all boot
 * paths (bootm/booti/bootz). Arm the watchdog and mark boot_stage = BS_KERNEL
 * so the monitor takes over if the kernel fails to hand off in time.
 */
void board_preboot_os(void)
{
	set_pad_before_kernel_state();
}