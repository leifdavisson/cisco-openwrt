/* SPDX-License-Identifier: GPL-2.0-or-later OR AGPL-3.0-or-later */
#define P1023_CCSR_BASE		0xe0000000UL
#define P1023_FMAN_PHYS		(P1023_CCSR_BASE + 0x100000UL)
#define P1023_FMAN_SIZE		0x100000UL

/* Offsets within FMan (0xe0100000) */
#define FM_MURAM_OFF		0x000000UL
#define FM_MURAM_SIZE		0x010000UL	/* 64 KiB MURAM */
#define FM_BMI_COMMON_OFF	0x080000UL
#define FM_QMI_COMMON_OFF	0x080400UL
#define FM_BMI_RX0_OFF		0x088000UL	/* 1G RX Port 0 (hw port 0x08) */
#define FM_BMI_TX0_OFF		0x0a8000UL	/* 1G TX Port 0 (hw port 0x28) */
#define FM_DMA_OFF		0x0c2000UL
#define FM_FPM_OFF		0x0c3000UL
#define FM_IRAM_OFF		0x0c4000UL
#define FM_DTSEC0_OFF		0x0e0000UL	/* dTSEC0 MAC */
#define FM_MDIO0_OFF		0x0e1120UL	/* FMan MDIO0 */

/* Non-overlapping 64 KiB MURAM partition map */
#define FM_DMA_CAM_OFF		0x001000U	/* 2 KiB (0x1000..0x17ff, 4K-aligned) */
#define FM_BMI_FIFO_OFF		0x002000U	/* 48 KiB (0x2000..0xdfff, 256-aligned) */
#define FM_BMI_FIFO_SIZE	0x00c000U

/* IRAM registers */
#define IRAM_IADD		0x00
#define IRAM_IDATA		0x04
#define IRAM_READY_REG		0x0c
#define IRAM_IADD_AIE		0x80000000U
#define IRAM_READY_BIT		0x80000000U
#define FM_IRAM_SIZE_WORDS	(0x8000U / 4U)	/* 32 KiB = 8192 words */

/* dTSEC0 registers */
#define DTSEC_IEVENT		0x008
#define DTSEC_IMASK		0x00c
#define DTSEC_ECNTRL		0x014
#define DTSEC_TBIPA		0x01c
#define DTSEC_TCTRL		0x040
#define DTSEC_RCTRL		0x050
#define DTSEC_MACCFG1		0x100
#define DTSEC_MACCFG2		0x104
#define DTSEC_IPGIFG		0x108
#define DTSEC_HAFDUP		0x10c
#define DTSEC_MAXFRM		0x110
#define DTSEC_MACSTNADDR1	0x140
#define DTSEC_MACSTNADDR2	0x144
#define DTSEC_IADDR0		0x080
#define DTSEC_GADDR0		0x0a0
#define DTSEC_CAR1		0x2b0
#define DTSEC_CAR2		0x2b4

#define DTSEC_ECNTRL_STEN  0x00001000U
#define DTSEC_ECNTRL_GMIIM 0x00000040U
#define DTSEC_ECNTRL_RPM   0x00000010U
#define DTSEC_ECNTRL_R100M 0x00000008U
#define DTSEC_RCTRL_PROM   0x00000009U
#define DTSEC_MACCFG1_TX_EN	0x00000001U
#define DTSEC_MACCFG1_RX_EN	0x00000004U
#define DTSEC_MACCFG1_SOFT_RST	0x80000000U

static void p1023_program_mac_addr(struct p1023_priv *priv, const u8 *mac)
{
	u32 a1, a2;

	/*
	 * dTSEC MACSTNADDR1 holds octets 5,4,3,2 (b5<<24 | b4<<16 | b3<<8 | b2)
	 * dTSEC MACSTNADDR2 holds octets 1,0 in upper 16 bits (b1<<24 | b0<<16)
	 */
	a1 = ((u32)mac[5] << 24) | ((u32)mac[4] << 16) |
	     ((u32)mac[3] << 8)  | ((u32)mac[2]);
	a2 = ((u32)mac[1] << 24) | ((u32)mac[0] << 16);

	iowrite32be(a1, priv->dtsec0 + DTSEC_MACSTNADDR1);
	iowrite32be(a2, priv->dtsec0 + DTSEC_MACSTNADDR2);
}

