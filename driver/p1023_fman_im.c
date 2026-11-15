// SPDX-License-Identifier: GPL-2.0-or-later OR AGPL-3.0-or-later
/*
 * Freescale QorIQ P1023 Frame Manager (FMan v2) Independent Mode Ethernet Driver
 * for Cisco Aironet 3702i / 3602i (Platform ap3g2 / Montserrat)
 *
 * Performs full cold-reset FMan v2 initialization:
 *  - Uploads FMan RISC microcode to IRAM and starts both FPM RISC cores
 *  - Initializes MURAM, FMan DMA, FPM, BMI Common, QMI Common
 *  - Configures BMI 1G RX0 (hw port 0x08) & TX0 (hw port 0x28) in Independent Mode
 *  - Configures dTSEC0 RGMII MAC and optional phylib Marvell 88E1510 PHY management
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>

#ifdef CONFIG_FMAN_UCODE_EMBEDDED
#include "p1023_fman_ucode.h"
#else
static const u32 p1023_fman_ucode[1] = { 0 };
#define P1023_FMAN_UCODE_WORDS 0
#endif

#define DRV_NAME		"p1023_fman_im"

/* P1023 CCSR physical base (0xe0000000 on Cisco 3702i) */
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
#define FM_RX_PRAM_OFF		0x000200U	/* 256 B (0x0200..0x02ff) */
#define FM_TX_PRAM_OFF		0x000300U	/* 256 B (0x0300..0x03ff) */
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

/* BMI Port registers */
#define FMBM_RCFG		0x00
#define FMBM_RST		0x04
#define FMBM_RFED		0x10
#define FMBM_RFNE		0x20
#define FMBM_RFQD		0x60
#define FMBM_RCFG_EN		0x80000000U
#define FMBM_RCFG_IM		0x01000000U
#define FMBM_RST_BSY		0x80000000U

#define FMBM_TCFG		0x00
#define FMBM_TST		0x04
#define FMBM_TFNE		0x18
#define FMBM_TCFQD		0x20
#define FMBM_TCFNE		0x28
#define FMBM_TCFG_EN		0x80000000U
#define FMBM_TCFG_IM		0x01000000U
#define FMBM_TST_BSY		0x80000000U

/* Independent Mode Global PRAM (256 bytes in MURAM, 256-byte aligned) */
#define IM_PRAM_SIZE		0x100
#define IM_PRAM_MODE		0x00
#define IM_PRAM_RXQD_PTR	0x04
#define IM_PRAM_TXQD_PTR	0x08
#define IM_PRAM_MRBLR		0x0c	/* u16: log2(rx_buf_size) */
#define IM_PRAM_RXQD		0x20	/* 16-byte RX Queue Descriptor */
#define IM_PRAM_TXQD		0x40	/* 16-byte TX Queue Descriptor */

/* Queue Descriptor offsets relative to RXQD (0x20) or TXQD (0x40) */
#define QD_GEN			0x00	/* u16 */
#define QD_BD_BASE_HI		0x02	/* u16 (byte +1 is bits 39:32) */
#define QD_BD_BASE_LO		0x04	/* u32 */
#define QD_BD_RING_SIZE		0x08	/* u16: ring bytes (N * 16) */
#define QD_OFFSET_IN		0x0a	/* u16 */
#define QD_OFFSET_OUT		0x0c	/* u16 */

#define IM_MODE_GBL		0x20000000U
#define IM_MODE_GRACEFUL_STOP	0x00800000U

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
#define DTSEC_IADDR0		0x180
#define DTSEC_GADDR0		0x1a0
#define DTSEC_CAR1		0x2ec
#define DTSEC_CAR2		0x2f0

#define DTSEC_ECNTRL_STEN	0x00001000U
#define DTSEC_ECNTRL_RGMII	0x00000010U	/* RGMII mode: bit 27 (0x10) */
#define DTSEC_ECNTRL_RPM	0x00000020U	/* Reduced pin mode for MII 10/100 */
#define DTSEC_ECNTRL_R100M	0x00000002U	/* R100M: 100 Mbps when RPM=1 */

#define DTSEC_RCTRL_PROM	0x00000009U	/* UPROM=0x08 | MPROM=0x01 */
#define DTSEC_MACCFG1_TX_EN	0x00000001U
#define DTSEC_MACCFG1_RX_EN	0x00000004U
#define DTSEC_MACCFG1_LOOPBACK	0x00000010U
#define DTSEC_MACCFG1_RX_FLOW	0x00000020U
#define DTSEC_MACCFG1_TX_FLOW	0x00000100U
#define DTSEC_MACCFG1_SOFT_RST	0x80000000U

/* Ring & buffer sizing */
#define RX_RING_SIZE		64
#define TX_RING_SIZE		64
#define PKT_BUF_SHIFT		11
#define PKT_BUF_SIZE		(1U << PKT_BUF_SHIFT)	/* 2048 bytes */

/* 16-byte FMan Independent Mode Buffer Descriptor (big-endian in RAM) */
struct fm_im_bd {
	__be16 status;
	__be16 len;
	__be32 res0;
	__be16 res1;
	__be16 buf_hi;
	__be32 buf_lo;
} __packed;

#define RXBD_EMPTY		0x8000U
#define RXBD_LAST		0x0800U
#define RXBD_ERR_MASK		0x003fU

#define TXBD_READY		0x8000U
#define TXBD_LAST		0x0800U

struct p1023_snap {
	u32 mdio_miimcfg;
	u32 dtsec_mac1;
	u32 dtsec_mac2;
	u32 iram_ready;
	u32 fpm_rcr;
	u32 fpm_rstc;
	u32 bmi_init;
};

struct p1023_priv {
	struct net_device *ndev;
	struct platform_device *pdev;
	struct device_node *np;
	struct napi_struct napi;
	struct hrtimer poll_timer;
	spinlock_t tx_lock;

	void __iomem *fman;
	void __iomem *muram;
	void __iomem *bmi_rx0;
	void __iomem *bmi_tx0;
	void __iomem *dtsec0;
	void __iomem *rx_pram;
	void __iomem *tx_pram;

	u32 rx_pram_off;
	u32 tx_pram_off;

	struct fm_im_bd *rx_bd;
	dma_addr_t rx_bd_dma;
	struct fm_im_bd *tx_bd;
	dma_addr_t tx_bd_dma;

	u8 *rx_bufs;
	dma_addr_t rx_bufs_dma;
	u8 *tx_bufs;
	dma_addr_t tx_bufs_dma;

