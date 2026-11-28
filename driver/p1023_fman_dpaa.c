// SPDX-License-Identifier: GPL-2.0-or-later OR AGPL-3.0-or-later
/* Cisco AP3G2: P1023 FMan Ethernet through the Linux QMan/BMan APIs. */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/hashtable.h>
#include <linux/iopoll.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <net/page_pool.h>
#include <soc/fsl/bman.h>
#include <soc/fsl/qman.h>
#include "fman.h"
#include "p1023_fman_ucode.h"

#define DRV_NAME "p1023_fman_dpaa"
#define RX_COUNT 512
#define TX_COUNT 256
#define RX_OFFSET 64
#define RX_SIZE 2048
#define TX_OFFSET 64
#define TX_SG_COUNT 16
#define FQ_COUNT 5
#define FD_ERRORS 0x070c0000
#define PORT_ENABLE 0x80000000
#define QMI_OFFSET 0x400

enum { RX_DEFAULT, RX_ERROR, TX_CONFIRM, TX_ERROR, TX_QUEUE };

struct p1023_priv;

struct p1023_fq {
	struct qman_fq fq;
	struct p1023_priv *priv;
	int type;
};

struct p1023_buffer {
	struct hlist_node node;
	struct page *page;
	struct sk_buff *skb;
	unsigned int nents;
	void *data;
	dma_addr_t dma;
	unsigned int len;
};

struct p1023_napi {
	struct napi_struct napi;
	struct qman_portal *portal;
	u32 polls, frames, rx_csum;
	struct p1023_priv *priv;
	struct bm_buffer recycled[8];
	u8 recycled_count;
};

struct p1023_priv {
	struct net_device *ndev;
	struct device *dev;
	void __iomem *fman, *muram, *bmi_rx0, *bmi_tx0, *dtsec0;
	void __iomem *qbman, *bman_ci;
	struct p1023_napi __percpu *pcpu;
	struct bman_pool *pool;
	struct page_pool *page_pool;
	struct p1023_fq fqs[FQ_COUNT];
	struct p1023_buffer rx[RX_COUNT], tx[TX_COUNT];
	DECLARE_HASHTABLE(rx_hash, 9);
	DECLARE_HASHTABLE(tx_hash, 8);
	DECLARE_BITMAP(tx_used, TX_COUNT);
	spinlock_t tx_lock;
	unsigned int pending;
	atomic_t rx_errors, tx_errors, rx_dropped, tx_dropped;
	u32 fault, tx_sg;
	bool running, initialized;
};

#include "p1023_dpaa_hw.h"

static void p1023_fault(struct p1023_priv *priv, const char *what)
{
	if (!xchg(&priv->fault, true)) {
		netdev_err(priv->ndev, "%s; reboot required\n", what);
		netif_device_detach(priv->ndev);
	}
}

static int p1023_stop_port(void __iomem *port, bool tx)
{
	u32 status;
	int ret;

	if (tx) {
		iowrite32be(ioread32be(port + QMI_OFFSET) & ~PORT_ENABLE,
			    port + QMI_OFFSET);
		ret = read_poll_timeout(ioread32be, status, !(status & 0x20000000),
					10, 100000, false, port + QMI_OFFSET + 4);
		if (ret)
			return ret;
	}
	iowrite32be(ioread32be(port) & ~PORT_ENABLE, port);
	return read_poll_timeout(ioread32be, status, !(status & PORT_ENABLE),
				10, 100000, false, port + 4);
}

static int p1023_init_parser(struct p1023_priv *priv)
{
	void __iomem *prs = priv->bmi_rx0 + 0x800;
	u32 status;
	int i, ret;

	/* P1023 parser: the same per-port setup as the NXP FMan driver. */
	iowrite32be(ioread32be(priv->fman + 0xc7844) | 1, priv->fman + 0xc7844);
	iowrite32be(1, prs + 0x3f8);
	ret = read_poll_timeout(ioread32be, status, !(status & 0x100),
				10, 1000, false, prs + 0x3f8);
	if (ret)
		return ret;
	for (i = 0; i < 16; i++) {
		iowrite32be(0, prs + i * 8);
		iowrite32be(0xffffffff, prs + i * 8 + 4);
	}
	iowrite32be(0x80000000, prs + 0xa * 8); /* TCP: exclude Ethernet padding */
	iowrite32be(0x80000000, prs + 0xb * 8); /* UDP: exclude Ethernet padding */
	for (i = 0; i < 8; i++)
		iowrite32be(i < 4 ? 0 : 0xffffffff, priv->bmi_rx0 + 0x40 + i * 4);
	iowrite32be(0, priv->bmi_rx0 + 0x2c); /* parse from Ethernet header */
	iowrite32be(0x00500002, priv->bmi_rx0 + 0x28); /* parser -> BMI enqueue */
	iowrite32be(0, prs + 0x3f8);
	return 0;
}

