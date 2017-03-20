/*
 * Copyright (C) 2016 Marvell
 *
 * Antoine Tenart <antoine.tenart@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/workqueue.h>

#include "safexcel.h"

/* eip_in_use holds the active engine id */
static int eip_in_use = -1;

/* Module param to save the assigned rings to the Kernel */
static uint rings[MAX_EIP_ENGINE] = {RINGS_UNINITIALIZED, RINGS_UNINITIALIZED};

static void eip197_prng_init(struct safexcel_crypto_priv *priv)
{
	/* disable PRNG and set to manual mode */
	writel(0, EIP197_PE(priv) + EIP197_PE_EIP96_PRNG_CTRL);

	/* Write seed data */
	writel(EIP197_PE_EIP96_PRNG_SEED_L_VAL,
	       EIP197_PE(priv) + EIP197_PE_EIP96_PRNG_SEED_L);
	writel(EIP197_PE_EIP96_PRNG_SEED_H_VAL,
	       EIP197_PE(priv) + EIP197_PE_EIP96_PRNG_SEED_H);

	/* Write key data */
	writel(EIP197_PE_EIP96_PRNG_KEY_0_L_VAL,
	       EIP197_PE(priv) + EIP197_PE_EIP96_PRNG_KEY_0_L);
	writel(EIP197_PE_EIP96_PRNG_KEY_0_H_VAL,
	       EIP197_PE(priv) + EIP197_PE_EIP96_PRNG_KEY_0_H);
	writel(EIP197_PE_EIP96_PRNG_KEY_1_L_VAL,
	       EIP197_PE(priv) + EIP197_PE_EIP96_PRNG_KEY_1_L);
	writel(EIP197_PE_EIP96_PRNG_KEY_1_H_VAL,
	       EIP197_PE(priv) + EIP197_PE_EIP96_PRNG_KEY_1_H);

	/* Write LFSR data */
	writel(EIP197_PE_EIP96_PRNG_LFSR_L_VAL,
	       EIP197_PE(priv) + EIP197_PE_EIP96_PRNG_LFSR_L);
	writel(EIP197_PE_EIP96_PRNG_LFSR_H_VAL,
	       EIP197_PE(priv) + EIP197_PE_EIP96_PRNG_LFSR_H);

	/* enable PRNG and set to auto mode */
	writel(EIP197_PE_EIP96_PRNG_EN | EIP197_PE_EIP96_PRNG_AUTO,
	       EIP197_PE(priv) + EIP197_PE_EIP96_PRNG_CTRL);
}

static int eip197_trc_cache_init(struct device *dev,
				 struct safexcel_crypto_priv *priv)
{
	u32 i, reg, reg_addr,
		rc_rec_wc,
		rc_rec1_wc,
		rc_rec2_wc,
		rc_record_cnt,
		rc_ht_wc,
		rc_ht_byte_offset,
		rc_ht_sz,
		rc_ht_factor,
		rc_ht_entry_cnt,
		rc_admn_ram_wc,
		rc_admn_ram_entry_cnt;

	rc_rec1_wc = EIP197_CS_TRC_REC_WC;
	rc_rec2_wc = EIP197_CS_TRC_LG_REC_WC;

	/* Determine the RC record size to use */
	if (rc_rec2_wc > rc_rec1_wc)
		rc_rec_wc = rc_rec2_wc;
	else
		rc_rec_wc = rc_rec1_wc;

	/* Calculate the maximum possible record count that
	 * the Record Cache Data RAM can contain
	 */
	rc_record_cnt = EIP197_TRC_RAM_WC / rc_rec_wc;

	/* rc_record_cnt is calculated using the configured RC Data RAM size. */

	/* rc_admn_ram_entry_cnt is calculated using
	 * the configured RC Admin RAM size.
	 */

	/* set the configured RC Admin RAM size */
	rc_admn_ram_wc = EIP197_TRC_ADMIN_RAM_WC;

	/* Calculate the maximum possible record count that
	 * the RC Hash Table (in Record Cache Administration RAM) can contain
	 */
	rc_admn_ram_entry_cnt = EIP197_RC_ADMN_MEMWORD_ENTRY_CNT * rc_admn_ram_wc /
				(EIP197_RC_ADMN_MEMWORD_WC +
				EIP197_RC_ADMN_MEMWORD_ENTRY_CNT *
				EIP197_RC_HEADER_WC);

	/* Try to extend the Hash Table in the RC Admin RAM */
	if (rc_record_cnt < rc_admn_ram_entry_cnt) {
		unsigned int ht_space_wc;

		/* Calculate the size of space available for the Hash Table */
		ht_space_wc = rc_admn_ram_wc -
			rc_record_cnt * EIP197_RC_HEADER_WC;

		/* Calculate maximum possible Hash Table entry count */
		rc_ht_entry_cnt = (ht_space_wc / EIP197_RC_ADMN_MEMWORD_WC) *
			EIP197_RC_ADMN_MEMWORD_ENTRY_CNT;
	} else {
		/* Extension impossible */
		rc_ht_entry_cnt = rc_admn_ram_entry_cnt;
	}

	/* Check minimum number of entries in the record cache */
	rc_ht_entry_cnt = max_t(u32, EIP197_RC_MIN_ENTRY_CNT,
				rc_ht_entry_cnt);

	/* Check maximum number of entries in the record cache */
	rc_ht_entry_cnt = min_t(u32, EIP197_RC_MAX_ENTRY_CNT,
				rc_ht_entry_cnt);

	/* calc power factor */
	rc_ht_factor = fls(rc_ht_entry_cnt) - 1;

	/* Round down to power of two */
	rc_ht_entry_cnt = 1 << rc_ht_factor;

	/* Hash Table Mask that determines the hash table size */
	if (rc_ht_factor >= EIP197_RC_HASH_TABLE_SIZE_POWER_FACTOR) {
		rc_ht_sz = rc_ht_factor -
			EIP197_RC_HASH_TABLE_SIZE_POWER_FACTOR;
	} else {
		/* Insufficient memory for Hash Table in the RC Admin RAM */
		dev_dbg(priv->dev, "Insufficient memory for HT\n");
		return -EINVAL;
	}

	/* Calculate the Hash Table size in 32-bit words */
	rc_ht_wc = rc_ht_entry_cnt / EIP197_RC_ADMN_MEMWORD_ENTRY_CNT *
		   EIP197_RC_ADMN_MEMWORD_WC;