	u16 rx_idx;
	u16 tx_idx;
	u16 tx_dirty;
	bool running;

	int last_link;
	int last_speed;
	int last_duplex;

	/* Diagnostics */
	u64 rx_packets;
	u64 rx_bytes;
	u64 rx_errors;
	u64 tx_packets;
	u64 tx_bytes;
	u64 tx_dropped;
	u16 last_rx_status;
	u16 last_rx_len;
	u8 last_rx_hdr[16];

	struct p1023_snap snap;
};

static struct p1023_priv *g_priv;
static struct proc_dir_entry *g_proc;

static char *mac_str;
module_param(mac_str, charp, 0444);
MODULE_PARM_DESC(mac_str, "Optional MAC address override (xx:xx:xx:xx:xx:xx)");

static void p1023_capture_snapshot(struct p1023_priv *priv)
{
	struct p1023_snap *s = &priv->snap;

	s->mdio_miimcfg = ioread32be(priv->fman + FM_MDIO0_OFF + 0x00);
	s->dtsec_mac1   = ioread32be(priv->dtsec0 + DTSEC_MACSTNADDR1);
	s->dtsec_mac2   = ioread32be(priv->dtsec0 + DTSEC_MACSTNADDR2);
	s->iram_ready   = ioread32be(priv->fman + FM_IRAM_OFF + IRAM_READY_REG);
	s->fpm_rcr      = ioread32be(priv->fman + FM_FPM_OFF + 0x70);
	s->fpm_rstc     = ioread32be(priv->fman + FM_FPM_OFF + 0xcc);
	s->bmi_init     = ioread32be(priv->fman + FM_BMI_COMMON_OFF + 0x00);
}

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
	u32 ecntrl, maccfg2;

	if (speed == SPEED_100) {
		ecntrl = DTSEC_ECNTRL_STEN | DTSEC_ECNTRL_RGMII | DTSEC_ECNTRL_RPM | DTSEC_ECNTRL_R100M;
		maccfg2 = 0x00007106U;
	} else if (speed == SPEED_10) {
		ecntrl = DTSEC_ECNTRL_STEN | DTSEC_ECNTRL_RGMII | DTSEC_ECNTRL_RPM;
		maccfg2 = 0x00007106U;
	} else {
		/* Default 1000 Mbps RGMII (matches ROMMON 0x00001010) */
		ecntrl = DTSEC_ECNTRL_STEN | DTSEC_ECNTRL_RGMII;
		maccfg2 = 0x00007206U;
	}

	if (duplex)
		maccfg2 |= 0x00000001U; /* 0x00007207 matches ROMMON */

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
	for (i = 0; i < FM_IRAM_SIZE_WORDS; i++)
		iowrite32be(0xffffffffU, iram + IRAM_IDATA);

	iowrite32be((FM_IRAM_SIZE_WORDS - 1) * 4, iram + IRAM_IADD);
	mb();
	for (i = 0; i < 1000; i++) {
		if (ioread32be(iram + IRAM_IDATA) == 0xffffffffU)
			break;
		udelay(1);
	}

	/* 2. Enable auto-increment from 0 and write FMan v2 microcode */
	iowrite32be(IRAM_IADD_AIE, iram + IRAM_IADD);
	for (i = 0; i < 1000; i++) {
		if (ioread32be(iram + IRAM_IADD) == IRAM_IADD_AIE)
			break;
		udelay(1);
	}

	for (i = 0; i < P1023_FMAN_UCODE_WORDS; i++)
		iowrite32be(p1023_fman_ucode[i], iram + IRAM_IDATA);

	pad_words = (4U - (P1023_FMAN_UCODE_WORDS & 3U)) & 3U;
	for (i = 0; i < pad_words; i++)
		iowrite32be(0xffffffffU, iram + IRAM_IDATA);

	/* 3. Verify last microcode word landed in IRAM */
	last_off = (P1023_FMAN_UCODE_WORDS - 1) * 4;
	iowrite32be(last_off, iram + IRAM_IADD);
	mb();
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
	mb();
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
		mb();
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
		wmb();

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
	iowrite32be(0x00000000U, fpm + 0x0c);		/* fmfp_tnc = 0 in IM */
	iowrite32be(0x00000000U, fpm + 0x10);		/* fmfp_prc1 = 0 in IM */
	iowrite32be(0x00000000U, fpm + 0x14);		/* fmfp_prc2 = 0 in IM */

	/* Bind Port 8 (RX0) to RISC #0 and Port 40 (TX0) to RISC #1 */
	iowrite32be((8U << 24) | 0x00010001U, fpm + 0x04);
	iowrite32be((40U << 24) | 0x00020002U, fpm + 0x04);

	iowrite32be(0xe0000000U, fpm + 0xdc);		/* fmfp_ee: clear sticky, mask IRQ */
	iowrite32be(0xffffffffU, fpm + 0xe0);		/* fmfp_cev0 */
	iowrite32be(0xffffffffU, fpm + 0xe4);		/* fmfp_cev1 */
	iowrite32be(0xffffffffU, fpm + 0xe8);		/* fmfp_cev2 */
	iowrite32be(0xffffffffU, fpm + 0xec);		/* fmfp_cev3 */
	iowrite32be(0x00000000U, fpm + 0x1c);		/* fmfp_brkc */

	/* Enable RISC cores with Soft Force Execution (SFE = 0x08000000 | 0x0000c000) */
	iowrite32be(0x0800c000U, fpm + 0x70);		/* fmfp_rcr: enable RISC 0 & 1 with SFE */
	mb();
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
	mb();
	udelay(200);

	/* 7. BMI_RX0 Independent Mode Port Config (disabled until ndo_open) */
	iowrite32be(FMBM_RCFG_IM, priv->bmi_rx0 + 0x00);	/* fmbm_rcfg: IM=1 */
	iowrite32be(0x00100000U, priv->bmi_rx0 + 0x08);		/* fmbm_rda */
	iowrite32be(0x00ff00bfU, priv->bmi_rx0 + 0x0c);		/* fmbm_rfp */
	iowrite32be(0x00040000U, priv->bmi_rx0 + FMBM_RFED);	/* fmbm_rfed: strip 4B FCS */
	iowrite32be(0x00000001U, priv->bmi_rx0 + 0x14);		/* fmbm_ricp: 1 */
	iowrite32be(0x0000000aU, priv->bmi_rx0 + FMBM_RFNE);	/* NIA = 0x0a (IM RX) */
	iowrite32be(0x003c0000U, priv->bmi_rx0 + 0x24);		/* fmbm_rfca */
	iowrite32be(0x00d40000U, priv->bmi_rx0 + 0x70);		/* fmbm_rebm */
	iowrite32be(0xffffffffU, priv->bmi_rx0 + 0x68);		/* fmbm_rievr */
	iowrite32be(0x00000000U, priv->bmi_rx0 + 0x6c);		/* fmbm_rier */
	iowrite32be(0x80000000U, priv->bmi_rx0 + 0x200);	/* fmbm_rstc */
	iowrite32be(0x80000000U, priv->bmi_rx0 + 0x280);	/* fmbm_rpc */
	iowrite32be(priv->rx_pram_off, priv->bmi_rx0 + FMBM_RFQD);

	/* 8. BMI_TX0 Independent Mode Port Config (disabled until ndo_open) */
	iowrite32be(FMBM_TCFG_IM, priv->bmi_tx0 + 0x00);	/* fmbm_tcfg: IM=1 */
	iowrite32be(0x00000000U, priv->bmi_tx0 + 0x08);		/* fmbm_tda */
	iowrite32be(0x00001013U, priv->bmi_tx0 + 0x0c);		/* fmbm_tfp */
	iowrite32be(0x00000000U, priv->bmi_tx0 + 0x10);		/* fmbm_tfed */
	iowrite32be(0x00000008U, priv->bmi_tx0 + FMBM_TFNE);	/* NIA = 0x08 (IM TX) */
	iowrite32be(0x00040000U, priv->bmi_tx0 + 0x1c);		/* fmbm_tfca */
	iowrite32be(0x00000008U, priv->bmi_tx0 + FMBM_TCFNE);	/* NIA = 0x08 (IM TX) */
	iowrite32be(0x80000000U, priv->bmi_tx0 + 0x200);	/* fmbm_tstc */
	iowrite32be(0x80000000U, priv->bmi_tx0 + 0x280);	/* fmbm_tpc */
	iowrite32be(priv->tx_pram_off, priv->bmi_tx0 + FMBM_TCFQD);

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
	mb();
	return 0;
}

