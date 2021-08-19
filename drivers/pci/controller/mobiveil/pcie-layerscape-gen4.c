// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe Gen4 host controller driver for NXP Layerscape SoCs
 *
 * Copyright 2019 NXP
 *
 * Author: Zhiqiang Hou <Zhiqiang.Hou@nxp.com>
 */

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#include "pcie-mobiveil.h"

#define REV_1_0				(0x10)

/* LUT and PF control registers */
#define PCIE_LUT_OFF			0x80000
#define PCIE_LUT_GCR			(0x28)
#define PCIE_LUT_GCR_RRE		(0)
#define PCIE_PF_OFF			0xc0000
#define PCIE_PF_INT_STAT		0x18
#define PF_INT_STAT_PABRST		BIT(31)

#define PCIE_PF_DBG			0x7fc
#define PF_DBG_LTSSM_MASK		0x3f
#define PF_DBG_LTSSM_L0			0x2d /* L0 state */
#define PF_DBG_WE			BIT(31)
#define PF_DBG_PABR			BIT(27)

#define to_ls_pcie_g4(x)		platform_get_drvdata((x)->pdev)

struct ls_pcie_g4 {
	struct mobiveil_pcie pci;
	struct delayed_work dwork;
	int irq;
	u8 rev;
};

static inline u32 ls_pcie_g4_lut_readl(struct ls_pcie_g4 *pcie, u32 off)
{
	return ioread32(pcie->pci.csr_axi_slave_base + PCIE_LUT_OFF + off);
}

static inline void ls_pcie_g4_lut_writel(struct ls_pcie_g4 *pcie,
					 u32 off, u32 val)
{
	iowrite32(val, pcie->pci.csr_axi_slave_base + PCIE_LUT_OFF + off);
}

static inline u32 ls_pcie_g4_pf_readl(struct ls_pcie_g4 *pcie, u32 off)
{
	return ioread32(pcie->pci.csr_axi_slave_base + PCIE_PF_OFF + off);
}

static inline void ls_pcie_g4_pf_writel(struct ls_pcie_g4 *pcie,
					u32 off, u32 val)
{
	iowrite32(val, pcie->pci.csr_axi_slave_base + PCIE_PF_OFF + off);
}

static bool ls_pcie_g4_is_bridge(struct ls_pcie_g4 *pcie)
{
	struct mobiveil_pcie *mv_pci = &pcie->pci;
	u32 header_type;

	header_type = csr_readb(mv_pci, PCI_HEADER_TYPE);
	header_type &= 0x7f;

	return header_type == PCI_HEADER_TYPE_BRIDGE;
}

static void workaround_A011451(struct ls_pcie_g4 *pcie)
{
	struct mobiveil_pcie *mv_pci = &pcie->pci;
	u32 val;

	/* Set ACK latency timeout */
	val = csr_readl(mv_pci, GPEX_ACK_REPLAY_TO);
	val &= ~(ACK_LAT_TO_VAL_MASK << ACK_LAT_TO_VAL_SHIFT);
	val |= (4 << ACK_LAT_TO_VAL_SHIFT);
	csr_writel(mv_pci, val, GPEX_ACK_REPLAY_TO);
}

static int ls_pcie_g4_host_init(struct mobiveil_pcie *pci)
{
	struct ls_pcie_g4 *pcie = to_ls_pcie_g4(pci);

	pcie->rev = csr_readb(pci, PCI_REVISION_ID);

	if (pcie->rev == REV_1_0)
		workaround_A011451(pcie);

	return 0;
}

static int ls_pcie_g4_link_up(struct mobiveil_pcie *pci)
{
	struct ls_pcie_g4 *pcie = to_ls_pcie_g4(pci);
	u32 state;

	state = ls_pcie_g4_pf_readl(pcie, PCIE_PF_DBG);
	state =	state & PF_DBG_LTSSM_MASK;

	if (state == PF_DBG_LTSSM_L0)
		return 1;

	return 0;
}