	/* Recalculate the record count that fits the RC Admin RAM space
	 * without the Hash Table, restricting for the maximum records which
	 * fit the RC Data RAM Adjusted record count which fits the RC Admin
	 * RAM
	 */

	/* Maximum record count which fits the RC Data RAM - rc_record_cnt
	 * use the minimum of the two
	 */
	rc_record_cnt = min(rc_record_cnt, (rc_admn_ram_wc - rc_ht_wc) /
			    EIP197_RC_HEADER_WC);

	/* enable record cache RAM access */
	reg = ~EIP197_TRC_ENABLE_MASK & readl(priv->base + EIP197_CS_RAM_CTRL);
	reg |= EIP197_TRC_ENABLE(0);
	writel(reg, priv->base + EIP197_CS_RAM_CTRL);

	/* Clear all ECC errors */
	writel(0, priv->base + EIP197_TRC_ECCCTRL);

	/* Take Record Cache into reset
	 * Make cache administration RAM accessible
	 */
	writel(EIP197_TRC_SW_RESET, priv->base + EIP197_TRC_PARAMS);

	/* Clear all record administration words in Record Cache
	 * administration RAM
	 */
	for (i = 0; i < rc_record_cnt; i++) {
		/* Calculate byte offset for the current record */
		reg_addr = EIP197_CLASSIF_RAM_ACCESS_SPACE + i * EIP197_RC_HEADER_WC * sizeof(u32);

		/* Write word 0 */
		writel((EIP197_RC_NULL << 20) |		/* Hash_Collision_Prev */
			 (EIP197_RC_NULL << 10),	/* Hash_Collision_Next */
			 priv->base + reg_addr);

		/* Write word 1 */
		reg_addr += sizeof(u32);

		if (i == rc_record_cnt - 1) {
			/* last record */
			writel(((i - 1) << 10) |	/* Free_List_Prev */
				 EIP197_RC_NULL,	/* Free_List_Prev */
				 priv->base + reg_addr);
		} else if (!i) {
			/* first record */
			writel((EIP197_RC_NULL << 10) |	/* Free_List_Prev */
			       (i + 1),			/* Free_List_Prev */
			       priv->base + reg_addr);
		} else {
			/* All other records */
			writel(((i - 1) << 10) |	/* Free_List_Prev */
			       (i + 1),			/* Free_List_Prev */
			       priv->base + reg_addr);
		}

		/* Write word 2 */
		reg_addr += sizeof(u32);
		writel(0, priv->base + reg_addr);	/* Address_Key, low bits */

		/* Write word 3 */
		reg_addr += sizeof(u32);
		writel(0, priv->base + reg_addr);	/* Address_Key, high bits */
	}

	/* Calculate byte offset for hash table */
	rc_ht_byte_offset = EIP197_CLASSIF_RAM_ACCESS_SPACE +
			    rc_record_cnt * EIP197_RC_HEADER_WC * sizeof(u32);

	/* Clear all hash table words */
	for (i = 0; i < rc_ht_wc; i++)
		writel(GENMASK(29, 0),
		       priv->base + rc_ht_byte_offset + i * sizeof(u32));

	/* Disable Record Cache RAM access */
	writel(0, priv->base + EIP197_CS_RAM_CTRL);

	/* Write head and tail pointers to the RC Free Chain */
	writel(((rc_record_cnt - 1) & GENMASK(9, 0)) << 16,
		 priv->base + EIP197_TRC_FREECHAIN);

	/* Set Hash Table start */
	reg = ((EIP197_CS_TRC_REC_WC << 18) |
		(EIP197_RC_DMA_WR_COMB_DLY << 10) |
		(rc_record_cnt & GENMASK(9, 0)));
	writel(reg, priv->base + EIP197_TRC_PARAMS2);

	/* Select the highest clock count as specified by
	 * the Host and the Firmware for the FRC
	 */

	/* Take Record Cache out of reset */
	reg = ((rc_rec2_wc & GENMASK(8, 0)) << 18)	| /* large record_size */
	       (1 << 10)				| /* block_timebase */
	       ((rc_ht_sz & GENMASK(2, 0)) << 4);	  /* hash_table_size */
	writel(reg, priv->base + EIP197_TRC_PARAMS);

	return 0;
}