static void p1023_graceful_stop_ports(struct p1023_priv *priv)
{
	u32 val, off;
	int i;

	/* Disable dTSEC0 RX/TX first */
	val = ioread32be(priv->dtsec0 + DTSEC_MACCFG1);
	iowrite32be(val & ~(DTSEC_MACCFG1_RX_EN | DTSEC_MACCFG1_TX_EN),
		    priv->dtsec0 + DTSEC_MACCFG1);

	/* Stop BMI RX0 */
	val = ioread32be(priv->bmi_rx0 + FMBM_RCFG);
	iowrite32be(val & ~FMBM_RCFG_EN, priv->bmi_rx0 + FMBM_RCFG);
	off = ioread32be(priv->bmi_rx0 + FMBM_RFQD);
	if (off && off <= (FM_MURAM_SIZE - IM_PRAM_SIZE)) {
		val = ioread32be(priv->muram + off + IM_PRAM_MODE);
		iowrite32be(val | IM_MODE_GRACEFUL_STOP,
			    priv->muram + off + IM_PRAM_MODE);
	}
	for (i = 0; i < 5000; i++) {
		if (!(ioread32be(priv->bmi_rx0 + FMBM_RST) & FMBM_RST_BSY))
			break;
		udelay(1);
	}

	/* Stop BMI TX0 */
	val = ioread32be(priv->bmi_tx0 + FMBM_TCFG);
	iowrite32be(val & ~FMBM_TCFG_EN, priv->bmi_tx0 + FMBM_TCFG);
	off = ioread32be(priv->bmi_tx0 + FMBM_TCFQD);
	if (off && off <= (FM_MURAM_SIZE - IM_PRAM_SIZE)) {
		val = ioread32be(priv->muram + off + IM_PRAM_MODE);
		iowrite32be(val | IM_MODE_GRACEFUL_STOP,
			    priv->muram + off + IM_PRAM_MODE);
	}
	for (i = 0; i < 5000; i++) {
		if (!(ioread32be(priv->bmi_tx0 + FMBM_TST) & FMBM_TST_BSY))
			break;
		udelay(1);
	}
}

static void p1023_setup_rings_and_pram(struct p1023_priv *priv)
{
	int i;

	memset(priv->rx_bd, 0, RX_RING_SIZE * sizeof(struct fm_im_bd));
	memset(priv->tx_bd, 0, TX_RING_SIZE * sizeof(struct fm_im_bd));

	for (i = 0; i < RX_RING_SIZE; i++) {
		dma_addr_t bphys = priv->rx_bufs_dma + (i * PKT_BUF_SIZE);

		priv->rx_bd[i].buf_hi = 0;
		priv->rx_bd[i].buf_lo = cpu_to_be32((u32)bphys);
		priv->rx_bd[i].len = 0;
		priv->rx_bd[i].status = cpu_to_be16(RXBD_EMPTY);
	}
	wmb();

	priv->rx_idx = 0;
	priv->tx_idx = 0;
	priv->tx_dirty = 0;

	/* Zero and populate RX PRAM in MURAM */
	for (i = 0; i < IM_PRAM_SIZE; i += 4)
		iowrite32be(0, priv->rx_pram + i);

	iowrite32be(IM_MODE_GBL | IM_MODE_GRACEFUL_STOP, priv->rx_pram + IM_PRAM_MODE);
	iowrite32be(priv->rx_pram_off + IM_PRAM_RXQD, priv->rx_pram + IM_PRAM_RXQD_PTR);
	iowrite16be(PKT_BUF_SHIFT, priv->rx_pram + IM_PRAM_MRBLR);
	iowrite16be(0, priv->rx_pram + IM_PRAM_RXQD + QD_GEN);
	iowrite16be(0, priv->rx_pram + IM_PRAM_RXQD + QD_BD_BASE_HI);
	iowrite32be((u32)priv->rx_bd_dma, priv->rx_pram + IM_PRAM_RXQD + QD_BD_BASE_LO);
	iowrite16be(RX_RING_SIZE * sizeof(struct fm_im_bd),
		    priv->rx_pram + IM_PRAM_RXQD + QD_BD_RING_SIZE);
	iowrite16be(0, priv->rx_pram + IM_PRAM_RXQD + QD_OFFSET_IN);
	iowrite16be(0, priv->rx_pram + IM_PRAM_RXQD + QD_OFFSET_OUT);

	/* Zero and populate TX PRAM in MURAM */
	for (i = 0; i < IM_PRAM_SIZE; i += 4)
		iowrite32be(0, priv->tx_pram + i);

	iowrite32be(IM_MODE_GBL | IM_MODE_GRACEFUL_STOP, priv->tx_pram + IM_PRAM_MODE);
	iowrite32be(priv->tx_pram_off + IM_PRAM_TXQD, priv->tx_pram + IM_PRAM_TXQD_PTR);
	iowrite16be(0, priv->tx_pram + IM_PRAM_TXQD + QD_GEN);
	iowrite16be(0, priv->tx_pram + IM_PRAM_TXQD + QD_BD_BASE_HI);
	iowrite32be((u32)priv->tx_bd_dma, priv->tx_pram + IM_PRAM_TXQD + QD_BD_BASE_LO);
	iowrite16be(TX_RING_SIZE * sizeof(struct fm_im_bd),
		    priv->tx_pram + IM_PRAM_TXQD + QD_BD_RING_SIZE);
	iowrite16be(0, priv->tx_pram + IM_PRAM_TXQD + QD_OFFSET_IN);
	iowrite16be(0, priv->tx_pram + IM_PRAM_TXQD + QD_OFFSET_OUT);

	/* Bind PRAM offsets in BMI RX0 & TX0 */
	iowrite32be(priv->rx_pram_off, priv->bmi_rx0 + FMBM_RFQD);
	iowrite32be(priv->tx_pram_off, priv->bmi_tx0 + FMBM_TCFQD);
	mb();
}

