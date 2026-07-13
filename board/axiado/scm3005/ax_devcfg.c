// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2021-26 Axiado Corporation (or its affiliates).
 *
 * dev-cfg handoff parser.
 *
 */

#include <linux/types.h>
#include <linux/stddef.h>
#include <u-boot/crc.h>

/**
 * @brief Board type — identifies the physical product.
 */
typedef enum {
    AX_BOARD_TYPE_HOPEVALLEY_V3  = 0x486f706F,
    AX_BOARD_TYPE_SCM3005MT      = 0x6D543430,
} ax_board_type_t;

/**
 * @brief Image version
 *
 */
typedef struct ax_image_version_s {
    uint16_t major;   /* major version */
    uint16_t minor;   /* minor version */
    uint16_t patch;   /* patch version */
    uint16_t reserved;
} __attribute__((packed, aligned(4))) ax_image_version_t;

/**
 * @brief Target platform identifier.
 */
typedef enum {
    AX_TARGET_PLATFORM_EVB = 0,   /* EVB (standalone) */
    AX_TARGET_PLATFORM_HMC,       /* HMC */
    AX_TARGET_PLATFORM_BMC,       /* BMC */
} ax_target_platform_t;

/**
 * @brief Optional user register settings, applied at boot.
 *
 */
#define AX_BOARD_REG_SETTING_MAX     16U

typedef struct ax_reg_setting_s {
    uint32_t addr;    /* target register (must be in [BASE, END), dword-aligned) */
    uint32_t value;   /* 32-bit value to write                                  */
} __attribute__((packed, aligned(4))) ax_reg_setting_t;

/**
 * @brief Optional per-function power bitmaps, applied at post-init.
 */
#define AX_PWR_FUNC_MASK_WORDS  8U                          /* 256 bits per mask */
#define AX_PWR_FUNC_MASK_BITS   (AX_PWR_FUNC_MASK_WORDS * 32U)

typedef struct ax_pwr_func_cfg_s {
    uint32_t enable_mask[AX_PWR_FUNC_MASK_WORDS];  /* bit i set -> enable pwr_ctrl entry i  */
    uint32_t disable_mask[AX_PWR_FUNC_MASK_WORDS]; /* bit i set -> disable pwr_ctrl entry i */
} __attribute__((packed)) ax_pwr_func_cfg_t;

typedef struct ax_board_config_header_s {
    uint32_t magic;              /* AX_BOARD_CONFIG_MAGIC ("BCFG") */
    uint32_t crc32;              /* CRC32 from total_size to end of struct */
    uint32_t total_size;         /*  sizeof(ax_board_config_header_t) */
    uint32_t blob_version;       /* Version of the struct/tooling */
    /* --- IDENTITY SECTION ---
       Maps to Hardware P/N: [Prefix]-[Root][Ver]-[Suffix] [Rev]
       Example: 900-009A-00 Rev1
       It's duplicated with FRU. */
    uint32_t board_prefix;       /* HW "Prefix",   Ex: 900 */
    uint32_t board_id;           /* HW "Root",     Ex: 9  ("009" from "009A") */
    uint32_t board_version;      /* HW "Ver",      Ex: 1  ("A" from "009A", A=1, B=2) */
    uint32_t board_suffix;       /* HW "Suffix",   Ex: 0  ("00" from "009A-00") */
    uint32_t board_bom_rev;      /* HW "Revision", Ex: 1  ("Rev1") */
    ax_board_type_t  board_type;      /* Board type (ax_board_type_t) */
    char             serial[32];      /* Serial number (mirrors FRU product serial) */
    uint8_t          mac[6];          /* First MAC address */
    uint8_t          portmap;         /* Enabled ethernet port bitmap */
    uint8_t          num_mac;         /* Number of supported MACs */
    ax_image_version_t   image_ver;       /* Image version (major.minor.patch) */
    ax_target_platform_t target_platform; /* Target platform (ax_target_platform_t) */
    /*  --- USER REGISTER SETTINGS (optional, applied at boot) --- */
    uint32_t          reg_setting_count;                       /* number of valid entries (0..AX_BOARD_REG_SETTING_MAX) */
    ax_reg_setting_t  reg_setting[AX_BOARD_REG_SETTING_MAX];    /* addr/value pairs */
    // --- PER-FUNCTION POWER ENABLE BITMAP (optional, applied at post-init) --- */
    ax_pwr_func_cfg_t pwr_func;                                /* bit i set -> enable pwr_ctrl entry i */
    /* --- DDR INIT SPEED (optional) --- */
    uint32_t          ddr_speed;                               /* LPDDR5 init speed (MT/s); 0 = firmware default */
} __attribute__((packed, aligned(4))) ax_board_config_header_t;