static int eip197_load_fw(struct device *dev, struct safexcel_crypto_priv *priv)
{
	const struct firmware	*fw[MAX_FW_NR] = {0};
	const u32		*fw_data;
	int			i, ret;
	u32			fw_size, reg;
	const char		*fw_name[MAX_FW_NR] = {"ifpp.bin", "ipue.bin"};

	for (i = 0; i < MAX_FW_NR; i++) {
		ret = request_firmware(&fw[i], fw_name[i], dev);
		if (ret) {
			dev_err(dev, "request_firmware failed (fw: %s)\n",
				fw_name[i]);
			goto release_fw;
		}
	 }

	/* Clear EIP-207c ICE Scratchpad RAM where the firmware */
	reg = (EIP197_PE_ICE_SCRATCH_CTRL_DFLT				|
	       EIP197_PE_ICE_SCRATCH_CTRL_CHANGE_TIMER			|
	       EIP197_PE_ICE_SCRATCH_CTRL_TIMER_EN			|
	       ((0x1 << EIP197_PE_ICE_SCRATCH_CTRL_SCRATCH_ACCESS_OFFSET) &
		EIP197_PE_ICE_SCRATCH_CTRL_SCRATCH_ACCESS_MASK)		|
	       EIP197_PE_ICE_SCRATCH_CTRL_CHANGE_ACCESS);
	writel(reg, EIP197_PE(priv) + EIP197_PE_ICE_SCRATCH_CTRL_OFFSET);

	/* Write the ICE Scratchpad RAM with 0 */
	for (i = 0; i < EIP197_NUM_OF_SCRATCH_BLOCKS; i++)
		writel(0, EIP197_PE(priv) + EIP197_PE_ICE_SCRATCH_RAM(i));

	/* Reset the Input Flow Post-Processor micro-engine (IFPP) to make its
	 * program RAM accessible.
	 */
	reg = (EIP197_PE_ICE_FPP_CTRL_SW_RESET			|
	       EIP197_PE_ICE_FPP_CTRL_CLR_ECC_CORR		|
	       EIP197_PE_ICE_FPP_CTRL_CLR_ECC_NON_CORR);
	writel(reg, EIP197_PE(priv) + EIP197_PE_ICE_FPP_CTRL);

	/* Enable access to IFPP Program RAM */
	reg = (EIP197_PE_ICE_RAM_CTRL_DFLT |
	       EIP197_PE_ICE_RAM_CTRL_FPP_PROG_EN);
	writel(reg, EIP197_PE(priv) + EIP197_PE_ICE_RAM_CTRL);

	/* Save pointer to the data and the size of the data */
	fw_data = (const u32 *)fw[IFPP_FW]->data;
	fw_size = fw[IFPP_FW]->size / sizeof(u32);

	/* Write the Input Flow post-Processor micro-Engine firmware */
	for (i = 0; i < fw_size; i++)
		writel(be32_to_cpu(*(fw_data + i)),
		       priv->base + EIP197_CLASSIF_RAM_ACCESS_SPACE + (i * 4));

	/* Disable access to IFPP Program RAM
	 * Enable access to IPUE Program RAM
	 */
	reg = (EIP197_PE_ICE_RAM_CTRL_DFLT |
	       EIP197_PE_ICE_RAM_CTRL_PUE_PROG_EN);
	writel(reg, EIP197_PE(priv) + EIP197_PE_ICE_RAM_CTRL);

	/* Reset the Input Pull-Up micro-Engine (IPUE) to make its
	 * program RAM accessible.
	 */
	reg = (EIP197_PE_ICE_PUE_CTRL_SW_RESET		|
	       EIP197_PE_ICE_PUE_CTRL_CLR_ECC_CORR	|
	       EIP197_PE_ICE_PUE_CTRL_CLR_ECC_NON_CORR);
	writel(reg, EIP197_PE(priv) + EIP197_PE_ICE_PUE_CTRL);

	/* Save pointer to the data and the size of the data */
	fw_data = (const u32 *)fw[IPUE_FW]->data;
	fw_size = fw[IPUE_FW]->size / sizeof(u32);

	/* Write the Input Flow post-Processor micro-Engine firmware */
	for (i = 0; i < fw_size; i++)
		writel(be32_to_cpu(*(fw_data + i)), EIP197_RAM(priv) + (i * 4));

	/* Disable access to IPUE Program RAM */
	reg = EIP197_PE_ICE_RAM_CTRL_DFLT;
	writel(reg, EIP197_PE(priv) + EIP197_PE_ICE_RAM_CTRL);

	/* Release IFPP from reset */
	reg = readl(priv->base + EIP197_PE_ICE_PUE_CTRL);
	reg &= ~EIP197_PE_ICE_FPP_CTRL_SW_RESET;
	writel(reg, EIP197_PE(priv) + EIP197_PE_ICE_FPP_CTRL);

	/* Release IPUE from reset */
	reg = readl(priv->base + EIP197_PE_ICE_PUE_CTRL);
	reg &= ~EIP197_PE_ICE_PUE_CTRL_SW_RESET;
	writel(reg, EIP197_PE(priv) + EIP197_PE_ICE_PUE_CTRL);

	for (i = 0; i < MAX_FW_NR; i++)
		release_firmware(fw[i]);

	return 0;

release_fw:
	for (i = 0; i < MAX_FW_NR; i++)
		release_firmware(fw[i]);

	return ret;
}

static void eip_priv_unit_offset_init(struct safexcel_crypto_priv *priv)
{
	struct safexcel_unit_offset *unit_off = &priv->unit_off;

	unit_off->hia_aic = EIP197_HIA_AIC_ADDR;
	unit_off->hia_aic_g = EIP197_HIA_AIC_G_ADDR;
	unit_off->hia_aic_r = EIP197_HIA_AIC_R_ADDR;
	unit_off->hia_xdr = EIP197_HIA_AIC_xDR_ADDR;
	unit_off->hia_dfe = EIP197_HIA_AIC_DFE_ADDR;
	unit_off->hia_dfe_thrd = EIP197_HIA_AIC_DFE_THRD_ADDR;
	unit_off->hia_dse = EIP197_HIA_AIC_DSE_ADDR;
	unit_off->hia_dse_thrd = EIP197_HIA_AIC_DSE_THRD_ADDR;
	unit_off->hia_gen_cfg = EIP197_HIA_GC;
	unit_off->pe = EIP197_HIA_PE_ADDR;
}

/* Configure the command descriptor ring manager */
static int eip_hw_setup_cdesc_rings(struct safexcel_crypto_priv *priv)
{
	u32 hdw, cd_size_rnd, val;
	int i;

	hdw = readl(EIP197_HIA_AIC_G(priv) + EIP197_HIA_OPTIONS);
	hdw = (hdw & EIP197_xDR_HDW_MASK) >> EIP197_xDR_HDW_OFFSET;

	cd_size_rnd = (priv->config.cd_size + (BIT(hdw) - 1)) >> hdw;

	for (i = 0; i < priv->config.rings; i++) {
		/* ring base address */
		writel(lower_32_bits(priv->ring[i].cdr.base_dma),
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_RING_BASE_ADDR_LO);
		writel(upper_32_bits(priv->ring[i].cdr.base_dma),
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_RING_BASE_ADDR_HI);

		writel(EIP197_xDR_DESC_MODE_64BIT |
		       (priv->config.cd_offset << EIP197_xDR_DESC_CD_OFFSET) |
		       priv->config.cd_size,
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_DESC_SIZE);
		writel(((EIP197_FETCH_COUNT * (cd_size_rnd << hdw)) << EIP197_XDR_CD_FETCH_THRESH) |
		       (EIP197_FETCH_COUNT * priv->config.cd_offset),
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_CFG);

		/* Configure DMA tx control */
		val = EIP197_HIA_xDR_CFG_WR_CACHE(WR_CACHE_3BITS);
		val |= EIP197_HIA_xDR_CFG_RD_CACHE(RD_CACHE_3BITS);
		writel(val, EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_DMA_CFG);

		/* clear any pending interrupt */
		writel(EIP197_CDR_INTR_MASK,
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_STAT);
	}

	return 0;
}

