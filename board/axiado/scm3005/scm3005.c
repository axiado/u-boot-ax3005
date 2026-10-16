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
#include <env_internal.h>
#include <command.h>
#include <net.h>
#include <i2c.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/system.h>

/* Defined in ax_devcfg.c */
u32 ax_counter_freq_from_devcfg(void);

static int get_uart_index_from_fru(void);

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
 * uart4_is_console - does the FRU EEPROM select UART4 as console?
 *
 */
static bool uart4_is_console(void)
{
	return get_uart_index_from_fru() == 4;
}

/*
 * ft_board_setup - fix up the devicetree handed to the OS
 *
 * arch_fixup_fdt() / spin_table_update_dt() overwrites cpu-release-addr
 * with U-Boot's relocated address.  Restore the pre-relocation physical
 * address so secondary cores spin on the correct location.  Also override
 * the arch-timer rate and, when dev-cfg selects UART4, point the console
 * at it.
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

	/*
	 * The console UART is selected at runtime from FRU data saved on EEPROM
	 * blob, so hand the kernel a matching devicetree: disable uart3 so no
	 * ttyPS3 is registered, and repoint stdout-path, which is what a bare
	 * "earlycon" resolves through (console= only takes effect once the
	 * real driver probes). The blob has CONFIG_SYS_FDT_PAD headroom here,
	 * so growing "okay" to "disabled" is safe.
	 */
	if (uart4_is_console()) {
		offset = fdt_path_offset(blob, "serial3");	/* uses the alias */
		if (offset >= 0) {
			ret = fdt_setprop_string(blob, offset, "status",
						 "disabled");
			if (ret)
				printf("WARNING: Failed to disable uart3 (%d)\n",
				       ret);
		}

		offset = fdt_path_offset(blob, "/chosen");
		if (offset >= 0) {
			ret = fdt_setprop_string(blob, offset, "stdout-path",
						 "serial4:115200");
			if (ret)
				printf("WARNING: Failed to set stdout-path (%d)\n",
				       ret);
		}
	} else {
		offset = fdt_path_offset(blob, "serial4");	/* uses the alias */
		if (offset >= 0) {
			ret = fdt_setprop_string(blob, offset, "status",
						 "disabled");
			if (ret)
				printf("WARNING: Failed to disable uart4 (%d)\n",
				       ret);
		}
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
 * board_cntfrq_init - program CNTFRQ_EL0 from the dev-cfg-derived counter
 * clock on every CPU.
 *
 * Called from the SoC's boot0.h hook while each CPU is still at EL3, before
 * secondary CPUs switch to EL2 and enter the spin-table wait. timer_init()
 * alone is not enough here: it only runs on the boot CPU, so CPU1-3 would
 * keep CNTFRQ_EL0 == 0 and Linux's per-CPU SYS_CNTFRQ_EL0 sanity check would
 * trip (TAINT_CPU_OUT_OF_SPEC).
 */
void board_cntfrq_init(void)
{
	raw_write_cntfrq_el0(ax_counter_freq_from_devcfg());
}

/*
 * timer_init - enable the AX3005 platform system timer.
 * SYS_TIMER_CTRL (0x48016000) is the AX3005 system timer control register —
 * writing SYS_TIMER_ENABLE starts the counter that feeds the ARM generic timer.
 */
int timer_init(void)
{
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
#define ACTION_NONE 0 /* no action*/
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
	pad_val.bits.boot_stage = BS_UBOOT;
	pad_val.bits.counter += 1;
	writel(pad_val.raw, AX3005_SCRATCHPAD_ADDR);
}

/* BMC FRU EEPROM (24c16) on I2C10 */
#define FRU_I2C_BUS		10
#define FRU_I2C_ADDR		0x50
#define FRU_I2C_OFFSET_LEN	1	/* 1-byte word address */
#define FRU_I2C_ADDR_MASK	0x7	/* offset bits 10:8 go in the chip address */
#define FRU_I2C_BLOCK_SIZE	256	/* bytes per chip address */
#define FRU_EEPROM_SIZE		2048
#define FRU_HDR_LEN		8	/* IPMI FRU common header */
#define FRU_HDR_VERSION		0x01
#define FRU_UART_INDEX_OFFSET		0x18	/* u8 */
#define FRU_MAC_ADDR_OFFSET		0x19
#define FRU_PORTMAP_OFFSET		0x1E	/* u8, bit 0-3 -> MAC-1..4, bit 4 -> MAC-0 */
#define AX_PORTMAP_MASK			GENMASK(4, 0)	/* MAC-1..MAC-4, MAC-0 */
#define AX_PORTMAP_MAC0_BIT		4	/* bit 4 -> MAC-0 */
#define AX_DEFAULT_MACID		1	/* MAC port #1 */

/**
 * @brief read FRU data from the EEPROM on I2C10
 *
 * The 24c16 answers on 0x50-0x57, one address per 256-byte block, so the
 * read is split at block boundaries.
 *
 * @param offset byte offset in the EEPROM
 * @param buf    destination buffer
 * @param len    number of bytes to read
 *
 * @return 0 on success, negative errno on failure
 */
static int scm3005_get_fru_data(uint offset, u8 *buf, uint len)
{
	struct udevice *dev;
	uint chunk;
	int ret;

	if (offset >= FRU_EEPROM_SIZE || len > FRU_EEPROM_SIZE - offset)
		return -EINVAL;

	ret = i2c_get_chip_for_busnum(FRU_I2C_BUS, FRU_I2C_ADDR,
				      FRU_I2C_OFFSET_LEN, &dev);
	if (ret)
		return ret;

	ret = i2c_set_chip_addr_offset_mask(dev, FRU_I2C_ADDR_MASK);
	if (ret)
		return ret;

	while (len) {
		chunk = min(len, FRU_I2C_BLOCK_SIZE - offset % FRU_I2C_BLOCK_SIZE);
		ret = dm_i2c_read(dev, offset, buf, chunk);
		if (ret)
			return ret;
		offset += chunk;
		buf += chunk;
		len -= chunk;
	}

	return 0;
}

/**
 * @brief get the console UART index stored in the FRU EEPROM
 *
 * @param void
 *
 * @return UART index on success, or a negative errno if the read fails
 */
static int get_uart_index_from_fru(void)
{
	u8 index;
	int ret;

	ret = scm3005_get_fru_data(FRU_UART_INDEX_OFFSET, &index, 1);
	if (ret) {
		printf("FRU: failed to read UART index (%d)\n", ret);
		return ret;
	}

	return index;
}

/**
 * @brief get the first MAC address stored in the FRU EEPROM
 *
 * @param mac destination for the ARP_HLEN-byte MAC address
 *
 * @return 0 on success, -EINVAL if the stored address isn't a valid
 *         unicast MAC, or another negative errno if the read fails
 */
static int get_mac_address_from_fru(u8 *mac)
{
	int ret;

	ret = scm3005_get_fru_data(FRU_MAC_ADDR_OFFSET, mac, ARP_HLEN);
	if (ret) {
		printf("FRU: failed to read MAC address (%d)\n", ret);
		return ret;
	}

	if (!is_valid_ethaddr(mac)) {
		printf("FRU: invalid MAC address %pM\n", mac);
		return -EINVAL;
	}

	return 0;
}

/**
 * @brief get the MAC port id (macid) from the FRU port map
 *
 * Port map bits: bit 0 -> MAC-1, bit 1 -> MAC-2, bit 2 -> MAC-3,
 * bit 3 -> MAC-4, bit 4 -> MAC-0.
 * If more than one port is enabled, the lowest bit is used, so MAC-1..4
 * take priority over MAC-0.
 *
 * @param void
 *
 * @return macid (0-4) on success, -ENOENT if no port bit is set, or
 *         another negative errno if the read fails
 */
static int get_macid_from_fru(void)
{
	u8 portmap, ports;
	int bit, ret;

	ret = scm3005_get_fru_data(FRU_PORTMAP_OFFSET, &portmap, 1);
	if (ret) {
		printf("FRU: failed to read port map (%d)\n", ret);
		return ret;
	}

	ports = portmap & AX_PORTMAP_MASK;
	if (!ports) {
		printf("FRU: no MAC port enabled in port map 0x%02x\n",
		       portmap);
		return -ENOENT;
	}

	bit = ffs(ports) - 1;
	if (bit == AX_PORTMAP_MAC0_BIT)
		return 0;

	return bit + 1;
}

/*
 * board_late_init - runs at the end of U-Boot init, just before the
 * autoboot/console prompt. Stamp the scratchpad to indicate U-Boot has
 * booted (boot_stage = BS_UBOOT) and bump the liveness counter.
 * Requires CONFIG_BOARD_LATE_INIT=y in the board defconfig.
 */
int board_late_init(void)
{
	u8 mac[ARP_HLEN];
	int ret;

	set_pad_in_uboot_state();
	/* defaults in use => the eMMC copy was unusable (bad CRC or read error) */
	if (gd->flags & GD_FLG_ENV_DEFAULT) {
		ret = env_save();
		if (ret)
			printf("WARNING: Failed to write default environment (%d)\n",
			       ret);
	}

	/* console=ttyPS3 --> console=ttyPS4 when dev-cfg selects UART4 */
	if (uart4_is_console())
		env_set("bootargs", AX_BOOTARGS_UART4);

	printf("FRU UART Index: %d\n", get_uart_index_from_fru());

	ret = get_mac_address_from_fru(mac);
	if (!ret)
		printf("FRU MAC Addr: %pM\n", mac);

	/* Runs before initr_net(), so the Ethernet driver picks this up */
	if (!ret && !env_get("ethaddr"))
		eth_env_set_enetaddr("ethaddr", mac);

	/* axiado-eth needs macid (0-4) to know which MAC port to program */
	if (!env_get("macid")) {
		ret = get_macid_from_fru();
		if (ret < 0) {
			printf("FRU: using default macid %d\n", AX_DEFAULT_MACID);
			ret = AX_DEFAULT_MACID;
		}
		env_set_ulong("macid", ret);
	}

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
	pad_val.bits.boot_stage = BS_KERNEL;
	pad_val.bits.timeout = TIMEOUT_60_SECONDS;
	pad_val.bits.counter += 1;
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

int board_early_init_f(void)
{
	void *blob = (void *)gd->fdt_blob;
	const char *path = "serial4:115200";
	int chosen;

	/*
	 * This runs before relocate_code(), so nothing here may write a global
	 * or static variable: .bss is linked over .rela.dyn (both start at
	 * __rel_dyn_start), so such a store lands on a relocation entry and
	 * wedges relocate_code() before board_init_r() can report anything.
	 * Keep the result local; post-relocation consumers must re-derive it.
	 */
	if (uart4_is_console()) {
		fdt_open_into(blob, blob, fdt_totalsize(blob) + 64); /* headroom for the string */
		chosen = fdt_path_offset(blob, "/chosen");
		fdt_setprop_string(blob, chosen, "stdout-path", path);
	}

	return 0;
}