static void p1023_init_ports(struct p1023_priv *priv)
{
	void __iomem *rx = priv->bmi_rx0, *tx = priv->bmi_tx0;
	unsigned int bpid = bman_get_bpid(priv->pool);
	int i;

	/* Normal BMI path; no independent-mode PRAM or buffer descriptors. */
	iowrite32be(0, rx);
	iowrite32be(0x00100000, rx + 0x08); /* DMA write optimization */
	iowrite32be(0x003f003f, rx + 0x0c); /* 16 KiB priority/threshold */
	iowrite32be(4 << 16, rx + 0x10); /* strip Ethernet FCS */
	iowrite32be(0, rx + 0x14); /* no exported internal context */
	iowrite32be(0, rx + 0x18);
	iowrite32be(RX_OFFSET << 16, rx + 0x1c);
	iowrite32be(0x00440000, rx + 0x20); /* Ethernet hardware parser */
	iowrite32be(0x803c0000, rx + 0x24); /* ordered RX attributes */
	iowrite32be(priv->fqs[RX_DEFAULT].fq.fqid, rx + 0x60);
	iowrite32be(priv->fqs[RX_ERROR].fq.fqid, rx + 0x64);
	iowrite32be(0x000c0000, rx + 0x68); /* discard physical/oversized frames */
	iowrite32be(FD_ERRORS, rx + 0x6c);
	iowrite32be(0x00d40000, rx + 0x70); /* QMI enqueue + order restoration */
	iowrite32be(0xc0000000 | (bpid << 16) | RX_SIZE, rx + 0x100);
	for (i = 1; i < 4; i++)
		iowrite32be(0, rx + 0x100 + i * 4);
	iowrite32be(0, rx + 0x180); /* no depletion pause policy */
	iowrite32be(PORT_ENABLE, rx + 0x200);
	iowrite32be(0x10000000, rx + QMI_OFFSET);
	iowrite32be(0x005000c0, rx + QMI_OFFSET + 0x1c); /* release after enqueue */

	iowrite32be(0, tx);
	iowrite32be(0, tx + 0x08);
	iowrite32be(0x00001013, tx + 0x0c);
	iowrite32be(0, tx + 0x10);
	iowrite32be(0x00000202, tx + 0x14); /* parse result: buffer 0 -> IC 32, 32 bytes */
	iowrite32be(0x00580000, tx + 0x18); /* QMI dequeue */
	iowrite32be(0x80000000, tx + 0x1c);
	iowrite32be(priv->fqs[TX_CONFIRM].fq.fqid, tx + 0x20);
	iowrite32be(priv->fqs[TX_ERROR].fq.fqid, tx + 0x24);
	iowrite32be(0x00d40000, tx + 0x28); /* QMI enqueue + order restoration */
	iowrite32be(PORT_ENABLE, tx + 0x200);
	iowrite32be(0x10000000, tx + QMI_OFFSET);
	iowrite32be(0x005002c0, tx + QMI_OFFSET + 0x1c);
	iowrite32be(0x00500274, tx + QMI_OFFSET + 0x2c); /* BMI transmit */
	/* P1023 has no dequeue-prefetch option. Subportal 0, priority order. */
	iowrite32be(0x10000400, tx + QMI_OFFSET + 0x30);
}

static void p1023_unmap_tx(struct p1023_priv *priv, struct p1023_buffer *buf)
{
	struct qm_sg_entry *sgt = buf->data + TX_OFFSET;
	int i;

	for (i = 0; i < buf->nents; i++) {
		if (i)
			dma_unmap_page(priv->dev, qm_sg_addr(&sgt[i]),
				       qm_sg_entry_get_len(&sgt[i]), DMA_TO_DEVICE);
		else
			dma_unmap_single(priv->dev, qm_sg_addr(&sgt[0]),
					 qm_sg_entry_get_len(&sgt[0]), DMA_TO_DEVICE);
	}
	buf->nents = 0;
}

static void p1023_complete_tx(struct p1023_priv *priv,
			       const struct qm_fd *fd, bool error)
{
	struct p1023_buffer *buf;
	dma_addr_t addr = qm_fd_addr(fd);
	unsigned long flags;
	unsigned int index;
	struct sk_buff *skb;

	hash_for_each_possible(priv->tx_hash, buf, node, addr) {
		if (buf->dma != addr)
			continue;
		index = buf - priv->tx;
		spin_lock_irqsave(&priv->tx_lock, flags);
		if (!test_bit(index, priv->tx_used)) {
			spin_unlock_irqrestore(&priv->tx_lock, flags);
			p1023_fault(priv, "duplicate TX completion");
			return;
		}
		dma_sync_single_range_for_cpu(priv->dev, buf->dma, 0,
					      TX_OFFSET + buf->nents * sizeof(struct qm_sg_entry),
					      DMA_TO_DEVICE);
		p1023_unmap_tx(priv, buf);
		skb = buf->skb;
		buf->skb = NULL;
		clear_bit(index, priv->tx_used);
		priv->pending--;
		netdev_completed_queue(priv->ndev, 1, buf->len);
		if (error)
			atomic_inc(&priv->tx_errors);
		else
			dev_sw_netstats_tx_add(priv->ndev, 1, buf->len);
		if (priv->running && !priv->fault && netif_queue_stopped(priv->ndev))
			netif_wake_queue(priv->ndev);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		dev_consume_skb_any(skb);
		return;
	}
	p1023_fault(priv, "unknown TX completion address");
}