/* Configure the result descriptor ring manager */
static int eip_hw_setup_rdesc_rings(struct safexcel_crypto_priv *priv)
{
	u32 hdw, rd_size_rnd, val;
	int i;

	hdw = readl(EIP197_HIA_AIC_G(priv) + EIP197_HIA_OPTIONS);
	hdw = (hdw & EIP197_xDR_HDW_MASK) >> EIP197_xDR_HDW_OFFSET;

	rd_size_rnd = (priv->config.rd_size + (BIT(hdw) - 1)) >> hdw;

	for (i = 0; i < priv->config.rings; i++) {
		/* ring base address */
		writel(lower_32_bits(priv->ring[i].rdr.base_dma),
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_RING_BASE_ADDR_LO);
		writel(upper_32_bits(priv->ring[i].rdr.base_dma),
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_RING_BASE_ADDR_HI);

		writel(EIP197_xDR_DESC_MODE_64BIT |
		       priv->config.rd_offset << EIP197_xDR_DESC_CD_OFFSET |
		       priv->config.rd_size,
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_DESC_SIZE);

		writel((EIP197_FETCH_COUNT * (rd_size_rnd << hdw)) << EIP197_XDR_CD_FETCH_THRESH |
		       (EIP197_FETCH_COUNT * priv->config.rd_offset),
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_CFG);

		/* Configure DMA tx control */
		val = EIP197_HIA_xDR_CFG_WR_CACHE(WR_CACHE_3BITS);
		val |= EIP197_HIA_xDR_CFG_RD_CACHE(RD_CACHE_3BITS);
		val |= EIP197_HIA_xDR_WR_RES_BUF | EIP197_HIA_xDR_WR_CTRL_BUG;
		writel(val, EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_DMA_CFG);

		/* clear any pending interrupt */
		writel(EIP197_RDR_INTR_MASK,
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_STAT);

		/* enable ring interrupt */
		val = readl(EIP197_HIA_AIC_R(priv) + EIP197_HIA_AIC_R_ENABLE_CTRL(i));
		val |= EIP197_RDR_IRQ(i);
		writel(val, EIP197_HIA_AIC_R(priv) + EIP197_HIA_AIC_R_ENABLE_CTRL(i));
	}

	return 0;
}

static int eip_hw_init(struct device *dev, struct safexcel_crypto_priv *priv)
{
	u32 version, val;
	int i, ret;

	/* Determine endianness and configure byte swap */
	version = readl(EIP197_HIA_AIC(priv) + EIP197_HIA_VERSION);
	val = readl(EIP197_HIA_AIC(priv) + EIP197_HIA_MST_CTRL);

	if ((version & 0xffff) == EIP197_HIA_VERSION_BE)
		val |= EIP197_HIA_SLAVE_BYTE_SWAP;
	else if (((version >> 16) & 0xffff) == EIP197_HIA_VERSION_LE)
		val |= (EIP197_HIA_SLAVE_NO_BYTE_SWAP);

	writel(val, EIP197_HIA_AIC(priv) + EIP197_HIA_MST_CTRL);


	/* configure wr/rd cache values */
	val = MST_CTRL_RD_CACHE(RD_CACHE_4BITS) | MST_CTRL_WD_CACHE(WR_CACHE_4BITS);
	writel(val, EIP197_HIA_GEN_CFG(priv) + EIP197_MST_CTRL);

	/*
	 * Interrupts reset
	 */

	/* Disable all global interrupts */
	writel(0, EIP197_HIA_AIC_G(priv) + EIP197_HIA_AIC_G_ENABLE_CTRL);

	/* Clear any pending interrupt */
	writel(GENMASK(31, 0), EIP197_HIA_AIC_G(priv) + EIP197_HIA_AIC_G_ACK);

	/*
	 * Data Fetch Engine configuration
	 */

	/* Reset all DFE threads */
	writel(EIP197_DxE_THR_CTRL_RESET_PE,
	       EIP197_HIA_DFE_THRD(priv) + EIP197_HIA_DFE_THR_CTRL);

	/* Reset HIA input interface arbiter */
	writel(EIP197_HIA_RA_PE_CTRL_RESET,
	       EIP197_HIA_AIC(priv) + EIP197_HIA_RA_PE_CTRL);

	/* DMA transfer size to use */
	val = EIP197_HIA_DFE_CFG_DIS_DEBUG;
	val |= EIP197_HIA_DxE_CFG_MIN_DATA_SIZE(5) | EIP197_HIA_DxE_CFG_MAX_DATA_SIZE(9);
	val |= EIP197_HIA_DxE_CFG_MIN_CTRL_SIZE(5) | EIP197_HIA_DxE_CFG_MAX_CTRL_SIZE(7);
	val |= EIP197_HIA_DxE_CFG_DATA_CACHE_CTRL(RD_CACHE_3BITS);
	val |= EIP197_HIA_DxE_CFG_CTRL_CACHE_CTRL(RD_CACHE_3BITS);
	writel(val, EIP197_HIA_DFE(priv) + EIP197_HIA_DFE_CFG);

	/* Leave the DFE threads reset state */
	writel(0, EIP197_HIA_DFE_THRD(priv) + EIP197_HIA_DFE_THR_CTRL);

	/* Configure the procesing engine thresholds */
	writel(EIP197_PE_IN_xBUF_THRES_MIN(5) | EIP197_PE_IN_xBUF_THRES_MAX(9),
	      EIP197_PE(priv) + EIP197_PE_IN_DBUF_THRES);
	writel(EIP197_PE_IN_xBUF_THRES_MIN(5) | EIP197_PE_IN_xBUF_THRES_MAX(7),
	      EIP197_PE(priv) + EIP197_PE_IN_TBUF_THRES);

	/* enable HIA input interface arbiter and rings */
	writel(EIP197_HIA_RA_PE_CTRL_EN | GENMASK(priv->config.hw_rings - 1, 0),
	       EIP197_HIA_AIC(priv) + EIP197_HIA_RA_PE_CTRL);

	/*
	 * Data Store Engine configuration
	 */

	/* Reset all DSE threads */
	writel(EIP197_DxE_THR_CTRL_RESET_PE,
	       EIP197_HIA_DSE_THRD(priv) + EIP197_HIA_DSE_THR_CTRL);

	/* Wait for all DSE threads to complete */
	while ((readl(EIP197_HIA_DSE_THRD(priv) + EIP197_HIA_DSE_THR_STAT) &
	       GENMASK(15, 12)) != GENMASK(15, 12))
		;

	/* DMA transfer size to use */
	val = EIP197_HIA_DSE_CFG_DIS_DEBUG;
	val |= EIP197_HIA_DxE_CFG_MIN_DATA_SIZE(7) | EIP197_HIA_DxE_CFG_MAX_DATA_SIZE(8);
	val |= EIP197_HIA_DxE_CFG_DATA_CACHE_CTRL(RD_CACHE_3BITS);
	val |= EIP197_HIA_DSE_CFG_BUFFER_CTRL(2);
	writel(val, EIP197_HIA_DSE(priv) + EIP197_HIA_DSE_CFG);

	/* Leave the DSE threads reset state */
	writel(0, EIP197_HIA_DSE_THRD(priv) + EIP197_HIA_DSE_THR_CTRL);

	/* Configure the procesing engine thresholds */
	writel(EIP197_PE_OUT_DBUF_THRES_MIN(7) | EIP197_PE_OUT_DBUF_THRES_MAX(8),
	       EIP197_PE(priv) + EIP197_PE_OUT_DBUF_THRES);

	/*
	 * Processing Engine configuration
	 */

	/*
	 * Command Descriptor Rings prepare
	 */
	for (i = 0; i < priv->config.hw_rings; i++) {
		/* Clear interrupts for this ring */
		writel(GENMASK(31, 0),
		       EIP197_HIA_AIC_R(priv) + EIP197_HIA_AIC_R_ENABLE_CLR(i));

		/* disable external triggering */
		writel(0, EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_CFG);

		/* Clear the pending prepared counter */
		writel(EIP197_xDR_PREP_CLR_COUNT,
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_PREP_COUNT);

		/* Clear the pending processed counter */
		writel(EIP197_xDR_PROC_CLR_COUNT,
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_PROC_COUNT);

		writel(0, EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_PREP_PNTR);
		writel(0, EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_PROC_PNTR);

		writel((EIP197_DEFAULT_RING_SIZE * priv->config.cd_offset) << 2,
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(i) + EIP197_HIA_xDR_RING_SIZE);
	}

	/*
	 * Result Descriptor Ring prepare
	 */

	for (i = 0; i < priv->config.hw_rings; i++) {
		/* disable external triggering*/
		writel(0, EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_CFG);

		/* Clear the pending prepared counter */
		writel(EIP197_xDR_PREP_CLR_COUNT,
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_PREP_COUNT);

		/* Clear the pending processed counter */
		writel(EIP197_xDR_PROC_CLR_COUNT,
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_PROC_COUNT);

		writel(0, EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_PREP_PNTR);
		writel(0, EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_PROC_PNTR);

		/* ring size */
		writel((EIP197_DEFAULT_RING_SIZE * priv->config.rd_offset) << 2,
		       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(i) + EIP197_HIA_xDR_RING_SIZE);
	}

	/* Enable command descriptor rings */
	writel(EIP197_DxE_THR_CTRL_EN | GENMASK(priv->config.hw_rings - 1, 0),
	       EIP197_HIA_DFE_THRD(priv) + EIP197_HIA_DFE_THR_CTRL);

	/* Enable result descriptor rings */
	writel(EIP197_DxE_THR_CTRL_EN | GENMASK(priv->config.hw_rings - 1, 0),
	       EIP197_HIA_DSE_THRD(priv) + EIP197_HIA_DSE_THR_CTRL);

	/* Clear any HIA interrupt */
	writel(GENMASK(30, 20), EIP197_HIA_AIC_G(priv) + EIP197_HIA_AIC_G_ACK);

	/* init PRNG */
	eip197_prng_init(priv);

	/* init transform record cache */
	ret = eip197_trc_cache_init(dev, priv);
	if (ret) {
		dev_err(dev, "eip197_trc_cache_init failed\n");
		return ret;
	}

	/* Firmware load */
	ret = eip197_load_fw(dev, priv);
	if (ret) {
		dev_err(dev, "eip197_load_fw failed\n");
		return ret;
	}

	eip_hw_setup_cdesc_rings(priv);
	eip_hw_setup_rdesc_rings(priv);

	return 0;
}