static void ls_pcie_g4_disable_interrupt(struct ls_pcie_g4 *pcie)
{
	struct mobiveil_pcie *mv_pci = &pcie->pci;

	csr_writel(mv_pci, 0, PAB_INTP_AMBA_MISC_ENB);
}

static void ls_pcie_g4_enable_interrupt(struct ls_pcie_g4 *pcie)
{
	struct mobiveil_pcie *mv_pci = &pcie->pci;
	u32 val;

	/* Clear the interrupt status */
	csr_writel(mv_pci, 0xffffffff, PAB_INTP_AMBA_MISC_STAT);

	val = PAB_INTP_INTX_MASK | PAB_INTP_MSI | PAB_INTP_RESET |
	      PAB_INTP_PCIE_UE | PAB_INTP_IE_PMREDI | PAB_INTP_IE_EC;
	csr_writel(mv_pci, val, PAB_INTP_AMBA_MISC_ENB);
}

static void ls_pcie_g4_reinit_hw(struct ls_pcie_g4 *pcie)
{
	struct mobiveil_pcie *mv_pci = &pcie->pci;
	struct device *dev = &mv_pci->pdev->dev;
	u32 val, act_stat;
	int to = 100;

	/* Poll for pab_csb_reset to set and PAB activity to clear */
	do {
		usleep_range(10, 15);
		val = ls_pcie_g4_pf_readl(pcie, PCIE_PF_INT_STAT);
		act_stat = csr_readl(mv_pci, PAB_ACTIVITY_STAT);
	} while (((val & PF_INT_STAT_PABRST) == 0 || act_stat) && to--);
	if (to < 0) {
		dev_err(dev, "Poll PABRST&PABACT timeout\n");
		return;
	}

	/* clear PEX_RESET bit in PEX_PF0_DBG register */
	val = ls_pcie_g4_pf_readl(pcie, PCIE_PF_DBG);
	val |= PF_DBG_WE;
	ls_pcie_g4_pf_writel(pcie, PCIE_PF_DBG, val);

	val = ls_pcie_g4_pf_readl(pcie, PCIE_PF_DBG);
	val |= PF_DBG_PABR;
	ls_pcie_g4_pf_writel(pcie, PCIE_PF_DBG, val);

	val = ls_pcie_g4_pf_readl(pcie, PCIE_PF_DBG);
	val &= ~PF_DBG_WE;
	ls_pcie_g4_pf_writel(pcie, PCIE_PF_DBG, val);

	mobiveil_host_init(mv_pci, true);

	to = 100;
	while (!ls_pcie_g4_link_up(mv_pci) && to--)
		usleep_range(200, 250);
	if (to < 0)
		dev_err(dev, "PCIe link training timeout\n");
}

static irqreturn_t ls_pcie_g4_isr(int irq, void *dev_id)
{
	struct ls_pcie_g4 *pcie = (struct ls_pcie_g4 *)dev_id;
	struct mobiveil_pcie *mv_pci = &pcie->pci;
	u32 val;

	val = csr_readl(mv_pci, PAB_INTP_AMBA_MISC_STAT);
	if (!val)
		return IRQ_NONE;

	if (val & PAB_INTP_RESET) {
		ls_pcie_g4_disable_interrupt(pcie);
		schedule_delayed_work(&pcie->dwork, msecs_to_jiffies(1));
	}

	csr_writel(mv_pci, val, PAB_INTP_AMBA_MISC_STAT);

	return IRQ_HANDLED;
}

static int ls_pcie_g4_interrupt_init(struct mobiveil_pcie *mv_pci)
{
	struct ls_pcie_g4 *pcie = to_ls_pcie_g4(mv_pci);
	struct platform_device *pdev = mv_pci->pdev;
	struct device *dev = &pdev->dev;
	int ret;

	pcie->irq = platform_get_irq_byname(pdev, "intr");
	if (pcie->irq < 0) {
		dev_err(dev, "Can't get 'intr' IRQ, errno = %d\n", pcie->irq);
		return pcie->irq;
	}
	ret = devm_request_irq(dev, pcie->irq, ls_pcie_g4_isr,
			       IRQF_SHARED, pdev->name, pcie);
	if (ret) {
		dev_err(dev, "Can't register PCIe IRQ, errno = %d\n", ret);
		return  ret;
	}

	return 0;
}