static void p1023_recycle_rx(struct p1023_napi *np)
{
	int ret;

	if (!np->recycled_count)
		return;
	ret = bman_release(np->priv->pool, np->recycled, np->recycled_count);
	np->recycled_count = 0;
	if (ret)
		p1023_fault(np->priv, "BMan RX buffer return failed");
}

static void p1023_receive(struct p1023_priv *priv, struct p1023_napi *np,
			   const struct qm_fd *fd, bool error)
{
	struct p1023_buffer *buf;
	struct sk_buff *skb;
	struct page *replacement;
	dma_addr_t addr = qm_fd_addr(fd);
	unsigned int offset = qm_fd_get_offset(fd), len = qm_fd_get_length(fd);
	unsigned long flags;

	hash_for_each_possible(priv->rx_hash, buf, node, addr) {
		if (buf->dma != addr)
			continue;
		if (error || qm_fd_get_format(fd) != qm_fd_contig ||
		    len < ETH_HLEN || offset + len > RX_SIZE) {
			atomic_inc(&priv->rx_errors);
			goto recycle;
		}
		if (!READ_ONCE(priv->running) || priv->fault)
			goto recycle;
		/* The stack keeps this page; BMan gets a replacement before reuse. */
		replacement = page_pool_dev_alloc_pages(priv->page_pool);
		if (!replacement) {
			atomic_inc(&priv->rx_dropped);
			goto recycle;
		}
		dma_sync_single_range_for_cpu(priv->dev, addr, offset, len,
					      DMA_FROM_DEVICE);
		skb = napi_build_skb(buf->data, PAGE_SIZE);
		if (!skb) {
			page_pool_recycle_direct(priv->page_pool, replacement);
			dma_sync_single_range_for_device(priv->dev, addr, offset, len,
							 DMA_FROM_DEVICE);
			atomic_inc(&priv->rx_dropped);
			goto recycle;
		}
		skb_reserve(skb, offset);
		skb_put(skb, len);
		skb_mark_for_recycle(skb);
		hash_del(&buf->node);
		buf->page = replacement;
		buf->data = page_address(replacement);
		buf->dma = page_pool_get_dma_addr(replacement);
		hash_add(priv->rx_hash, &buf->node, buf->dma);
		addr = buf->dma;
		skb->protocol = eth_type_trans(skb, priv->ndev);
		if ((priv->ndev->features & NETIF_F_RXCSUM) &&
		    (be32_to_cpu(fd->status) & 4) &&
		    !(be32_to_cpu(fd->status) & 0xe8)) {
			skb->ip_summed = CHECKSUM_UNNECESSARY;
			np->rx_csum++;
		}
		local_irq_save(flags);
		dev_sw_netstats_rx_add(priv->ndev, len);
		local_irq_restore(flags);
		napi_gro_receive(&np->napi, skb);
recycle:
		bm_buffer_set64(&np->recycled[np->recycled_count++], addr);
		if (np->recycled_count == ARRAY_SIZE(np->recycled))
			p1023_recycle_rx(np);
		return;
	}
	netdev_err(priv->ndev, "RX FD addr=%pad bpid=%u cfg=%08x status=%08x\n",
		   &addr, fd->bpid, be32_to_cpu(fd->cfg), be32_to_cpu(fd->status));
	p1023_fault(priv, "unknown RX buffer address");
}

static enum qman_cb_dqrr_result p1023_dqrr(struct qman_portal *portal,
	struct qman_fq *fq, const struct qm_dqrr_entry *dq, bool sched_napi)
{
	struct p1023_fq *queue = container_of(fq, struct p1023_fq, fq);
	struct p1023_priv *priv = queue->priv;
	struct p1023_napi *np = NULL;
	int cpu;
	bool error = be32_to_cpu(dq->fd.status) & FD_ERRORS;

	for_each_cpu(cpu, qman_affine_cpus()) {
		np = per_cpu_ptr(priv->pcpu, cpu);
		if (np->portal == portal)
			break;
	}
	if (sched_napi) {
		qman_p_irqsource_remove(portal, QM_PIRQ_DQRI);
		napi_schedule(&np->napi);
		return qman_cb_dqrr_stop;
	}
	if (queue->type == RX_DEFAULT || queue->type == RX_ERROR)
		p1023_receive(priv, np, &dq->fd, error || queue->type == RX_ERROR);
	else
		p1023_complete_tx(priv, &dq->fd, error || queue->type == TX_ERROR);
	return qman_cb_dqrr_consume;
}