/* Dequeue crypto API requests and send to the engine */
void safexcel_dequeue(struct safexcel_crypto_priv *priv, int ring)
{
	struct crypto_async_request *req, *backlog;
	struct safexcel_context *ctx;
	struct safexcel_request *request;
	int ret, nreq = 0;
	int cdesc = 0, rdesc = 0;
	int commands, results;
	u32 val;

	do {
		spin_lock_bh(&priv->ring[ring].queue_lock);
		req = crypto_dequeue_request(&priv->ring[ring].queue);
		backlog = crypto_get_backlog(&priv->ring[ring].queue);
		spin_unlock_bh(&priv->ring[ring].queue_lock);

		if (!req)
			goto finalize;

		if (backlog)
			backlog->complete(backlog, -EINPROGRESS);

		request = kzalloc(sizeof(*request), EIP197_GFP_FLAGS(*req));
		if (!request) {
			ret = -ENOMEM;
			goto resource_fail;
		}

		ctx = crypto_tfm_ctx(req->tfm);
		ret = ctx->send(req, ring, request, &commands, &results);
		if (ret) {
			kfree(request);
			goto resource_fail;
		}

		cdesc += commands;
		rdesc += results;

		if (++nreq > EIP197_MAX_BATCH_SZ) {
			priv->ring[ring].need_dequeue = true;
			goto finalize;
		}
	} while (true);

resource_fail:
	/* resource alloc fail, bail out, complete the request and */
	/* leave dequeue enabled since we have not cleaned it all  */
	priv->ring[ring].need_dequeue = true;

	local_bh_disable();
	req->complete(req, ret);
	local_bh_enable();

finalize:
	if (!nreq)
		return;

	spin_lock_bh(&priv->ring[ring].lock);

	/* Configure when we want an interrupt */
	val = EIP197_HIA_RDR_THRESH_PKT_MODE | EIP197_HIA_RDR_THRESH_PROC_PKT(nreq);
	val |= EIP197_HIA_RDR_THRESH_TIMEOUT(0x80);
	writel_relaxed(val, EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(ring) + EIP197_HIA_xDR_THRESH);

	/* let the RDR know we have pending descriptors */
	writel_relaxed((rdesc * priv->config.rd_offset) << EIP197_xDR_PREP_RD_COUNT_INCR_OFFSET,
	       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(ring) + EIP197_HIA_xDR_PREP_COUNT);

	/* let the CDR know we have pending descriptors */
	writel((cdesc * priv->config.cd_offset) << EIP197_xDR_PREP_RD_COUNT_INCR_OFFSET,
	       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(ring) + EIP197_HIA_xDR_PREP_COUNT);

	spin_unlock_bh(&priv->ring[ring].lock);
}

/* Select the ring which will be used for the operation */
inline int safexcel_select_ring(struct safexcel_crypto_priv *priv)
{
	return (atomic_inc_return(&priv->ring_used) % priv->config.rings);
}