/* Magic number stamped in every valid ax_board_config_header_t ("BCFG"). */
#define AX_BOARD_CONFIG_MAGIC   0x42434647U

/*
 * The generic-timer counter is clocked by CPU_CLOCK = CPU-PLL VCO /
 * ((POSTDIV0_2 + 1) * (POSTDIV1_2 + 1)). The control processor owns the PLL
 * and may change that divider via a dev-cfg register setting. The VCO is
 * fixed at 4000 MHz; only the post-divider changes.
 */
#define AX_DEV_CFG_HANDOFF_ADDR			0x800FF000UL /* must match ax_board_init.c */
#define AX_CPU_PLL_POSTDIV_DEVCFG_ADDR	0x40000008U  /* AX_BOARD_REG_SETTING_BASE + 0x08 */
#define AX3005_POSTDIV0_2_LSB			6U
#define AX3005_POSTDIV1_2_LSB			18U
#define AX_POSTDIV_MASK					0x7U
#define AX_CPU_PLL_VCO_HZ				4000000000ULL
#define AX_DEFAULT_COUNTER_FREQ			1000000000U  /* default CPU_CLOCK = 4000/4 */

/*
 * Derive the counter frequency from the dev-cfg blob left at AX_DEV_CFG_HANDOFF_ADDR.
 " Returns the default 1 GHz when the blob is absent or
 * invalid, or when it carries no CPU-PLL post-divider override.
 */
u32 ax_counter_freq_from_devcfg(void)
{
	const ax_board_config_header_t *hdr =
		(const ax_board_config_header_t *)AX_DEV_CFG_HANDOFF_ADDR;
	u32 len, crc, count, i;

	if (hdr->magic != AX_BOARD_CONFIG_MAGIC ||
	    hdr->total_size != sizeof(*hdr))
		return AX_DEFAULT_COUNTER_FREQ;

	/* CRC32 covers total_size..end (standard zlib CRC, == ax_bm_crc32). */
	len = hdr->total_size - offsetof(ax_board_config_header_t, total_size);
	crc = crc32(0, (const unsigned char *)&hdr->total_size, len);
	if (crc != hdr->crc32)
		return AX_DEFAULT_COUNTER_FREQ;

	count = hdr->reg_setting_count;
	if (count > AX_BOARD_REG_SETTING_MAX)
		count = AX_BOARD_REG_SETTING_MAX;

	for (i = 0; i < count; i++) {
		if (hdr->reg_setting[i].addr == AX_CPU_PLL_POSTDIV_DEVCFG_ADDR) {
			u32 val = hdr->reg_setting[i].value;
			u32 d0 = (val >> AX3005_POSTDIV0_2_LSB) & AX_POSTDIV_MASK;
			u32 d1 = (val >> AX3005_POSTDIV1_2_LSB) & AX_POSTDIV_MASK;

			return (u32)(AX_CPU_PLL_VCO_HZ / ((u64)(d0 + 1) * (d1 + 1)));
		}
	}

	return AX_DEFAULT_COUNTER_FREQ;
}