static void p1023_ern(struct qman_portal *portal, struct qman_fq *fq,
		       const union qm_mr_entry *msg)
{
	struct p1023_fq *queue = container_of(fq, struct p1023_fq, fq);

	p1023_complete_tx(queue->priv, &msg->ern.fd, true);
}

static int p1023_poll(struct napi_struct *napi, int budget)
{
	struct p1023_napi *np = container_of(napi, struct p1023_napi, napi);
	int done = qman_p_poll_dqrr(np->portal, budget);

	p1023_recycle_rx(np);
	np->polls++;
	np->frames += done;
	if (done < budget && napi_complete_done(napi, done))
		qman_p_irqsource_add(np->portal, QM_PIRQ_DQRI);
	return done;
}

static int p1023_create_fqs(struct p1023_priv *priv)
{
	struct qm_mcc_initfq init = {};
	struct p1023_fq *queue;
	u32 flags;
	int i, ret, channel;

	for (i = 0; i < FQ_COUNT; i++) {
		queue = &priv->fqs[i];
		queue->priv = priv;
		queue->type = i;
		queue->fq.cb.dqrr = p1023_dqrr;
		queue->fq.cb.ern = p1023_ern;
		flags = QMAN_FQ_FLAG_DYNAMIC_FQID;
		flags |= i == TX_QUEUE ? QMAN_FQ_FLAG_TO_DCPORTAL : QMAN_FQ_FLAG_NO_ENQUEUE;
		ret = qman_create_fq(0, flags, &queue->fq);
		if (ret)
			goto unwind;
		init.we_mask = cpu_to_be16(QM_INITFQ_WE_DESTWQ | QM_INITFQ_WE_FQCTRL);
		init.fqd.fq_ctrl = cpu_to_be16(QM_FQCTRL_PREFERINCACHE);
		if (i == TX_QUEUE)
			channel = 0x40;
		else if (i == TX_CONFIRM || i == TX_ERROR)
			channel = qman_affine_channel(cpumask_last(qman_affine_cpus()));
		else
			channel = qman_affine_channel(0);
		qm_fqd_set_destwq(&init.fqd, channel, 4);
		/* Keep queues parked until all allocation/registration has succeeded. */
		ret = qman_init_fq(&queue->fq, 0, &init);
		if (ret) {
			qman_destroy_fq(&queue->fq);
			goto unwind;
		}
	}
	return 0;
unwind:
	while (--i >= 0)
		qman_destroy_fq(&priv->fqs[i].fq);
	return ret;
}

static void p1023_free_buffers(struct p1023_priv *priv)
{
	struct p1023_buffer *buf;
	int i;

	for (i = 0; i < RX_COUNT; i++)
		if (priv->rx[i].page)
			page_pool_put_full_page(priv->page_pool, priv->rx[i].page, false);
	page_pool_destroy(priv->page_pool);
	for (i = 0; i < TX_COUNT; i++) {
		buf = &priv->tx[i];
		if (!buf->data)
			continue;
		dma_unmap_single(priv->dev, buf->dma, PAGE_SIZE, DMA_TO_DEVICE);
		free_page((unsigned long)buf->data);
	}
}

static int p1023_alloc_buffers(struct p1023_priv *priv)
{
	struct page_pool_params params = {
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.pool_size = RX_COUNT,
		.nid = NUMA_NO_NODE,
		.dev = priv->dev,
		.dma_dir = DMA_FROM_DEVICE,
		.offset = RX_OFFSET,
		.max_len = RX_SIZE - RX_OFFSET,
	};
	struct p1023_buffer *buf;
	int i;

	BUILD_BUG_ON(SKB_WITH_OVERHEAD(PAGE_SIZE) < RX_SIZE);
	priv->page_pool = page_pool_create(&params);
	if (IS_ERR(priv->page_pool))
		return PTR_ERR(priv->page_pool);
	for (i = 0; i < RX_COUNT; i++) {
		buf = &priv->rx[i];
		buf->page = page_pool_alloc_pages(priv->page_pool, GFP_KERNEL);
		if (!buf->page)
			goto fail;
		buf->data = page_address(buf->page);
		buf->dma = page_pool_get_dma_addr(buf->page);
		hash_add(priv->rx_hash, &buf->node, buf->dma);
	}
	for (i = 0; i < TX_COUNT; i++) {
		buf = &priv->tx[i];
		buf->data = (void *)get_zeroed_page(GFP_KERNEL);
		if (!buf->data)
			goto fail;
		buf->dma = dma_map_single(priv->dev, buf->data, PAGE_SIZE, DMA_TO_DEVICE);
		if (dma_mapping_error(priv->dev, buf->dma)) {
			free_page((unsigned long)buf->data);
			buf->data = NULL;
			goto fail;
		}
		hash_add(priv->tx_hash, &buf->node, buf->dma);
		dma_sync_single_for_cpu(priv->dev, buf->dma, PAGE_SIZE, DMA_TO_DEVICE);
	}
	return 0;
fail:
	p1023_free_buffers(priv);
	return -ENOMEM;
}