static void p1023_adjust_link(struct net_device *ndev)
{
	struct p1023_priv *priv = netdev_priv(ndev);
	struct phy_device *phydev = ndev->phydev;
	bool changed = false;

	if (!phydev)
		return;

	if (phydev->link) {
		if (!priv->last_link ||
		    priv->last_speed != phydev->speed ||
		    priv->last_duplex != phydev->duplex) {
			p1023_configure_dtsec_speed(priv, phydev->speed,
						    phydev->duplex);
			priv->last_link = 1;
			priv->last_speed = phydev->speed;
			priv->last_duplex = phydev->duplex;
			changed = true;
		}
	} else if (priv->last_link) {
		priv->last_link = 0;
		priv->last_speed = 0;
		priv->last_duplex = -1;
		changed = true;
	}

	if (changed)
		phy_print_status(phydev);
}

static void p1023_reclaim_tx(struct p1023_priv *priv)
{
	while (priv->tx_dirty != priv->tx_idx) {
		struct fm_im_bd *bd = &priv->tx_bd[priv->tx_dirty];
		u16 status = be16_to_cpu(READ_ONCE(bd->status));

		if (status & TXBD_READY)
			break;

		bd->status = 0;
		bd->len = 0;
		priv->tx_dirty = (priv->tx_dirty + 1) % TX_RING_SIZE;
	}

	if (netif_queue_stopped(priv->ndev)) {
		u16 next = (priv->tx_idx + 1) % TX_RING_SIZE;

		if (next != priv->tx_dirty)
			netif_wake_queue(priv->ndev);
	}
}

static int p1023_napi_poll(struct napi_struct *napi, int budget)
{
	struct p1023_priv *priv = container_of(napi, struct p1023_priv, napi);
	struct net_device *ndev = priv->ndev;
	unsigned long flags;
	int work_done = 0;

	spin_lock_irqsave(&priv->tx_lock, flags);
	p1023_reclaim_tx(priv);
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	while (work_done < budget) {
		struct fm_im_bd *bd = &priv->rx_bd[priv->rx_idx];
		u16 status = be16_to_cpu(READ_ONCE(bd->status));
		u16 pkt_len;
		u8 *buf;

		if (status & RXBD_EMPTY)
			break;

		rmb();
		pkt_len = be16_to_cpu(READ_ONCE(bd->len));
		buf = priv->rx_bufs + (priv->rx_idx * PKT_BUF_SIZE);

		priv->last_rx_status = status;
		priv->last_rx_len = pkt_len;
		memcpy(priv->last_rx_hdr, buf, min_t(u16, pkt_len, 16));

		if ((status & RXBD_ERR_MASK) || !(status & RXBD_LAST) ||
		    pkt_len < 14 || pkt_len > PKT_BUF_SIZE) {
			priv->rx_errors++;
		} else {
			struct sk_buff *skb;

			/* BMI RX0 fmbm_rfed=0x00040000 already stripped 4B FCS */
			skb = napi_alloc_skb(napi, pkt_len);
			if (skb) {
				skb_put_data(skb, buf, pkt_len);
				skb->protocol = eth_type_trans(skb, ndev);
				napi_gro_receive(napi, skb);
				priv->rx_packets++;
				priv->rx_bytes += pkt_len;
			} else {
				priv->rx_errors++;
			}
		}

		/* Return BD to FMan */
		bd->buf_hi = 0;
		bd->buf_lo = cpu_to_be32((u32)(priv->rx_bufs_dma +
					       (priv->rx_idx * PKT_BUF_SIZE)));
		bd->len = 0;
		wmb();
		WRITE_ONCE(bd->status, cpu_to_be16(RXBD_EMPTY));
		wmb();

		priv->rx_idx = (priv->rx_idx + 1) % RX_RING_SIZE;
		iowrite16be(priv->rx_idx * sizeof(struct fm_im_bd),
			    priv->rx_pram + IM_PRAM_RXQD + QD_OFFSET_OUT);

		work_done++;
	}

	if (work_done < budget)
		napi_complete_done(napi, work_done);

	return work_done;
}

static enum hrtimer_restart p1023_poll_timer_fn(struct hrtimer *timer)
{
	struct p1023_priv *priv = container_of(timer, struct p1023_priv, poll_timer);

	if (!priv->running)
		return HRTIMER_NORESTART;

	if (!(be16_to_cpu(READ_ONCE(priv->rx_bd[priv->rx_idx].status)) & RXBD_EMPTY) ||
	    priv->tx_dirty != priv->tx_idx) {
		napi_schedule(&priv->napi);
	}

	hrtimer_forward_now(timer, ns_to_ktime(250000)); /* 250 us */
	return HRTIMER_RESTART;
}