/* Default IRQ handler */
static irqreturn_t safexcel_irq_default(int irq, void *data)
{
	struct safexcel_crypto_priv *priv = data;

	dev_err(priv->dev, "Got an interrupt handled by the default handler.\n");

	return IRQ_NONE;
}

/* Global IRQ handler */
static irqreturn_t safexcel_irq_global(int irq, void *data)
{
	struct safexcel_crypto_priv *priv = data;
	u32 status = readl(EIP197_HIA_AIC_G(priv) + EIP197_HIA_AIC_G_ENABLED_STAT);

	writel(status, EIP197_HIA_AIC_G(priv) + EIP197_HIA_AIC_G_ACK);

	return IRQ_HANDLED;
}

/* Free crypto API result mapping */
void safexcel_free_context(struct safexcel_crypto_priv *priv,
				  struct crypto_async_request *req,
				  int result_sz)
{
	struct safexcel_context *ctx = crypto_tfm_ctx(req->tfm);

	if (ctx->result_dma)
		dma_unmap_single(priv->dev, ctx->result_dma, result_sz,
				 DMA_FROM_DEVICE);

	if (ctx->cache_dma) {
		dma_unmap_single(priv->dev, ctx->cache_dma, ctx->cache_sz,
				 DMA_TO_DEVICE);
		ctx->cache_sz = 0;
	}
}

/* Acknoledge and release the used descriptors */
void safexcel_complete(struct safexcel_crypto_priv *priv, int ring)
{
	struct safexcel_command_desc *cdesc;

	/* Acknowledge the command descriptors */
	do {
		cdesc = safexcel_ring_next_rptr(priv, &priv->ring[ring].cdr);
		if (IS_ERR(cdesc)) {
			dev_err(priv->dev,
				"Could not retrieve the command descriptor\n");
			return;
		}
	} while (!cdesc->last_seg);
}

/* Context completion cache invalidation */
void safexcel_inv_complete(struct crypto_async_request *req, int error)
{
	struct safexcel_inv_result *result = req->data;

	if (error == -EINPROGRESS)
		return;

	result->error = error;
	complete(&result->completion);
}

/* Context cache invalidation */
int safexcel_invalidate_cache(struct crypto_async_request *async,
			      struct safexcel_context *ctx,
			      struct safexcel_crypto_priv *priv,
			      dma_addr_t ctxr_dma,
			      int ring, struct safexcel_request *request)
{
	struct safexcel_command_desc *cdesc;
	struct safexcel_result_desc *rdesc;
	phys_addr_t ctxr_phys;
	int ret;

	ctxr_phys = dma_to_phys(priv->dev, ctxr_dma);

	spin_lock_bh(&priv->ring[ring].egress_lock);

	/* prepare command descriptor */
	cdesc = safexcel_add_cdesc(priv, ring, true, true,
				   0, 0, 0, ctxr_phys);

	if (IS_ERR(cdesc)) {
		ret = PTR_ERR(cdesc);
		goto unlock;
	}

	cdesc->control_data.type = CONTEXT_CONTROL_TYPE_AUTONOMUS_TOKEN;
	cdesc->control_data.options = 0;
	cdesc->control_data.refresh = 0;
	cdesc->control_data.control0 = CONTEXT_CONTROL_INV_TR <<
				       CONTEXT_CONTROL_HW_SERVICES_OFFSET;

	/* prepare result descriptor */
	rdesc = safexcel_add_rdesc(priv, ring, true, true, ctxr_phys, 0);

	if (IS_ERR(rdesc)) {
		ret = PTR_ERR(rdesc);
		goto cdesc_rollback;
	}

	request->req = async;
	list_add_tail(&request->list, &priv->ring[ring].list);

	spin_unlock_bh(&priv->ring[ring].egress_lock);

	return 0;

cdesc_rollback:
	safexcel_ring_rollback_wptr(priv, &priv->ring[ring].cdr);

unlock:
	spin_unlock_bh(&priv->ring[ring].egress_lock);
	return ret;
}

/* Generic result handler */
static inline void safexcel_handle_result_descriptor(struct safexcel_crypto_priv *priv,
						     int ring)
{
	struct safexcel_request *sreq;
	struct safexcel_context *ctx;
	int ret, i, nreq, ndesc = 0;
	bool should_complete;

	nreq = readl(EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(ring) + EIP197_HIA_xDR_PROC_COUNT);
	nreq = (nreq >> EIP197_xDR_PROC_xD_PKT_OFFSET) & EIP197_xDR_PROC_xD_PKT_MASK;

	if (!nreq)
		return;

	for (i = 0; i < nreq; i++) {
		spin_lock_bh(&priv->ring[ring].egress_lock);
		sreq = list_first_entry(&priv->ring[ring].list, struct safexcel_request, list);
		list_del(&sreq->list);
		spin_unlock_bh(&priv->ring[ring].egress_lock);

		ctx = crypto_tfm_ctx(sreq->req->tfm);
		ndesc = ctx->handle_result(priv, ring, sreq->req,
					   &should_complete, &ret);
		if (ndesc < 0) {
			dev_err(priv->dev, "failed to handle result (%d)", ndesc);
			return;
		}

		if (should_complete) {
			local_bh_disable();
			sreq->req->complete(sreq->req, ret);
			local_bh_enable();
		}

		kfree(sreq);
	}

	writel(EIP197_HIA_RDR_THRESH_PKT_MODE | EIP197_xDR_PROC_xD_PKT(i),
	       EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_RDR(ring) + EIP197_HIA_xDR_PROC_COUNT);
}

/* Result worker routine */
static void safexcel_handle_result_work(struct work_struct *work)
{
	struct safexcel_work_data *data = container_of(work, struct safexcel_work_data, work);
	struct safexcel_crypto_priv *priv = data->priv;

	safexcel_handle_result_descriptor(priv, data->ring);

	if (priv->ring[data->ring].need_dequeue) {
		priv->ring[data->ring].need_dequeue = false;
		safexcel_dequeue(data->priv, data->ring);
	}
}

struct safexcel_ring_irq_data {
	struct safexcel_crypto_priv *priv;
	int ring;
};