static int p1023_tx_csum(struct sk_buff *skb, struct fman_prs_result *prs,
			  struct qm_fd *fd)
{
	u16 protocol = ntohs(vlan_get_protocol(skb));
	u8 l4;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;
	if (skb->encapsulation)
		return skb_checksum_help(skb);
	if (protocol == ETH_P_IP)
		l4 = ip_hdr(skb)->protocol;
	else if (protocol == ETH_P_IPV6)
		l4 = ipv6_hdr(skb)->nexthdr;
	else
		return skb_checksum_help(skb);
	if ((l4 != IPPROTO_TCP && l4 != IPPROTO_UDP) ||
	    skb_network_offset(skb) > 255 || skb_transport_offset(skb) > 255)
		return skb_checksum_help(skb);

	/* Same parse-result format and command as the NXP DPAA driver. */
	memset(prs, 0, sizeof(*prs));
	prs->l3r = cpu_to_be16(protocol == ETH_P_IP ? 0x8000 : 0x4000);
	prs->l4r = l4 == IPPROTO_TCP ? 0x20 : 0x40;
	prs->ip_off[0] = skb_network_offset(skb);
	prs->l4_off = skb_transport_offset(skb);
	fd->cmd |= cpu_to_be32(FM_FD_CMD_RPD | FM_FD_CMD_DTC);
	return 0;
}

static int p1023_map_tx(struct p1023_priv *priv, struct p1023_buffer *buf,
			 struct sk_buff *skb)
{
	struct qm_sg_entry *sgt = buf->data + TX_OFFSET;
	dma_addr_t addr;
	unsigned int len;
	int i;

	/* FMan accepts at most 16 entries, including the linear head. */
	if (skb_shinfo(skb)->nr_frags >= TX_SG_COUNT && __skb_linearize(skb))
		return -ENOMEM;
	buf->nents = 0;
	for (i = 0; i <= skb_shinfo(skb)->nr_frags; i++) {
		if (i) {
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i - 1];

			len = skb_frag_size(frag);
			addr = skb_frag_dma_map(priv->dev, frag, 0, len, DMA_TO_DEVICE);
		} else {
			len = skb_headlen(skb);
			addr = dma_map_single(priv->dev, skb->data, len, DMA_TO_DEVICE);
		}
		if (dma_mapping_error(priv->dev, addr)) {
			p1023_unmap_tx(priv, buf);
			return -ENOMEM;
		}
		sgt[i] = (struct qm_sg_entry){};
		qm_sg_entry_set64(&sgt[i], addr);
		qm_sg_entry_set_len(&sgt[i], len);
		sgt[i].bpid = 0xff;
		buf->nents++;
	}
	qm_sg_entry_set_f(&sgt[i - 1], len);
	return 0;
}

static netdev_tx_t p1023_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct p1023_priv *priv = netdev_priv(ndev);
	struct p1023_buffer *buf;
	struct qm_fd fd = {};
	unsigned long flags;
	unsigned int index;
	int ret;

	spin_lock_irqsave(&priv->tx_lock, flags);
	index = find_first_zero_bit(priv->tx_used, TX_COUNT);
	if (index == TX_COUNT) {
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}
	if (skb_put_padto(skb, ETH_ZLEN)) {
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_OK;
	}
	buf = &priv->tx[index];
	if (p1023_tx_csum(skb, buf->data, &fd) || p1023_map_tx(priv, buf, skb))
		goto drop;
	buf->len = skb->len;
	qm_fd_addr_set64(&fd, buf->dma);
	qm_fd_set_sg(&fd, TX_OFFSET, buf->len);
	fd.bpid = 0xff;
	fd.cmd |= cpu_to_be32(FM_FD_CMD_FCO | priv->fqs[TX_CONFIRM].fq.fqid);
	dma_sync_single_range_for_device(priv->dev, buf->dma, 0,
					 TX_OFFSET + buf->nents * sizeof(struct qm_sg_entry),
					 DMA_TO_DEVICE);
	ret = qman_enqueue(&priv->fqs[TX_QUEUE].fq, &fd);
	if (ret) {
		dma_sync_single_range_for_cpu(priv->dev, buf->dma, 0,
					      TX_OFFSET + buf->nents * sizeof(struct qm_sg_entry),
					      DMA_TO_DEVICE);
		p1023_unmap_tx(priv, buf);
		if (ret == -EBUSY && priv->pending) {
			netif_stop_queue(ndev);
			spin_unlock_irqrestore(&priv->tx_lock, flags);
			return NETDEV_TX_BUSY; /* pending completions wake the queue */
		}
		goto drop;
	}
	if (buf->nents > 1)
		priv->tx_sg++;
	buf->skb = skb;
	set_bit(index, priv->tx_used);
	priv->pending++;
	netdev_sent_queue(ndev, buf->len);
	if (priv->pending == TX_COUNT)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return NETDEV_TX_OK;