static int p1023_open(struct net_device *ndev)
{
	struct p1023_priv *priv = netdev_priv(ndev);
	struct device_node *phy_np = NULL;
	struct phy_device *phydev = NULL;
	phy_interface_t phy_iface = PHY_INTERFACE_MODE_RGMII_ID;
	u32 val;
	int i;

	p1023_graceful_stop_ports(priv);
	p1023_setup_rings_and_pram(priv);
	p1023_program_mac_addr(priv, ndev->dev_addr);

	/* Accept all multicast & enable promiscuous so bridges/IPv6/ARP work seamlessly */
	for (i = 0; i < 8; i++)
		iowrite32be(0xffffffffU, priv->dtsec0 + DTSEC_GADDR0 + i * 4);
	val = ioread32be(priv->dtsec0 + DTSEC_RCTRL);
	iowrite32be(val | DTSEC_RCTRL_PROM, priv->dtsec0 + DTSEC_RCTRL);

	/* Clear dTSEC pending error events */
	iowrite32be(0xffffffffU, priv->dtsec0 + DTSEC_IEVENT);

	/* Connect to DT phy-handle if available and unclaimed */
	if (priv->np) {
		of_get_phy_mode(priv->np, &phy_iface);
		phy_np = of_parse_phandle(priv->np, "phy-handle", 0);
	}
	if (!phy_np) {
		phy_np = of_find_compatible_node(NULL, NULL, "marvell,88e1510");
		if (!phy_np)
			phy_np = of_find_node_by_path("/soc@e0000000/mdio@1e1120/ethernet-phy@0");
		phy_iface = PHY_INTERFACE_MODE_RGMII_ID;
	}
	if (phy_np) {
		priv->last_link = 0;
		priv->last_speed = 0;
		priv->last_duplex = -1;
		phydev = of_phy_connect(ndev, phy_np, p1023_adjust_link,
					0, phy_iface);
		of_node_put(phy_np);
		if (phydev)
			pr_info(DRV_NAME ": Connected to PHY %s (mode %s)\n",
				dev_name(&phydev->mdio.dev), phy_modes(phy_iface));
		else
			pr_warn(DRV_NAME ": Could not connect to PHY\n");
	}

	if (!phydev) {
		p1023_configure_dtsec_speed(priv, SPEED_1000, DUPLEX_FULL);
	}

	/* Clear IM_MODE_GRACEFUL_STOP and enable BMI TX0 & RX0 */
	iowrite32be(IM_MODE_GBL, priv->tx_pram + IM_PRAM_MODE);
	iowrite32be(IM_MODE_GBL, priv->rx_pram + IM_PRAM_MODE);
	mb();

	val = ioread32be(priv->bmi_tx0 + FMBM_TCFG);
	iowrite32be(val | FMBM_TCFG_IM | FMBM_TCFG_EN, priv->bmi_tx0 + FMBM_TCFG);

	val = ioread32be(priv->bmi_rx0 + FMBM_RCFG);
	iowrite32be(val | FMBM_RCFG_IM | FMBM_RCFG_EN, priv->bmi_rx0 + FMBM_RCFG);
	mb();

	/* Enable dTSEC0 RX & TX (no loopback!) */
	val = DTSEC_MACCFG1_RX_EN | DTSEC_MACCFG1_TX_EN;
	iowrite32be(val, priv->dtsec0 + DTSEC_MACCFG1);
	mb();

	priv->running = true;
	napi_enable(&priv->napi);

	if (phydev)
		phy_start(phydev);
	else
		netif_carrier_on(ndev);

	netif_start_queue(ndev);
	hrtimer_start(&priv->poll_timer, ns_to_ktime(250000), HRTIMER_MODE_REL);

	netdev_info(ndev, "P1023 FMan IM up (MAC %pM, phy=%s, rx_pram=0x%x, tx_pram=0x%x)\n",
		    ndev->dev_addr, phydev ? phydev_name(phydev) : "fixed-1000M",
		    priv->rx_pram_off, priv->tx_pram_off);
	return 0;
}

static int p1023_stop(struct net_device *ndev)
{
	struct p1023_priv *priv = netdev_priv(ndev);

	priv->running = false;
	netif_stop_queue(ndev);
	hrtimer_cancel(&priv->poll_timer);
	napi_disable(&priv->napi);

	if (ndev->phydev) {
		phy_stop(ndev->phydev);
		phy_disconnect(ndev->phydev);
	} else {
		netif_carrier_off(ndev);
	}

	p1023_graceful_stop_ports(priv);
	return 0;
}

static netdev_tx_t p1023_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct p1023_priv *priv = netdev_priv(ndev);
	unsigned long flags;
	struct fm_im_bd *bd;
	u16 idx, next_idx;
	u32 pkt_len;
	u8 *tbuf;

	if (skb->len > PKT_BUF_SIZE) {
		dev_kfree_skb_any(skb);
		priv->tx_dropped++;
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&priv->tx_lock, flags);
	p1023_reclaim_tx(priv);

	idx = priv->tx_idx;
	next_idx = (idx + 1) % TX_RING_SIZE;

	if (next_idx == priv->tx_dirty ||
	    (be16_to_cpu(READ_ONCE(priv->tx_bd[idx].status)) & TXBD_READY) ||
	    (be16_to_cpu(READ_ONCE(priv->tx_bd[next_idx].status)) & TXBD_READY)) {
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}

	tbuf = priv->tx_bufs + (idx * PKT_BUF_SIZE);
	pkt_len = skb->len;
	skb_copy_from_linear_data(skb, tbuf, skb_headlen(skb));
	if (skb_is_nonlinear(skb))
		skb_copy_bits(skb, 0, tbuf, pkt_len);

	if (pkt_len < ETH_ZLEN) {
		memset(tbuf + pkt_len, 0, ETH_ZLEN - pkt_len);
		pkt_len = ETH_ZLEN;
	}

	bd = &priv->tx_bd[idx];
	bd->buf_hi = 0;
	bd->buf_lo = cpu_to_be32((u32)(priv->tx_bufs_dma + (idx * PKT_BUF_SIZE)));
	wmb();

	/* Write status (READY | LAST) and length atomically as 32-bit word */
	WRITE_ONCE(*(u32 *)bd, cpu_to_be32(((u32)(TXBD_READY | TXBD_LAST) << 16) | pkt_len));
	wmb();

	priv->tx_idx = next_idx;
	iowrite16be(next_idx * sizeof(struct fm_im_bd),
		    priv->tx_pram + IM_PRAM_TXQD + QD_OFFSET_IN);
	mb();

	priv->tx_packets++;
	priv->tx_bytes += skb->len;

	if (((next_idx + 1) % TX_RING_SIZE) == priv->tx_dirty)
		netif_stop_queue(ndev);

	spin_unlock_irqrestore(&priv->tx_lock, flags);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static int p1023_set_mac_address(struct net_device *ndev, void *p)
{
	struct p1023_priv *priv = netdev_priv(ndev);
	int ret = eth_mac_addr(ndev, p);

	if (ret == 0)
		p1023_program_mac_addr(priv, ndev->dev_addr);
	return ret;
}