static void ls_pcie_g4_reset(struct work_struct *work)
{
	struct delayed_work *dwork = container_of(work, struct delayed_work,
						  work);
	struct ls_pcie_g4 *pcie = container_of(dwork, struct ls_pcie_g4, dwork);
	struct mobiveil_pcie *mv_pci = &pcie->pci;
	u16 ctrl;

	ctrl = csr_readw(mv_pci, PCI_BRIDGE_CONTROL);
	ctrl &= ~PCI_BRIDGE_CTL_BUS_RESET;
	csr_writew(mv_pci, ctrl, PCI_BRIDGE_CONTROL);
	ls_pcie_g4_reinit_hw(pcie);
	ls_pcie_g4_enable_interrupt(pcie);
}

static int ls_pcie_g4_read_other_conf(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	struct mobiveil_pcie *pci = bus->sysdata;
	struct ls_pcie_g4 *pcie = to_ls_pcie_g4(pci);
	int ret;

	if (pcie->rev == REV_1_0 && where == PCI_VENDOR_ID)
		ls_pcie_g4_lut_writel(pcie, PCIE_LUT_GCR,
				      0 << PCIE_LUT_GCR_RRE);

	ret = pci_generic_config_read(bus, devfn, where, size, val);

	if (pcie->rev == REV_1_0 && where == PCI_VENDOR_ID)
		ls_pcie_g4_lut_writel(pcie, PCIE_LUT_GCR,
				      1 << PCIE_LUT_GCR_RRE);

	return ret;
}

static struct mobiveil_rp_ops ls_pcie_g4_rp_ops = {
	.interrupt_init = ls_pcie_g4_interrupt_init,
	.read_other_conf = ls_pcie_g4_read_other_conf,
};

static const struct mobiveil_pab_ops ls_pcie_g4_pab_ops = {
	.link_up = ls_pcie_g4_link_up,
	.host_init = ls_pcie_g4_host_init,
};

static int __init ls_pcie_g4_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct mobiveil_pcie *mv_pci;
	struct ls_pcie_g4 *pcie;
	struct device_node *np = dev->of_node;
	int ret;

	if (!of_parse_phandle(np, "msi-parent", 0)) {
		dev_err(dev, "Failed to find msi-parent\n");
		return -EINVAL;
	}

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;

	pcie = pci_host_bridge_priv(bridge);
	mv_pci = &pcie->pci;

	mv_pci->pdev = pdev;
	mv_pci->ops = &ls_pcie_g4_pab_ops;
	mv_pci->rp.ops = &ls_pcie_g4_rp_ops;
	mv_pci->bridge = bridge;

	platform_set_drvdata(pdev, pcie);

	INIT_DELAYED_WORK(&pcie->dwork, ls_pcie_g4_reset);

	ret = mobiveil_pcie_host_probe(mv_pci);
	if (ret) {
		dev_err(dev, "Fail to probe\n");
		return  ret;
	}

	if (!ls_pcie_g4_is_bridge(pcie))
		return -ENODEV;

	ls_pcie_g4_enable_interrupt(pcie);

	return 0;
}

static const struct of_device_id ls_pcie_g4_of_match[] = {
	{ .compatible = "fsl,lx2160a-pcie", },
	{ },
};

static struct platform_driver ls_pcie_g4_driver = {
	.driver = {
		.name = "layerscape-pcie-gen4",
		.of_match_table = ls_pcie_g4_of_match,
		.suppress_bind_attrs = true,
	},
};

builtin_platform_driver_probe(ls_pcie_g4_driver, ls_pcie_g4_probe);