drop:
	atomic_inc(&priv->tx_dropped);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void p1023_adjust_link(struct net_device *ndev)
{
	struct p1023_priv *priv = netdev_priv(ndev);
	struct phy_device *phy = ndev->phydev;

	if (phy->link) {
		p1023_configure_dtsec_speed(priv, phy->speed, phy->duplex);
		netif_carrier_on(ndev);
	} else {
		netif_carrier_off(ndev);
	}
	phy_print_status(phy);
}

static int p1023_activate_queues(struct p1023_priv *priv)
{
	struct bm_buffer buffers[8] = {};
	int i, j, ret;

	for (i = 0; i < FQ_COUNT; i++) {
		ret = qman_schedule_fq(&priv->fqs[i].fq);
		if (ret)
			goto fail;
	}
	for (i = 0; i < RX_COUNT; i += 8) {
		for (j = 0; j < 8; j++)
			bm_buffer_set64(&buffers[j], priv->rx[i + j].dma);
		ret = bman_release(priv->pool, buffers, 8);
		if (ret)
			goto fail;
	}
	priv->initialized = true;
	return 0;
fail:
	p1023_fault(priv, "DPAA queue/pool initialization failed");
	return ret;
}

static int p1023_open(struct net_device *ndev)
{
	struct p1023_priv *priv = netdev_priv(ndev);
	int ret;

	if (priv->fault)
		return -EIO;
	if (!priv->initialized) {
		ret = p1023_activate_queues(priv);
		if (ret)
			return ret;
	}
	p1023_program_mac_addr(priv, ndev->dev_addr);
	WRITE_ONCE(priv->running, true);
	iowrite32be(0x90000000, priv->bmi_tx0 + QMI_OFFSET);
	iowrite32be(PORT_ENABLE, priv->bmi_tx0);
	iowrite32be(PORT_ENABLE, priv->bmi_rx0);
	iowrite32be(DTSEC_MACCFG1_RX_EN | DTSEC_MACCFG1_TX_EN,
		    priv->dtsec0 + DTSEC_MACCFG1);
	phy_start(ndev->phydev);
	netif_start_queue(ndev);
	return 0;
}

static int p1023_stop(struct net_device *ndev)
{
	struct p1023_priv *priv = netdev_priv(ndev);
	int i;

	WRITE_ONCE(priv->running, false);
	netif_tx_disable(ndev);
	phy_stop(ndev->phydev);
	netif_carrier_off(ndev);
	iowrite32be(DTSEC_MACCFG1_TX_EN, priv->dtsec0 + DTSEC_MACCFG1);
	if (p1023_stop_port(priv->bmi_rx0, false))
		p1023_fault(priv, "RX stop timed out");
	/* NAPI stays alive to receive the last TX confirmation and drain RX. */
	for (i = 0; i < 50 && READ_ONCE(priv->pending); i++)
		msleep(20);
	if (READ_ONCE(priv->pending))
		p1023_fault(priv, "TX completion timed out");
	if (p1023_stop_port(priv->bmi_tx0, true))
		p1023_fault(priv, "TX stop timed out");
	iowrite32be(0x10000000, priv->bmi_tx0 + QMI_OFFSET);
	iowrite32be(0x10000000, priv->bmi_rx0 + QMI_OFFSET);
	iowrite32be(0, priv->dtsec0 + DTSEC_MACCFG1);
	return 0;
}

static int p1023_set_mac(struct net_device *ndev, void *addr)
{
	struct p1023_priv *priv = netdev_priv(ndev);
	int ret = eth_mac_addr(ndev, addr);

	if (!ret)
		p1023_program_mac_addr(priv, ndev->dev_addr);
	return ret;
}

static void p1023_stats(struct net_device *ndev, struct rtnl_link_stats64 *stats)
{
	struct p1023_priv *priv = netdev_priv(ndev);

	dev_get_tstats64(ndev, stats);
	stats->rx_errors = atomic_read(&priv->rx_errors);
	stats->tx_errors = atomic_read(&priv->tx_errors);
	stats->rx_dropped = atomic_read(&priv->rx_dropped);
	stats->tx_dropped = atomic_read(&priv->tx_dropped);
}

static void p1023_tx_timeout(struct net_device *ndev, unsigned int queue)
{
	p1023_fault(netdev_priv(ndev), "TX queue stalled");
}

static const struct net_device_ops p1023_ops = {
	.ndo_open = p1023_open,
	.ndo_stop = p1023_stop,
	.ndo_start_xmit = p1023_xmit,
	.ndo_tx_timeout = p1023_tx_timeout,
	.ndo_set_mac_address = p1023_set_mac,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_get_stats64 = p1023_stats,
};