static void p1023_get_stats64(struct net_device *ndev,
			      struct rtnl_link_stats64 *stats)
{
	struct p1023_priv *priv = netdev_priv(ndev);

	stats->rx_packets = priv->rx_packets;
	stats->rx_bytes   = priv->rx_bytes;
	stats->rx_errors  = priv->rx_errors;
	stats->tx_packets = priv->tx_packets;
	stats->tx_bytes   = priv->tx_bytes;
	stats->tx_dropped = priv->tx_dropped;
}

static void p1023_set_rx_mode(struct net_device *ndev)
{
	struct p1023_priv *priv = netdev_priv(ndev);
	u32 rctrl = ioread32be(priv->dtsec0 + DTSEC_RCTRL);

	rctrl |= DTSEC_RCTRL_PROM;
	iowrite32be(rctrl, priv->dtsec0 + DTSEC_RCTRL);
}

static const struct net_device_ops p1023_netdev_ops = {
	.ndo_open		= p1023_open,
	.ndo_stop		= p1023_stop,
	.ndo_start_xmit		= p1023_start_xmit,
	.ndo_set_mac_address	= p1023_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_get_stats64	= p1023_get_stats64,
	.ndo_set_rx_mode	= p1023_set_rx_mode,
};

static int p1023_proc_show(struct seq_file *m, void *v)
{
	struct p1023_priv *priv = g_priv;
	struct p1023_snap *s;

	if (!priv)
		return 0;
	s = &priv->snap;

	seq_printf(m, "=== LIVE DRIVER & FMAN V2 STATE ===\n");
	seq_printf(m, "running=%d rx_idx=%u tx_idx=%u tx_dirty=%u\n",
		   priv->running, priv->rx_idx, priv->tx_idx, priv->tx_dirty);
	seq_printf(m, "rx_pkts=%llu rx_bytes=%llu rx_err=%llu\n",
		   priv->rx_packets, priv->rx_bytes, priv->rx_errors);
	seq_printf(m, "tx_pkts=%llu tx_bytes=%llu tx_drop=%llu\n",
		   priv->tx_packets, priv->tx_bytes, priv->tx_dropped);
	seq_printf(m, "last_rx_status=0x%04x last_rx_len=%u hdr=%16ph\n",
		   priv->last_rx_status, priv->last_rx_len, priv->last_rx_hdr);
	seq_printf(m, "fman_core: iram_ready=0x%08x fpm_rcr=0x%08x fpm_rstc=0x%08x fpm_ee=0x%08x fpm_prc=0x%08x\n",
		   ioread32be(priv->fman + FM_IRAM_OFF + IRAM_READY_REG),
		   ioread32be(priv->fman + FM_FPM_OFF + 0x70),
		   ioread32be(priv->fman + FM_FPM_OFF + 0xcc),
		   ioread32be(priv->fman + FM_FPM_OFF + 0xdc),
		   ioread32be(priv->fman + FM_FPM_OFF + 0x04));
	seq_printf(m, "dma_bmi: dmsr=0x%08x dmmr=0x%08x bmi_init=0x%08x cfg1=0x%08x cfg2=0x%08x mdio_cfg=0x%08x\n",
		   ioread32be(priv->fman + FM_DMA_OFF + 0x00),
		   ioread32be(priv->fman + FM_DMA_OFF + 0x04),
		   ioread32be(priv->fman + FM_BMI_COMMON_OFF + 0x00),
		   ioread32be(priv->fman + FM_BMI_COMMON_OFF + 0x04),
		   ioread32be(priv->fman + FM_BMI_COMMON_OFF + 0x08),
		   ioread32be(priv->fman + FM_MDIO0_OFF + 0x00));
	seq_printf(m, "bmi_ports: rx_pp=0x%08x rx_pfs=0x%08x tx_pp=0x%08x tx_pfs=0x%08x\n",
		   ioread32be(priv->fman + FM_BMI_COMMON_OFF + 0x120),
		   ioread32be(priv->fman + FM_BMI_COMMON_OFF + 0x220),
		   ioread32be(priv->fman + FM_BMI_COMMON_OFF + 0x1a0),
		   ioread32be(priv->fman + FM_BMI_COMMON_OFF + 0x2a0));
	seq_printf(m, "bmi_rx0: rcfg=0x%08x rst=0x%08x rfed=0x%08x rfne=0x%08x rfqd=0x%08x\n",
		   ioread32be(priv->bmi_rx0 + FMBM_RCFG),
		   ioread32be(priv->bmi_rx0 + FMBM_RST),
		   ioread32be(priv->bmi_rx0 + FMBM_RFED),
		   ioread32be(priv->bmi_rx0 + FMBM_RFNE),
		   ioread32be(priv->bmi_rx0 + FMBM_RFQD));
	seq_printf(m, "bmi_tx0: tcfg=0x%08x tst=0x%08x tfne=0x%08x tcfne=0x%08x tcfqd=0x%08x\n",
		   ioread32be(priv->bmi_tx0 + FMBM_TCFG),
		   ioread32be(priv->bmi_tx0 + FMBM_TST),
		   ioread32be(priv->bmi_tx0 + FMBM_TFNE),
		   ioread32be(priv->bmi_tx0 + FMBM_TCFNE),
		   ioread32be(priv->bmi_tx0 + FMBM_TCFQD));
	seq_printf(m, "rx_pram(0x%x): mode=0x%08x ptr=0x%08x mrblr=0x%04x ring=0x%04x in=0x%04x out=0x%04x bd_st=0x%04x\n",
		   priv->rx_pram_off,
		   ioread32be(priv->rx_pram + IM_PRAM_MODE),
		   ioread32be(priv->rx_pram + IM_PRAM_RXQD_PTR),
		   ioread16be(priv->rx_pram + IM_PRAM_MRBLR),
		   ioread16be(priv->rx_pram + IM_PRAM_RXQD + QD_BD_RING_SIZE),
		   ioread16be(priv->rx_pram + IM_PRAM_RXQD + QD_OFFSET_IN),
		   ioread16be(priv->rx_pram + IM_PRAM_RXQD + QD_OFFSET_OUT),
		   be16_to_cpu(priv->rx_bd[priv->rx_idx].status));
	seq_printf(m, "tx_pram(0x%x): mode=0x%08x ptr=0x%08x ring=0x%04x in=0x%04x out=0x%04x bd_st=0x%04x\n",
		   priv->tx_pram_off,
		   ioread32be(priv->tx_pram + IM_PRAM_MODE),
		   ioread32be(priv->tx_pram + IM_PRAM_TXQD_PTR),
		   ioread16be(priv->tx_pram + IM_PRAM_TXQD + QD_BD_RING_SIZE),
		   ioread16be(priv->tx_pram + IM_PRAM_TXQD + QD_OFFSET_IN),
		   ioread16be(priv->tx_pram + IM_PRAM_TXQD + QD_OFFSET_OUT),
		   be16_to_cpu(priv->tx_bd[priv->tx_idx].status));
	seq_printf(m, "dtsec0: ievent=0x%08x ecntrl=0x%08x rctrl=0x%08x tctrl=0x%08x maccfg1=0x%08x maccfg2=0x%08x\n",
		   ioread32be(priv->dtsec0 + DTSEC_IEVENT),
		   ioread32be(priv->dtsec0 + DTSEC_ECNTRL),
		   ioread32be(priv->dtsec0 + DTSEC_RCTRL),
		   ioread32be(priv->dtsec0 + DTSEC_TCTRL),
		   ioread32be(priv->dtsec0 + DTSEC_MACCFG1),
		   ioread32be(priv->dtsec0 + DTSEC_MACCFG2));
	seq_printf(m, "dtsec0 RMON: rbyt=0x%08x rpkt=0x%08x rfcs=0x%08x tbyt=0x%08x tpkt=0x%08x\n",
		   ioread32be(priv->dtsec0 + 0x29c),
		   ioread32be(priv->dtsec0 + 0x2a0),
		   ioread32be(priv->dtsec0 + 0x2a4),
		   ioread32be(priv->dtsec0 + 0x2e0),
		   ioread32be(priv->dtsec0 + 0x2e4));

	seq_printf(m, "\n=== PRE-INIT SNAPSHOT ===\n");
	seq_printf(m, "iram_ready=0x%08x fpm_rcr=0x%08x fpm_rstc=0x%08x bmi_init=0x%08x mdio_cfg=0x%08x\n",
		   s->iram_ready, s->fpm_rcr, s->fpm_rstc, s->bmi_init, s->mdio_miimcfg);
	return 0;
}