/* Ring IRQ handler */
static irqreturn_t safexcel_irq_ring(int irq, void *data)
{
	struct safexcel_ring_irq_data *irq_data = data;
	struct safexcel_crypto_priv *priv = irq_data->priv;
	int ring = irq_data->ring;
	u32 status, stat;

	status = readl(EIP197_HIA_AIC_R(priv) + EIP197_HIA_AIC_R_ENABLED_STAT(ring));

	if (!status)
		return IRQ_NONE;

	/* CDR interrupts */
	if (status & EIP197_CDR_IRQ(ring)) {
		stat = readl_relaxed(EIP197_HIA_AIC_xDR(priv) + EIP197_HIA_CDR(ring) + EIP197_HIA_xDR_STAT);

		if (unlikely(stat & EIP197_xDR_ERR)) {
			/*
			 * Fatal error, the CDR is unusable and must be
			 * reinitialized. This should not happen under
			 * normal circumstances.
			 */
			dev_err(priv->dev, "CDR: fatal error.");
		}

		/* ACK the interrupts */
		writel_relaxed(stat & 0xff, EIP197_HIA_AIC_xDR(priv) +
			       EIP197_HIA_CDR(ring) + EIP197_HIA_xDR_STAT);
	}

	/* RDR interrupts */
	if (status & EIP197_RDR_IRQ(ring)) {
		stat = readl_relaxed(EIP197_HIA_AIC_xDR(priv) +
				     EIP197_HIA_RDR(ring) + EIP197_HIA_xDR_STAT);

		if (unlikely(stat & EIP197_xDR_ERR)) {
			/*
			 * Fatal error, the CDR is unusable and must be
			 * reinitialized. This should not happen under
			 * normal circumstances.
			 */
			dev_err(priv->dev, "RDR: fatal error.");
		}

		if (likely(stat & EIP197_xDR_THRESH)) {
			writel_relaxed(0, EIP197_HIA_AIC_xDR(priv) +
				       EIP197_HIA_RDR(ring) + EIP197_HIA_xDR_THRESH);
			queue_work(priv->ring[ring].workqueue, &priv->ring[ring].work_data.work);
		} else if (unlikely(stat & EIP197_xDR_TIMEOUT)) {
			queue_work(priv->ring[ring].workqueue, &priv->ring[ring].work_data.work);
		}

		/* ACK the interrupts */
		writel_relaxed(stat & 0xff, EIP197_HIA_AIC_xDR(priv) +
			       EIP197_HIA_RDR(ring) + EIP197_HIA_xDR_STAT);
	}

	/* ACK the interrupts */
	writel(status, EIP197_HIA_AIC_R(priv) + EIP197_HIA_AIC_R_ACK(ring));

	return IRQ_HANDLED;
}

/* Register gloabl interrupts */
static int safexcel_request_irq(struct platform_device *pdev, const char *name,
				irq_handler_t handler,
				struct safexcel_crypto_priv *priv)
{
	int ret, irq = platform_get_irq_byname(pdev, name);

	if (irq < 0) {
		dev_err(&pdev->dev, "unable to get IRQ '%s'\n", name);
		return irq;
	}

	ret = devm_request_irq(&pdev->dev, irq, handler, 0,
			       dev_name(&pdev->dev), priv);
	if (ret) {
		dev_err(&pdev->dev, "unable to request IRQ %d\n", irq);
		return ret;
	}

	return irq;
}

/* Register ring interrupts */
static int safexcel_request_ring_irq(struct platform_device *pdev, const char *name,
				     irq_handler_t handler,
				     struct safexcel_ring_irq_data *ring_irq_priv)
{
	int ret, irq = platform_get_irq_byname(pdev, name);

	if (irq < 0) {
		dev_err(&pdev->dev, "unable to get IRQ '%s'\n", name);
		return irq;
	}

	ret = devm_request_irq(&pdev->dev, irq, handler, 0,
			       dev_name(&pdev->dev), ring_irq_priv);
	if (ret) {
		dev_err(&pdev->dev, "unable to request IRQ %d\n", irq);
		return ret;
	}

	return irq;
}

/* List of supported algorithms */
static struct safexcel_alg_template *safexcel_algs[] = {
	&safexcel_alg_ecb_aes,
	&safexcel_alg_cbc_aes,
	&safexcel_alg_sha1,
	&safexcel_alg_sha224,
	&safexcel_alg_sha256,
	&safexcel_alg_hmac_sha1,
};

/* Register the supported hash and cipher algorithms */
static int safexcel_register_algorithms(struct safexcel_crypto_priv *priv)
{
	int i, j, ret = 0;

	for (i = 0; i < ARRAY_SIZE(safexcel_algs); i++) {
		safexcel_algs[i]->priv = priv;

		if (safexcel_algs[i]->type == SAFEXCEL_ALG_TYPE_CIPHER)
			ret = crypto_register_alg(&safexcel_algs[i]->alg.crypto);
		else
			ret = crypto_register_ahash(&safexcel_algs[i]->alg.ahash);

		if (ret)
			goto fail;
	}

	return 0;

fail:
	for (j = i; j < 0; j--) {
		if (safexcel_algs[j]->type == SAFEXCEL_ALG_TYPE_CIPHER)
			crypto_unregister_alg(&safexcel_algs[j]->alg.crypto);
		else
			crypto_unregister_ahash(&safexcel_algs[j]->alg.ahash);
	}

	return ret;
}

/* Unregister the hash and cipher algorithms */
static void safexcel_unregister_algorithms(struct safexcel_crypto_priv *priv)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(safexcel_algs); i++) {
		if (safexcel_algs[i]->type == SAFEXCEL_ALG_TYPE_CIPHER)
			crypto_unregister_alg(&safexcel_algs[i]->alg.crypto);
		else
			crypto_unregister_ahash(&safexcel_algs[i]->alg.ahash);
	}
}

static void safexcel_configure(struct safexcel_crypto_priv *priv)
{
	u32 val, mask;

	val = readl(EIP197_HIA_AIC_G(priv) + EIP197_HIA_OPTIONS);
	val = (val & EIP197_xDR_HDW_MASK) >> EIP197_xDR_HDW_OFFSET;
	mask = BIT(val) - 1;

	/* Read number of rings from the engine */
	val = readl(EIP197_HIA_AIC_G(priv) + EIP197_HIA_OPTIONS);
	priv->config.hw_rings = (val & GENMASK(3, 0));

	/* Check the requested number of rings given in the module param.
	 * If the module param is uninitialized, use all available rings
	 */
	if (rings[priv->id] == RINGS_UNINITIALIZED)
		rings[priv->id] = priv->config.hw_rings;

	/* Check if the number of requested rings in module param is valid */
	if (rings[priv->id] > priv->config.hw_rings) {
		/* Invalid, use all available rings */
		priv->config.rings = priv->config.hw_rings;
		dev_warn(priv->dev, "requested %d rings, given only %d rings\n",
			 rings[priv->id], priv->config.hw_rings);
	} else  {
		priv->config.rings = rings[priv->id];
	}

	priv->config.cd_size = (sizeof(struct safexcel_command_desc) / sizeof(u32));
	priv->config.cd_offset = (priv->config.cd_size + mask) & ~mask;

	priv->config.rd_size = (sizeof(struct safexcel_result_desc) / sizeof(u32));
	priv->config.rd_offset = (priv->config.rd_size + mask) & ~mask;
}