static const struct ethtool_ops p1023_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = phy_ethtool_get_link_ksettings,
	.set_link_ksettings = phy_ethtool_set_link_ksettings,
};

static int p1023_regs_show(struct seq_file *m, void *unused)
{
	struct p1023_priv *priv = m->private;
	struct p1023_napi *np;
	struct qm_mcr_queryfq_np state;
	void __iomem *ci;
	int cpu, i, ret;

	seq_printf(m, "mode=dpaa running=%d fault=%d pool=%d tx_pending=%u tx_sg=%u\n",
		   READ_ONCE(priv->running), READ_ONCE(priv->fault),
		   bman_get_bpid(priv->pool), READ_ONCE(priv->pending), READ_ONCE(priv->tx_sg));
	seq_printf(m, "rx cfg=%08x status=%08x nia=%08x fq=%u error_fq=%u\n",
		   ioread32be(priv->bmi_rx0), ioread32be(priv->bmi_rx0 + 4),
		   ioread32be(priv->bmi_rx0 + 0x20),
		   priv->fqs[RX_DEFAULT].fq.fqid, priv->fqs[RX_ERROR].fq.fqid);
	seq_printf(m, "tx cfg=%08x status=%08x nia=%08x fq=%u confirm_fq=%u error_fq=%u\n",
		   ioread32be(priv->bmi_tx0), ioread32be(priv->bmi_tx0 + 4),
		   ioread32be(priv->bmi_tx0 + 0x18), priv->fqs[TX_QUEUE].fq.fqid,
		   priv->fqs[TX_CONFIRM].fq.fqid, priv->fqs[TX_ERROR].fq.fqid);
	seq_printf(m, "qman_error=%08x bman_error=%08x bman_fbpr_fpc=%08x\n",
		   ioread32be(priv->qbman + 0xe00), ioread32be(priv->qbman + 0x2e00),
		   ioread32be(priv->qbman + 0x2800));
	for (i = 0; i < FQ_COUNT; i++) {
		ret = qman_query_fq_np(&priv->fqs[i].fq, &state);
		if (ret)
			seq_printf(m, "fq=%u query_error=%d\n", priv->fqs[i].fq.fqid, ret);
		else
			seq_printf(m, "fq=%u state=%02x frames=%u\n", priv->fqs[i].fq.fqid,
				   state.state, qm_mcr_np_get(&state, frm_cnt));
	}
	for_each_cpu(cpu, qman_affine_cpus()) {
		np = per_cpu_ptr(priv->pcpu, cpu);
		seq_printf(m, "portal_cpu=%d napi_polls=%u frames=%u rx_csum=%u\n", cpu,
			   READ_ONCE(np->polls), READ_ONCE(np->frames),
			   READ_ONCE(np->rx_csum));
		ci = priv->bman_ci + cpu * 0x1000;
		seq_printf(m, "bman_cpu=%d rcr_pi=%08x rcr_ci=%08x isr=%08x ier=%08x\n",
			   cpu, ioread32be(ci), ioread32be(ci + 4),
			   ioread32be(ci + 0xe00), ioread32be(ci + 0xe04));
	}
	return 0;
}