static void p1023_configure_dtsec_speed(struct p1023_priv *priv,
					int speed, int duplex)
{
	u32 ecntrl = DTSEC_ECNTRL_STEN | DTSEC_ECNTRL_GMIIM | DTSEC_ECNTRL_RPM;
	u32 maccfg2 = speed == SPEED_1000 ? 0x7206 : 0x7106;

	if (speed == SPEED_100)
		ecntrl |= DTSEC_ECNTRL_R100M;
	if (duplex == DUPLEX_FULL)
		maccfg2 |= 1;
	iowrite32be(ecntrl, priv->dtsec0 + DTSEC_ECNTRL);
	iowrite32be(maccfg2, priv->dtsec0 + DTSEC_MACCFG2);
}

static int p1023_upload_ucode(struct p1023_priv *priv)
{
	void __iomem *iram = priv->fman + FM_IRAM_OFF;
	u32 pad_words, last_off;
	int i;

	if (ioread32be(iram + IRAM_READY_REG) & IRAM_READY_BIT) {
		pr_info(DRV_NAME ": FMan IRAM microcode already active (0x%08x)\n",
			ioread32be(iram + IRAM_READY_REG));
		return 0;
	}

	/* 1. Enable auto-increment from 0 and fill all 32 KiB IRAM with 0xffffffff */
	iowrite32be(IRAM_IADD_AIE, iram + IRAM_IADD);
	for (i = 0; i < 1000; i++) {
		if (ioread32be(iram + IRAM_IADD) == IRAM_IADD_AIE)
			break;
		udelay(1);
	}
	if (i == 1000)
		return -ETIMEDOUT;
	for (i = 0; i < FM_IRAM_SIZE_WORDS; i++)
		iowrite32be(0xffffffffU, iram + IRAM_IDATA);

	iowrite32be((FM_IRAM_SIZE_WORDS - 1) * 4, iram + IRAM_IADD);
	mb(); /* Finish register writes before the following read or enable. */
	for (i = 0; i < 1000; i++) {
		if (ioread32be(iram + IRAM_IDATA) == 0xffffffffU)
			break;
		udelay(1);
	}

	if (i == 1000)
		return -ETIMEDOUT;

	/* 2. Enable auto-increment from 0 and write FMan v2 microcode */
	iowrite32be(IRAM_IADD_AIE, iram + IRAM_IADD);
	for (i = 0; i < 1000; i++) {
		if (ioread32be(iram + IRAM_IADD) == IRAM_IADD_AIE)
			break;
		udelay(1);
	}

	if (i == 1000)
		return -ETIMEDOUT;

	for (i = 0; i < P1023_FMAN_UCODE_WORDS; i++)
		iowrite32be(p1023_fman_ucode[i], iram + IRAM_IDATA);

	pad_words = (4U - (P1023_FMAN_UCODE_WORDS & 3U)) & 3U;
	for (i = 0; i < pad_words; i++)
		iowrite32be(0xffffffffU, iram + IRAM_IDATA);

	/* 3. Verify last microcode word landed in IRAM */
	last_off = (P1023_FMAN_UCODE_WORDS - 1) * 4;
	iowrite32be(last_off, iram + IRAM_IADD);
	mb(); /* Finish register writes before the following read or enable. */
	for (i = 0; i < 1000; i++) {
		if (ioread32be(iram + IRAM_IDATA) ==
		    p1023_fman_ucode[P1023_FMAN_UCODE_WORDS - 1])
			break;
		udelay(1);
	}
	if (i == 1000) {
		pr_err(DRV_NAME ": IRAM microcode verification timeout at 0x%x\n",
		       last_off);
		return -ETIMEDOUT;
	}

	/* 4. Point IADD to 0x0 and assert IRAM_READY */
	iowrite32be(0x00000000U, iram + IRAM_IADD);
	iowrite32be(IRAM_READY_BIT, iram + IRAM_READY_REG);
	mb(); /* Finish register writes before the following read or enable. */
	udelay(500);
	return 0;
}