static int p1023_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, p1023_proc_show, NULL);
}

static ssize_t p1023_proc_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct p1023_priv *priv = g_priv;
	char kbuf[64];
	char cmd[32];
	u32 val1 = 0, val2 = 0;
	size_t len = min(count, sizeof(kbuf) - 1);

	if (!priv)
		return -ENODEV;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';

	if (sscanf(kbuf, "%31s %x %x", cmd, &val1, &val2) >= 2) {
		if (!strcmp(cmd, "maccfg1")) {
			iowrite32be(val1, priv->dtsec0 + DTSEC_MACCFG1);
			pr_info(DRV_NAME ": DTSEC_MACCFG1 set to 0x%08x\n", val1);
		} else if (!strcmp(cmd, "maccfg2")) {
			iowrite32be(val1, priv->dtsec0 + DTSEC_MACCFG2);
			pr_info(DRV_NAME ": DTSEC_MACCFG2 set to 0x%08x\n", val1);
		} else if (!strcmp(cmd, "ecntrl")) {
			iowrite32be(val1, priv->dtsec0 + DTSEC_ECNTRL);
			pr_info(DRV_NAME ": DTSEC_ECNTRL set to 0x%08x\n", val1);
		} else if (!strcmp(cmd, "rctrl")) {
			iowrite32be(val1, priv->dtsec0 + DTSEC_RCTRL);
			pr_info(DRV_NAME ": DTSEC_RCTRL set to 0x%08x\n", val1);
		} else if (!strcmp(cmd, "tctrl")) {
			iowrite32be(val1, priv->dtsec0 + DTSEC_TCTRL);
			pr_info(DRV_NAME ": DTSEC_TCTRL set to 0x%08x\n", val1);
		} else if (!strcmp(cmd, "dtsec")) {
			iowrite32be(val2, priv->dtsec0 + val1);
			pr_info(DRV_NAME ": DTSEC[0x%x] set to 0x%08x\n", val1, val2);
		} else if (!strcmp(cmd, "fman")) {
			iowrite32be(val2, priv->fman + val1);
			pr_info(DRV_NAME ": FMAN[0x%x] set to 0x%08x\n", val1, val2);
		}
	}
	return count;
}