static int safexcel_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct safexcel_crypto_priv *priv;
	int i, ret;

	priv = devm_kzalloc(dev, sizeof(struct safexcel_crypto_priv),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->eip_type = (enum safexcel_eip_type)of_device_get_match_data(dev);

	eip_priv_unit_offset_init(priv);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base)) {
		dev_err(dev, "failed to get resource\n");
		return PTR_ERR(priv->base);
	}

	ret = of_property_read_u32(dev->of_node, "cell-index", &priv->id);
	if (ret) {
		dev_err(dev, "failed to read cell-index property\n");
		return ret;
	}

	priv->clk = of_clk_get(dev->of_node, 0);
	if (!IS_ERR(priv->clk)) {
		ret = clk_prepare_enable(priv->clk);
		if (ret) {
			dev_err(dev, "unable to enable clk (%d)\n", ret);
			return ret;
		}
	} else {
		/* The clock isn't mandatory */
		if (PTR_ERR(priv->clk) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
	}

	priv->context_pool = dmam_pool_create("safexcel-context", dev,
					      sizeof(struct safexcel_context_record),
					      1, 0);
	if (!priv->context_pool) {
		ret = -ENOMEM;
		goto err_clk;
	}

	safexcel_configure(priv);

	ret = safexcel_request_irq(pdev, "eip_out", safexcel_irq_global, priv);
	if (ret < 0)
		goto err_pool;

	ret = safexcel_request_irq(pdev, "eip_addr", safexcel_irq_default, priv);
	if (ret < 0)
		goto err_pool;

	for (i = 0; i < priv->config.rings; i++) {
		char irq_name[6] = {0}; /* "ringX\0" */
		char wq_name[9] = {0}; /* "wq_ringX\0" */
		int irq;
		struct safexcel_ring_irq_data *ring_irq;

		ret = safexcel_init_ring_descriptors(priv,
						     &priv->ring[i].cdr,
						     &priv->ring[i].rdr);
		if (ret)
			goto err_pool;

		ring_irq = devm_kzalloc(dev, sizeof(struct safexcel_ring_irq_data),
					GFP_KERNEL);
		if (!ring_irq) {
			ret = -ENOMEM;
			goto err_pool;
		}

		ring_irq->priv = priv;
		ring_irq->ring = i;

		snprintf(irq_name, 6, "ring%d", i);
		irq = safexcel_request_ring_irq(pdev, irq_name, safexcel_irq_ring,
						ring_irq);

		if (irq < 0)
			goto err_pool;

		priv->ring[i].work_data.priv = priv;
		priv->ring[i].work_data.ring = i;
		INIT_WORK(&priv->ring[i].work_data.work, safexcel_handle_result_work);

		snprintf(wq_name, 9, "wq_ring%d", i);
		priv->ring[i].workqueue = create_singlethread_workqueue(wq_name);
		if (!priv->ring[i].workqueue) {
			ret = -ENOMEM;
			goto err_pool;
		}

		INIT_LIST_HEAD(&priv->ring[i].list);
		spin_lock_init(&priv->ring[i].lock);
		spin_lock_init(&priv->ring[i].egress_lock);
		spin_lock_init(&priv->ring[i].queue_lock);
		crypto_init_queue(&priv->ring[i].queue, EIP197_DEFAULT_RING_SIZE);
	}
	atomic_set(&priv->ring_used, 0);

	platform_set_drvdata(pdev, priv);

	ret = eip_hw_init(dev, priv);
	if (ret) {
		dev_err(dev, "EIP h/w init failed (%d)\n", ret);
		goto err_pool;
	}

	/*
	 * Kernel crypto API doesn't allow to register 2 engines.
	 * Allowing working with 2 engines requires additional modification
	 * which are planned as future work (Modify the Kernel crypto API or
	 * implement load balance in EIP driver to handle 2 engines).
	 *
	 * Currently we want to register the first probed engine.
	 */
	if (eip_in_use == -1 && priv->config.rings) {
		eip_in_use = priv->id;
		ret = safexcel_register_algorithms(priv);
		if (ret) {
			dev_err(dev, "Failed to register algorithms (%d)\n", ret);
			goto err_pool;
		}
	}

	return 0;

err_pool:
	dma_pool_destroy(priv->context_pool);
err_clk:
	clk_disable_unprepare(priv->clk);
	return ret;
}


static int safexcel_remove(struct platform_device *pdev)
{
	struct safexcel_crypto_priv *priv = platform_get_drvdata(pdev);
	int i;

	if (priv->id == eip_in_use)
		safexcel_unregister_algorithms(priv);

	clk_disable_unprepare(priv->clk);

	for (i = 0; i < priv->config.rings; i++) {
		safexcel_free_ring_descriptors(priv, &priv->ring[i].cdr,
					       &priv->ring[i].rdr);
		destroy_workqueue(priv->ring[i].workqueue);
	}

	return 0;
}

static const struct of_device_id safexcel_of_match_table[] = {
	{
		.compatible = "inside-secure,safexcel-eip197",
		.data = (void *)EIP197,
	},
};


static struct platform_driver  crypto_safexcel = {
	.probe		= safexcel_probe,
	.remove		= safexcel_remove,
	.driver		= {
		.name	= "crypto-safexcel",
		.of_match_table = safexcel_of_match_table,
	},
};
module_platform_driver(crypto_safexcel);
module_param_array(rings, uint, NULL, 0);
MODULE_PARM_DESC(rings, "number of rings to be used by the driver");

MODULE_AUTHOR("Antoine Tenart <antoine.tenart@free-electrons.com>");
MODULE_DESCRIPTION("Support for SafeXcel Cryptographic Engines EIP197");
MODULE_LICENSE("GPL v2");