static int p1023_fman_hw_init(struct p1023_priv *priv, const u8 *mac)
{
	void __iomem *fpm = priv->fman + FM_FPM_OFF;
	void __iomem *dma = priv->fman + FM_DMA_OFF;
	void __iomem *bmi_com = priv->fman + FM_BMI_COMMON_OFF;
	void __iomem *qmi_com = priv->fman + FM_QMI_COMMON_OFF;
	void __iomem *mdio0 = priv->fman + FM_MDIO0_OFF;
	u32 val, miimcfg;
	int i, ret;
	bool is_active;

	/* Stop dTSEC0 RX/TX if active */
	val = ioread32be(priv->dtsec0 + DTSEC_MACCFG1);
	iowrite32be(val & ~(DTSEC_MACCFG1_RX_EN | DTSEC_MACCFG1_TX_EN),
		    priv->dtsec0 + DTSEC_MACCFG1);

	/* Preserve FMan MDIO0 clock divider */
	miimcfg = ioread32be(mdio0 + 0x00);
	if (!miimcfg || (miimcfg & 0x80000000U))
		miimcfg = 0x00000007U;

	is_active = (ioread32be(priv->fman + FM_IRAM_OFF + IRAM_READY_REG) & IRAM_READY_BIT) != 0;
	if (!is_active) {
		/* 1. FMan Soft Reset (fm_rstc = 0x80000000) */
		iowrite32be(0x80000000U, fpm + 0xcc);
		mb(); /* Finish register writes before the following read or enable. */
		for (i = 0; i < 20000; i++) {
			if (!(ioread32be(fpm + 0xcc) & 0x80000000U))
				break;
			udelay(1);
		}
		if (i == 20000) {
			pr_err(DRV_NAME ": FMan soft reset timeout (fm_rstc=0x%08x)\n",
			       ioread32be(fpm + 0xcc));
			return -ETIMEDOUT;
		}
		udelay(200);

		/* Restore MDIO0 clock config & move internal TBI PHY away from addr 0 */
		iowrite32be(miimcfg, mdio0 + 0x00);
		iowrite32be(0x0000001fU, priv->dtsec0 + DTSEC_TBIPA);

		/* 2. Zero all 64 KiB MURAM BEFORE microcode upload */
		for (i = 0; i < FM_MURAM_SIZE; i += 4)
			iowrite32be(0, priv->muram + i);
		wmb(); /* Finish clearing MURAM before publishing the microcode. */

		/* 3. Upload FMan RISC Microcode to IRAM */
		ret = p1023_upload_ucode(priv);
		if (ret)
			return ret;
	} else {
		pr_info(DRV_NAME ": Preserving ROMMON FMan & microcode state\n");
	}

	/* 4. FMan DMA Init */
	val = ioread32be(dma + 0x00);
	iowrite32be(val | 0x0f000000U, dma + 0x00);	/* fmdmsr: clear sticky */
	iowrite32be(0x20286830U, dma + 0x04);		/* fmdmmr: CEN, AID_OVERRIDE, 32 CAM */
	iowrite32be(0x0c060006U, dma + 0x08);		/* fmdmtr */
	iowrite32be(0x08040004U, dma + 0x0c);		/* fmdmhy */
	iowrite32be(0x00000000U, dma + 0x10);		/* fmdmsetr */
	iowrite32be(0x00000000U, dma + 0x28);		/* fmdmwcr */
	iowrite32be(FM_DMA_CAM_OFF, dma + 0x2c);	/* fmdm_muram_cam = 0x1000 */
	/* DMA port rate limiters: port 8 -> 0x70, port 40 -> 0xb0 */
	iowrite32be(0x00000000U, dma + 0x60 + (8 / 2) * 4);
	iowrite32be(0x00000000U, dma + 0x60 + (40 / 2) * 4);

	/* 5. FPM Init & Per-Port RISC Binding */
	iowrite32be(0x00000000U, fpm + 0x0c);		/* dispatch loop limit */
	iowrite32be(0x10101010U, fpm + 0x10);		/* dispatch thresholds 1 */
	iowrite32be(0x10101010U, fpm + 0x14);		/* dispatch thresholds 2 */

	/* Bind Port 8 (RX0) to RISC #0 and Port 40 (TX0) to RISC #1 */
	iowrite32be((8U << 24) | 0x00010001U, fpm + 0x04);
	iowrite32be((40U << 24) | 0x00020002U, fpm + 0x04);

	for (i = 0; i < 4; i++)
		iowrite32be(0, fpm + 0x40 + i * 4);
	iowrite32be(0xe0000000U, fpm + 0xdc);		/* fmfp_ee: clear sticky, mask IRQ */
	iowrite32be(0xffffffffU, fpm + 0xe0);		/* fmfp_cev0 */
	iowrite32be(0xffffffffU, fpm + 0xe4);		/* fmfp_cev1 */
	iowrite32be(0xffffffffU, fpm + 0xe8);		/* fmfp_cev2 */
	iowrite32be(0xffffffffU, fpm + 0xec);		/* fmfp_cev3 */
	iowrite32be(0x00000000U, fpm + 0x1c);		/* RAM exception enables */

	/* Select external ECC control and acknowledge RAM ECC events. */
	iowrite32be(0x0800c000U, fpm + 0x70);		/* RAM/ECC control */
	mb(); /* Finish register writes before the following read or enable. */
	udelay(1000);

	/* 6. BMI_COMMON & QMI_COMMON Init */
	val = (((FM_BMI_FIFO_SIZE >> 8) - 1U) << 16) | (FM_BMI_FIFO_OFF >> 8);
	iowrite32be(val, bmi_com + 0x04);		/* fmbm_cfg1 = 0x00bf0020 */
	iowrite32be(0x002f000bU, bmi_com + 0x08);	/* fmbm_cfg2: 48 tasks, 12 open DMAs */
	iowrite32be(0xf0000000U, bmi_com + 0x20);	/* fmbm_ievr: clear events */
	iowrite32be(0x00000000U, bmi_com + 0x24);	/* fmbm_ier: mask IRQs */

	/* Per-port tasks/DMAs & FIFO sizes for Port 8 (RX0) and Port 40 (TX0) */
	iowrite32be(0x02020101U, bmi_com + 0x100 + 8 * 4);	/* fmbm_pp[8]: 3 tasks, 2 extra, 2 DMAs, 1 extra */
	iowrite32be(0x0010003fU, bmi_com + 0x200 + 8 * 4);	/* fmbm_pfs[8]: 16KB + 4KB extra */
	iowrite32be(0x00000000U, bmi_com + 0x300 + 8 * 4);	/* fmbm_spliodn[8] */

	iowrite32be(0x02020101U, bmi_com + 0x100 + 40 * 4);	/* fmbm_pp[40]: 3 tasks, 2 extra, 2 DMAs, 1 extra */
	iowrite32be(0x00000013U, bmi_com + 0x200 + 40 * 4);	/* fmbm_pfs[40]: 5KB FIFO (matches tfp 0x1013) */
	iowrite32be(0x00000000U, bmi_com + 0x300 + 40 * 4);	/* fmbm_spliodn[40] */

	iowrite32be(0xc0000000U, qmi_com + 0x08);
	iowrite32be(0x00000000U, qmi_com + 0x0c);
	iowrite32be(0x80000000U, qmi_com + 0x14);
	iowrite32be(0x00000000U, qmi_com + 0x18);

	/* Start BMI free buffer pool & QMI */
	iowrite32be(0x80000000U, bmi_com + 0x00);	/* fmbm_init */
	iowrite32be(0xc0000000U, qmi_com + 0x00);	/* fmqm_gc */
	mb(); /* Finish register writes before the following read or enable. */
	udelay(200);

	/* 9. dTSEC0 Cold-Boot Init */
	iowrite32be(DTSEC_MACCFG1_SOFT_RST, priv->dtsec0 + DTSEC_MACCFG1);
	udelay(20);
	iowrite32be(0x00000000U, priv->dtsec0 + DTSEC_MACCFG1);
	udelay(20);

	iowrite32be(0x0000001fU, priv->dtsec0 + DTSEC_TBIPA);
	iowrite32be(0x00000000U, priv->dtsec0 + DTSEC_IMASK);
	iowrite32be(0xffffffffU, priv->dtsec0 + DTSEC_IEVENT);
	p1023_configure_dtsec_speed(priv, SPEED_1000, DUPLEX_FULL);
	iowrite32be(0x40605060U, priv->dtsec0 + DTSEC_IPGIFG);
	iowrite32be(0x0001f037U, priv->dtsec0 + DTSEC_HAFDUP);
	iowrite32be(0x00000600U, priv->dtsec0 + DTSEC_MAXFRM);
	iowrite32be(0x00000000U, priv->dtsec0 + DTSEC_TCTRL);
	iowrite32be(DTSEC_RCTRL_PROM, priv->dtsec0 + DTSEC_RCTRL);

	for (i = 0; i < 8; i++) {
		iowrite32be(0x00000000U, priv->dtsec0 + DTSEC_IADDR0 + i * 4);
		iowrite32be(0xffffffffU, priv->dtsec0 + DTSEC_GADDR0 + i * 4);
	}

	/* Clear RMON carry registers */
	iowrite32be(0xffffffffU, priv->dtsec0 + DTSEC_CAR1);
	iowrite32be(0xffffffffU, priv->dtsec0 + DTSEC_CAR2);

	p1023_program_mac_addr(priv, mac);
	mb(); /* Finish register writes before the following read or enable. */
	return 0;
}