static int p1023_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *phy_np;
	struct net_device *ndev;
	struct p1023_priv *priv;
	struct p1023_napi *np;
	u32 a1, a2;
	u8 mac[ETH_ALEN];
	int cpu, i, ret;

	if (qman_is_probed() < 0 || bman_is_probed() < 0 ||
	    qman_portals_probed() < 0 || bman_portals_probed() < 0)
		return -ENODEV;
	if (!qman_portals_probed() || !bman_portals_probed())
		return -EPROBE_DEFER;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ndev = alloc_etherdev(sizeof(*priv));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, dev);
	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	priv->dev = dev;
	spin_lock_init(&priv->tx_lock);
	hash_init(priv->rx_hash);
	hash_init(priv->tx_hash);
	priv->fman = devm_ioremap(dev, P1023_FMAN_PHYS, P1023_FMAN_SIZE);
	priv->qbman = devm_ioremap(dev, 0xe0088000, 0x3000);
	priv->bman_ci = devm_ioremap(dev, 0xff300000, 0x2000);
	if (!priv->fman || !priv->qbman || !priv->bman_ci) {
		ret = -ENOMEM;
		goto free_netdev;
	}
	priv->muram = priv->fman + FM_MURAM_OFF;
	priv->bmi_rx0 = priv->fman + FM_BMI_RX0_OFF;
	priv->bmi_tx0 = priv->fman + FM_BMI_TX0_OFF;
	priv->dtsec0 = priv->fman + FM_DTSEC0_OFF;
	a1 = ioread32be(priv->dtsec0 + DTSEC_MACSTNADDR1);
	a2 = ioread32be(priv->dtsec0 + DTSEC_MACSTNADDR2);
	mac[0] = a2 >> 16; mac[1] = a2 >> 24;
	mac[2] = a1; mac[3] = a1 >> 8; mac[4] = a1 >> 16; mac[5] = a1 >> 24;
	if (is_valid_ether_addr(mac)) {
		eth_hw_addr_set(ndev, mac);
	} else {
		/* Userspace can supply the EEPROM MAC before opening the interface. */
		eth_hw_addr_random(ndev);
		dev_info(dev, "no ROMMON MAC; using a random address\n");
	}
	iowrite32be(0, priv->dtsec0 + DTSEC_MACCFG1);
	ret = p1023_stop_port(priv->bmi_rx0, false);
	if (!ret)
		ret = p1023_stop_port(priv->bmi_tx0, true);
	if (!ret)
		ret = p1023_fman_hw_init(priv, ndev->dev_addr);
	if (ret)
		goto free_netdev;
	ndev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
	priv->pcpu = alloc_percpu(struct p1023_napi);
	if (!ndev->tstats || !priv->pcpu) {
		ret = -ENOMEM;
		goto free_pcpu;
	}
	ret = p1023_alloc_buffers(priv);
	if (ret)
		goto free_pcpu;
	priv->pool = bman_new_pool();
	if (!priv->pool) {
		ret = -ENOMEM;
		goto free_buffers;
	}
	ret = p1023_create_fqs(priv);
	if (ret)
		goto free_pool;
	p1023_init_ports(priv);
	ret = p1023_init_parser(priv);
	if (ret)
		goto free_fqs;
	phy_np = of_parse_phandle(dev->of_node, "phy-handle", 0);
	if (!phy_np) {
		ret = -EINVAL;
		goto free_fqs;
	}
	if (!of_phy_connect(ndev, phy_np, p1023_adjust_link, 0,
			    PHY_INTERFACE_MODE_RGMII_ID)) {
		of_node_put(phy_np);
		ret = -EPROBE_DEFER;
		goto free_fqs;
	}
	of_node_put(phy_np);
	ndev->netdev_ops = &p1023_ops;
	ndev->ethtool_ops = &p1023_ethtool_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = 1500;
	ndev->hw_features = NETIF_F_SG | NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM | NETIF_F_RXCSUM;
	ndev->features |= ndev->hw_features;
	ndev->vlan_features = ndev->hw_features;
	/* SG also works for tunnels; encapsulated checksums stay in software. */
	ndev->hw_enc_features = NETIF_F_SG;
#ifdef CONFIG_BQL
	netdev_get_tx_queue(ndev, 0)->dql.min_limit = 32768;
#endif
	ndev->watchdog_timeo = msecs_to_jiffies(2000);
	netif_carrier_off(ndev);
	for_each_cpu(cpu, qman_affine_cpus()) {
		np = per_cpu_ptr(priv->pcpu, cpu);
		np->portal = qman_get_affine_portal(cpu);
		np->priv = priv;
		netif_napi_add(ndev, &np->napi, p1023_poll, 64);
		napi_enable(&np->napi);
	}
	ret = register_netdev(ndev);
	if (ret)
		goto del_napi;
	platform_set_drvdata(pdev, ndev);
	if (!proc_create_single_data("p1023_fman_regs", 0444, NULL, p1023_regs_show, priv))
		netdev_warn(ndev, "register diagnostics unavailable\n");
	netdev_info(ndev, "DPAA: BMan pool %d, RX/TX confirmations via QMan IRQ/NAPI\n",
		    bman_get_bpid(priv->pool));
	return 0;
del_napi:
	for_each_cpu(cpu, qman_affine_cpus()) {
		np = per_cpu_ptr(priv->pcpu, cpu);
		napi_disable(&np->napi);
		netif_napi_del(&np->napi);
	}
	phy_disconnect(ndev->phydev);
free_fqs:
	for (i = 0; i < FQ_COUNT; i++)
		qman_destroy_fq(&priv->fqs[i].fq);
free_pool:
	bman_free_pool(priv->pool);
free_buffers:
	p1023_free_buffers(priv);
free_pcpu:
	free_percpu(priv->pcpu);
	free_percpu(ndev->tstats);
free_netdev:
	free_netdev(ndev);
	return ret;
}

static void p1023_shutdown(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);

	if (ndev && netif_running(ndev))
		p1023_stop(ndev);
}

static const struct of_device_id p1023_ids[] = {
	{ .compatible = "cisco,p1023-fman-dpaa" }, {}
};
MODULE_DEVICE_TABLE(of, p1023_ids);
static struct platform_driver p1023_driver = {
	.probe = p1023_probe,
	.shutdown = p1023_shutdown,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = p1023_ids,
		.suppress_bind_attrs = true,
	},
};

/* The board's QMan/BMan controllers and these FQs have boot lifetime. */
static int __init p1023_init(void)
{
	return platform_driver_register(&p1023_driver);
}
module_init(p1023_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cisco P1023 FMan DPAA Ethernet");