static const struct proc_ops p1023_proc_ops = {
	.proc_open = p1023_proc_open,
	.proc_read = seq_read,
	.proc_write = p1023_proc_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static void p1023_extract_initial_mac(struct p1023_priv *priv, u8 *mac)
{
	u32 a1, a2;

	if (mac_str && mac_pton(mac_str, mac) && is_valid_ether_addr(mac))
		return;

	if (priv->np && !of_get_mac_address(priv->np, mac) &&
	    is_valid_ether_addr(mac))
		return;

	a1 = priv->snap.dtsec_mac1;
	a2 = priv->snap.dtsec_mac2;

	mac[0] = (a2 >> 16) & 0xff;
	mac[1] = (a2 >> 24) & 0xff;
	mac[2] = (a1 >> 0)  & 0xff;
	mac[3] = (a1 >> 8)  & 0xff;
	mac[4] = (a1 >> 16) & 0xff;
	mac[5] = (a1 >> 24) & 0xff;

	if (!is_valid_ether_addr(mac)) {
		/* Fallback locally-administered MAC if hardware registers are uninitialized */
		mac[0] = 0x02; mac[1] = 0x00; mac[2] = 0x00;
		mac[3] = 0x37; mac[4] = 0x02; mac[5] = 0x01;
	}
}

static int __init p1023_fman_im_init(void)
{
	struct platform_device *pdev;
	struct net_device *ndev;
	struct p1023_priv *priv;
	u8 mac[ETH_ALEN];
	int ret;

	pdev = platform_device_register_simple(DRV_NAME, -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_pdev;

	ndev = alloc_etherdev(sizeof(*priv));
	if (!ndev) {
		ret = -ENOMEM;
		goto err_pdev;
	}

	SET_NETDEV_DEV(ndev, &pdev->dev);
	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	priv->pdev = pdev;
	priv->np = of_find_compatible_node(NULL, NULL, "cisco,p1023-fman-im");
	if (priv->np)
		pdev->dev.of_node = priv->np;
	spin_lock_init(&priv->tx_lock);

	priv->fman = ioremap(P1023_FMAN_PHYS, P1023_FMAN_SIZE);
	if (!priv->fman) {
		ret = -ENOMEM;
		goto err_ndev;
	}

	priv->muram   = priv->fman + FM_MURAM_OFF;
	priv->bmi_rx0 = priv->fman + FM_BMI_RX0_OFF;
	priv->bmi_tx0 = priv->fman + FM_BMI_TX0_OFF;
	priv->dtsec0  = priv->fman + FM_DTSEC0_OFF;

	priv->rx_pram_off = FM_RX_PRAM_OFF;
	priv->tx_pram_off = FM_TX_PRAM_OFF;
	priv->rx_pram = priv->muram + priv->rx_pram_off;
	priv->tx_pram = priv->muram + priv->tx_pram_off;

	/* 1. Snapshot hardware registers & extract MAC before FMan soft reset */
	p1023_capture_snapshot(priv);
	p1023_extract_initial_mac(priv, mac);
	eth_hw_addr_set(ndev, mac);

	/* 2. Perform full FMan v2 + Microcode + MURAM + BMI + dTSEC0 initialization */
	ret = p1023_fman_hw_init(priv, mac);
	if (ret)
		goto err_unmap;

	/* 3. Allocate DMA coherent rings & buffers */
	priv->rx_bd = dma_alloc_coherent(&pdev->dev,
					 RX_RING_SIZE * sizeof(struct fm_im_bd),
					 &priv->rx_bd_dma, GFP_KERNEL);
	priv->tx_bd = dma_alloc_coherent(&pdev->dev,
					 TX_RING_SIZE * sizeof(struct fm_im_bd),
					 &priv->tx_bd_dma, GFP_KERNEL);
	priv->rx_bufs = dma_alloc_coherent(&pdev->dev,
					   RX_RING_SIZE * PKT_BUF_SIZE,
					   &priv->rx_bufs_dma, GFP_KERNEL);
	priv->tx_bufs = dma_alloc_coherent(&pdev->dev,
					   TX_RING_SIZE * PKT_BUF_SIZE,
					   &priv->tx_bufs_dma, GFP_KERNEL);
	if (!priv->rx_bd || !priv->tx_bd || !priv->rx_bufs || !priv->tx_bufs) {
		ret = -ENOMEM;
		goto err_dma;
	}

	ndev->netdev_ops = &p1023_netdev_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = 1500;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	netif_napi_add(ndev, &priv->napi, p1023_napi_poll);
#else
	netif_napi_add(ndev, &priv->napi, p1023_napi_poll, 64);
#endif
	hrtimer_init(&priv->poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	priv->poll_timer.function = p1023_poll_timer_fn;

	g_priv = priv;
	g_proc = proc_create("p1023_fman_regs", 0644, NULL, &p1023_proc_ops);

	ret = register_netdev(ndev);
	if (ret)
		goto err_proc;

	pr_info(DRV_NAME ": v2 registered %s (MAC %pM, iram_ready=0x%08x, fpm_rcr=0x%08x)\n",
		ndev->name, ndev->dev_addr,
		ioread32be(priv->fman + FM_IRAM_OFF + IRAM_READY_REG),
		ioread32be(priv->fman + FM_FPM_OFF + 0x70));
	return 0;

err_proc:
	if (g_proc)
		proc_remove(g_proc);
	g_priv = NULL;
	netif_napi_del(&priv->napi);
err_dma:
	if (priv->tx_bufs)
		dma_free_coherent(&pdev->dev, TX_RING_SIZE * PKT_BUF_SIZE,
				  priv->tx_bufs, priv->tx_bufs_dma);
	if (priv->rx_bufs)
		dma_free_coherent(&pdev->dev, RX_RING_SIZE * PKT_BUF_SIZE,
				  priv->rx_bufs, priv->rx_bufs_dma);
	if (priv->tx_bd)
		dma_free_coherent(&pdev->dev, TX_RING_SIZE * sizeof(struct fm_im_bd),
				  priv->tx_bd, priv->tx_bd_dma);
	if (priv->rx_bd)
		dma_free_coherent(&pdev->dev, RX_RING_SIZE * sizeof(struct fm_im_bd),
				  priv->rx_bd, priv->rx_bd_dma);
err_unmap:
	iounmap(priv->fman);
err_ndev:
	if (priv->np) {
		pdev->dev.of_node = NULL;
		of_node_put(priv->np);
	}
	free_netdev(ndev);
err_pdev:
	platform_device_unregister(pdev);
	return ret;
}

static void __exit p1023_fman_im_exit(void)
{
	struct p1023_priv *priv = g_priv;
	struct platform_device *pdev;

	if (!priv)
		return;

	pdev = priv->pdev;

	unregister_netdev(priv->ndev);
	if (g_proc)
		proc_remove(g_proc);
	g_priv = NULL;
	netif_napi_del(&priv->napi);

	dma_free_coherent(&pdev->dev, TX_RING_SIZE * PKT_BUF_SIZE,
			  priv->tx_bufs, priv->tx_bufs_dma);
	dma_free_coherent(&pdev->dev, RX_RING_SIZE * PKT_BUF_SIZE,
			  priv->rx_bufs, priv->rx_bufs_dma);
	dma_free_coherent(&pdev->dev, TX_RING_SIZE * sizeof(struct fm_im_bd),
			  priv->tx_bd, priv->tx_bd_dma);
	dma_free_coherent(&pdev->dev, RX_RING_SIZE * sizeof(struct fm_im_bd),
			  priv->rx_bd, priv->rx_bd_dma);

	iounmap(priv->fman);
	if (priv->np) {
		pdev->dev.of_node = NULL;
		of_node_put(priv->np);
	}
	free_netdev(priv->ndev);
	platform_device_unregister(pdev);
}

module_init(p1023_fman_im_init);
module_exit(p1023_fman_im_exit);

MODULE_AUTHOR("Antigravity");
MODULE_DESCRIPTION("Freescale QorIQ P1023 FMan v2 Independent Mode Ethernet Driver");
MODULE_LICENSE("GPL");
