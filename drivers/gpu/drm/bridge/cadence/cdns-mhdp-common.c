// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 * Author: Chris Zhong <zyw@rock-chips.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/reset.h>

#include <asm/unaligned.h>

#include <drm/bridge/cdns-mhdp-common.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>
#include <linux/regmap.h>

#define CDNS_DP_SPDIF_CLK		200000000
#define FW_ALIVE_TIMEOUT_US		1000000
#define MAILBOX_RETRY_US		1000
#define MAILBOX_TIMEOUT_US		5000000
#define LINK_TRAINING_RETRY_MS		20
#define LINK_TRAINING_TIMEOUT_MS	500

#define mhdp_readx_poll_timeout(op, addr, offset, val, cond, sleep_us, timeout_us)	\
({ \
	u64 __timeout_us = (timeout_us); \
	unsigned long __sleep_us = (sleep_us); \
	ktime_t __timeout = ktime_add_us(ktime_get(), __timeout_us); \
	might_sleep_if((__sleep_us) != 0); \
	for (;;) { \
		(val) = op(addr, offset); \
		if (cond) \
			break; \
		if (__timeout_us && \
		    ktime_compare(ktime_get(), __timeout) > 0) { \
			(val) = op(addr, offset); \
			break; \
		} \
		if (__sleep_us) \
			usleep_range((__sleep_us >> 2) + 1, __sleep_us); \
	} \
	(cond) ? 0 : -ETIMEDOUT; \
})

static inline u32 get_unaligned_be24(const void *p)
{
	const u8 *_p = p;

	return _p[0] << 16 | _p[1] << 8 | _p[2];
}

static inline void put_unaligned_be24(u32 val, void *p)
{
	u8 *_p = p;

	_p[0] = val >> 16;
	_p[1] = val >> 8;
	_p[2] = val;
}

u32 cdns_mhdp_bus_read(struct cdns_mhdp_device *mhdp, u32 offset)
{
	u32 val;

	mutex_lock(&mhdp->iolock);

	if (mhdp->bus_type == BUS_TYPE_LOW4K_SAPB) {
		/* Remap address to low 4K SAPB bus */
		writel(offset >> 12, mhdp->regs_sec + 0xc);
		val = readl((offset & 0xfff) + mhdp->regs_base);
	} else if (mhdp->bus_type == BUS_TYPE_LOW4K_APB) {
		/* Remap address to low 4K memory */
		writel(offset >> 12, mhdp->regs_sec + 8);
		val = readl((offset & 0xfff) + mhdp->regs_base);
	} else if (mhdp->bus_type == BUS_TYPE_NORMAL_SAPB)
		val = readl(mhdp->regs_sec + offset);
	else
		val = readl(mhdp->regs_base + offset);

	mutex_unlock(&mhdp->iolock);

	return val;
}
EXPORT_SYMBOL(cdns_mhdp_bus_read);

void cdns_mhdp_bus_write(u32 val, struct cdns_mhdp_device *mhdp, u32 offset)
{
	mutex_lock(&mhdp->iolock);

	if (mhdp->bus_type == BUS_TYPE_LOW4K_SAPB) {
		/* Remap address to low 4K SAPB bus */
		writel(offset >> 12, mhdp->regs_sec + 0xc);
		writel(val, (offset & 0xfff) + mhdp->regs_base);
	} else if (mhdp->bus_type == BUS_TYPE_LOW4K_APB) {
		/* Remap address to low 4K memory */
		writel(offset >> 12, mhdp->regs_sec + 8);
		writel(val, (offset & 0xfff) + mhdp->regs_base);
	} else if (mhdp->bus_type == BUS_TYPE_NORMAL_SAPB)
		writel(val, mhdp->regs_sec + offset);
	else
		writel(val, mhdp->regs_base + offset);

	mutex_unlock(&mhdp->iolock);
}
EXPORT_SYMBOL(cdns_mhdp_bus_write);

u32 cdns_mhdp_get_fw_clk(struct cdns_mhdp_device *mhdp)
{
	return cdns_mhdp_bus_read(mhdp, SW_CLK_H);
}
EXPORT_SYMBOL(cdns_mhdp_get_fw_clk);

void cdns_mhdp_set_fw_clk(struct cdns_mhdp_device *mhdp, unsigned long clk)
{
	cdns_mhdp_bus_write(clk / 1000000, mhdp, SW_CLK_H);
}
EXPORT_SYMBOL(cdns_mhdp_set_fw_clk);

void cdns_mhdp_clock_reset(struct cdns_mhdp_device *mhdp)
{
	u32 val;

	val = DPTX_FRMR_DATA_CLK_RSTN_EN |
	      DPTX_FRMR_DATA_CLK_EN |
	      DPTX_PHY_DATA_RSTN_EN |
	      DPTX_PHY_DATA_CLK_EN |
	      DPTX_PHY_CHAR_RSTN_EN |
	      DPTX_PHY_CHAR_CLK_EN |
	      SOURCE_AUX_SYS_CLK_RSTN_EN |
	      SOURCE_AUX_SYS_CLK_EN |
	      DPTX_SYS_CLK_RSTN_EN |
	      DPTX_SYS_CLK_EN |
	      CFG_DPTX_VIF_CLK_RSTN_EN |
	      CFG_DPTX_VIF_CLK_EN;
	cdns_mhdp_bus_write(val, mhdp, SOURCE_DPTX_CAR);

	val = SOURCE_PHY_RSTN_EN | SOURCE_PHY_CLK_EN;
	cdns_mhdp_bus_write(val, mhdp, SOURCE_PHY_CAR);

	val = SOURCE_PKT_SYS_RSTN_EN |
	      SOURCE_PKT_SYS_CLK_EN |
	      SOURCE_PKT_DATA_RSTN_EN |
	      SOURCE_PKT_DATA_CLK_EN;
	cdns_mhdp_bus_write(val, mhdp, SOURCE_PKT_CAR);

	val = SPDIF_CDR_CLK_RSTN_EN |
	      SPDIF_CDR_CLK_EN |
	      SOURCE_AIF_SYS_RSTN_EN |
	      SOURCE_AIF_SYS_CLK_EN |
	      SOURCE_AIF_CLK_RSTN_EN |
	      SOURCE_AIF_CLK_EN;
	cdns_mhdp_bus_write(val, mhdp, SOURCE_AIF_CAR);

	val = SOURCE_CIPHER_SYSTEM_CLK_RSTN_EN |
	      SOURCE_CIPHER_SYS_CLK_EN |
	      SOURCE_CIPHER_CHAR_CLK_RSTN_EN |
	      SOURCE_CIPHER_CHAR_CLK_EN;
	cdns_mhdp_bus_write(val, mhdp, SOURCE_CIPHER_CAR);

	val = SOURCE_CRYPTO_SYS_CLK_RSTN_EN |
	      SOURCE_CRYPTO_SYS_CLK_EN;
	cdns_mhdp_bus_write(val, mhdp, SOURCE_CRYPTO_CAR);

	/* enable Mailbox and PIF interrupt */
	cdns_mhdp_bus_write(0, mhdp, APB_INT_MASK);
}
EXPORT_SYMBOL(cdns_mhdp_clock_reset);

int cdns_mhdp_mailbox_read(struct cdns_mhdp_device *mhdp)
{
	int val, ret;

	ret = mhdp_readx_poll_timeout(cdns_mhdp_bus_read, mhdp, MAILBOX_EMPTY_ADDR,
				 val, !val, MAILBOX_RETRY_US,
				 MAILBOX_TIMEOUT_US);
	if (ret < 0)
		return ret;

	return cdns_mhdp_bus_read(mhdp, MAILBOX0_RD_DATA) & 0xff;
}
EXPORT_SYMBOL(cdns_mhdp_mailbox_read);

static int cdp_dp_mailbox_write(struct cdns_mhdp_device *mhdp, u8 val)
{
	int ret, full;

	ret = mhdp_readx_poll_timeout(cdns_mhdp_bus_read, mhdp, MAILBOX_FULL_ADDR,
				 full, !full, MAILBOX_RETRY_US,
				 MAILBOX_TIMEOUT_US);
	if (ret < 0)
		return ret;

	cdns_mhdp_bus_write(val, mhdp, MAILBOX0_WR_DATA);

	return 0;
}

int cdns_mhdp_mailbox_validate_receive(struct cdns_mhdp_device *mhdp,
					      u8 module_id, u8 opcode,
					      u16 req_size)
{
	u32 mbox_size, i;
	u8 header[4];
	int ret;

	/* read the header of the message */
	for (i = 0; i < 4; i++) {
		ret = cdns_mhdp_mailbox_read(mhdp);
		if (ret < 0)
			return ret;

		header[i] = ret;
	}

	mbox_size = get_unaligned_be16(header + 2);

	if (opcode != header[0] || module_id != header[1] ||
	    req_size != mbox_size) {
		/*
		 * If the message in mailbox is not what we want, we need to
		 * clear the mailbox by reading its contents.
		 */
		for (i = 0; i < mbox_size; i++)
			if (cdns_mhdp_mailbox_read(mhdp) < 0)
				break;

		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(cdns_mhdp_mailbox_validate_receive);

int cdns_mhdp_mailbox_read_receive(struct cdns_mhdp_device *mhdp,
					  u8 *buff, u16 buff_size)
{
	u32 i;
	int ret;

	for (i = 0; i < buff_size; i++) {
		ret = cdns_mhdp_mailbox_read(mhdp);
		if (ret < 0)
			return ret;

		buff[i] = ret;
	}

	return 0;
}
EXPORT_SYMBOL(cdns_mhdp_mailbox_read_receive);

int cdns_mhdp_mailbox_send(struct cdns_mhdp_device *mhdp, u8 module_id,
				  u8 opcode, u16 size, u8 *message)
{
	u8 header[4];
	int ret, i;

	header[0] = opcode;
	header[1] = module_id;
	put_unaligned_be16(size, header + 2);

	for (i = 0; i < 4; i++) {
		ret = cdp_dp_mailbox_write(mhdp, header[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < size; i++) {
		ret = cdp_dp_mailbox_write(mhdp, message[i]);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(cdns_mhdp_mailbox_send);

int cdns_mhdp_reg_read(struct cdns_mhdp_device *mhdp, u32 addr)
{
	u8 msg[4], resp[8];
	u32 val;
	int ret;

	if (addr == 0) {
		ret = -EINVAL;
		goto err_reg_read;
	}

	put_unaligned_be32(addr, msg);

	ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_GENERAL,
				     GENERAL_READ_REGISTER,
				     sizeof(msg), msg);
	if (ret)
		goto err_reg_read;

	ret = cdns_mhdp_mailbox_validate_receive(mhdp, MB_MODULE_ID_GENERAL,
						 GENERAL_READ_REGISTER,
						 sizeof(resp));
	if (ret)
		goto err_reg_read;

	ret = cdns_mhdp_mailbox_read_receive(mhdp, resp, sizeof(resp));
	if (ret)
		goto err_reg_read;

	/* Returned address value should be the same as requested */
	if (memcmp(msg, resp, sizeof(msg))) {
		ret = -EINVAL;
		goto err_reg_read;
	}

	val = get_unaligned_be32(resp + 4);

	return val;
err_reg_read:
	DRM_DEV_ERROR(mhdp->dev, "Failed to read register.\n");

	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_reg_read);

int cdns_mhdp_reg_write(struct cdns_mhdp_device *mhdp, u32 addr, u32 val)
{
	u8 msg[8];

	put_unaligned_be32(addr, msg);
	put_unaligned_be32(val, msg + 4);

	return cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_GENERAL,
				      GENERAL_WRITE_REGISTER, sizeof(msg), msg);
}
EXPORT_SYMBOL(cdns_mhdp_reg_write);

int cdns_mhdp_reg_write_bit(struct cdns_mhdp_device *mhdp, u16 addr,
				   u8 start_bit, u8 bits_no, u32 val)
{
	u8 field[8];

	put_unaligned_be16(addr, field);
	field[2] = start_bit;
	field[3] = bits_no;
	put_unaligned_be32(val, field + 4);

	return cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
				      DPTX_WRITE_FIELD, sizeof(field), field);
}
EXPORT_SYMBOL(cdns_mhdp_reg_write_bit);

int cdns_mhdp_dpcd_read(struct cdns_mhdp_device *mhdp,
			u32 addr, u8 *data, u16 len)
{
	u8 msg[5], reg[5];
	int ret;

	put_unaligned_be16(len, msg);
	put_unaligned_be24(addr, msg + 2);

	ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
				     DPTX_READ_DPCD, sizeof(msg), msg);
	if (ret)
		goto err_dpcd_read;

	ret = cdns_mhdp_mailbox_validate_receive(mhdp, MB_MODULE_ID_DP_TX,
						 DPTX_READ_DPCD,
						 sizeof(reg) + len);
	if (ret)
		goto err_dpcd_read;

	ret = cdns_mhdp_mailbox_read_receive(mhdp, reg, sizeof(reg));
	if (ret)
		goto err_dpcd_read;

	ret = cdns_mhdp_mailbox_read_receive(mhdp, data, len);

err_dpcd_read:
	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_dpcd_read);

int cdns_mhdp_dpcd_write(struct cdns_mhdp_device *mhdp, u32 addr, u8 value)
{
	u8 msg[6], reg[5];
	int ret;

	put_unaligned_be16(1, msg);
	put_unaligned_be24(addr, msg + 2);
	msg[5] = value;

	ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
				     DPTX_WRITE_DPCD, sizeof(msg), msg);
	if (ret)
		goto err_dpcd_write;

	ret = cdns_mhdp_mailbox_validate_receive(mhdp, MB_MODULE_ID_DP_TX,
						 DPTX_WRITE_DPCD, sizeof(reg));
	if (ret)
		goto err_dpcd_write;

	ret = cdns_mhdp_mailbox_read_receive(mhdp, reg, sizeof(reg));
	if (ret)
		goto err_dpcd_write;

	if (addr != get_unaligned_be24(reg + 2))
		ret = -EINVAL;

err_dpcd_write:
	if (ret)
		DRM_DEV_ERROR(mhdp->dev, "dpcd write failed: %d\n", ret);
	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_dpcd_write);

int cdns_mhdp_load_firmware(struct cdns_mhdp_device *mhdp, const u32 *i_mem,
			    u32 i_size, const u32 *d_mem, u32 d_size)
{
	u32 reg;
	int i, ret;

	/* reset ucpu before load firmware*/
	cdns_mhdp_bus_write(APB_IRAM_PATH | APB_DRAM_PATH | APB_XT_RESET,
	       mhdp, APB_CTRL);

	for (i = 0; i < i_size; i += 4)
		cdns_mhdp_bus_write(*i_mem++, mhdp, ADDR_IMEM + i);

	for (i = 0; i < d_size; i += 4)
		cdns_mhdp_bus_write(*d_mem++, mhdp, ADDR_DMEM + i);

	/* un-reset ucpu */
	cdns_mhdp_bus_write(0, mhdp, APB_CTRL);

	/* check the keep alive register to make sure fw working */
	ret = mhdp_readx_poll_timeout(cdns_mhdp_bus_read, mhdp, KEEP_ALIVE,
				 reg, reg, 2000, FW_ALIVE_TIMEOUT_US);
	if (ret < 0) {
		DRM_DEV_ERROR(mhdp->dev, "failed to loaded the FW reg = %x\n",
			      reg);
		return -EINVAL;
	}

	reg = cdns_mhdp_bus_read(mhdp, VER_L) & 0xff;
	mhdp->fw_version = reg;
	reg = cdns_mhdp_bus_read(mhdp, VER_H) & 0xff;
	mhdp->fw_version |= reg << 8;
	reg = cdns_mhdp_bus_read(mhdp, VER_LIB_L_ADDR) & 0xff;
	mhdp->fw_version |= reg << 16;
	reg = cdns_mhdp_bus_read(mhdp, VER_LIB_H_ADDR) & 0xff;
	mhdp->fw_version |= reg << 24;

	DRM_DEV_DEBUG(mhdp->dev, "firmware version: %x\n", mhdp->fw_version);

	return 0;
}
EXPORT_SYMBOL(cdns_mhdp_load_firmware);

int cdns_mhdp_set_firmware_active(struct cdns_mhdp_device *mhdp, bool enable)
{
	u8 msg[5];
	int ret, i;

	msg[0] = GENERAL_MAIN_CONTROL;
	msg[1] = MB_MODULE_ID_GENERAL;
	msg[2] = 0;
	msg[3] = 1;
	msg[4] = enable ? FW_ACTIVE : FW_STANDBY;

	for (i = 0; i < sizeof(msg); i++) {
		ret = cdp_dp_mailbox_write(mhdp, msg[i]);
		if (ret)
			goto err_set_firmware_active;
	}

	/* read the firmware state */
	for (i = 0; i < sizeof(msg); i++)  {
		ret = cdns_mhdp_mailbox_read(mhdp);
		if (ret < 0)
			goto err_set_firmware_active;

		msg[i] = ret;
	}

	ret = 0;

err_set_firmware_active:
	if (ret < 0)
		DRM_DEV_ERROR(mhdp->dev, "set firmware active failed\n");
	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_set_firmware_active);

int cdns_mhdp_set_host_cap(struct cdns_mhdp_device *mhdp, bool flip)
{
	u8 msg[8];
	int ret;

	msg[0] = drm_dp_link_rate_to_bw_code(mhdp->dp.link.rate);
	msg[1] = mhdp->dp.link.num_lanes | SCRAMBLER_EN;
	msg[2] = VOLTAGE_LEVEL_2;
	msg[3] = PRE_EMPHASIS_LEVEL_3;
	msg[4] = PTS1 | PTS2 | PTS3 | PTS4;
	msg[5] = FAST_LT_NOT_SUPPORT;
	msg[6] = flip ? LANE_MAPPING_FLIPPED : LANE_MAPPING_NORMAL;
	msg[7] = ENHANCED;

	ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
				     DPTX_SET_HOST_CAPABILITIES,
				     sizeof(msg), msg);
	if (ret)
		goto err_set_host_cap;

/* TODO Sandor */
//	ret = cdns_mhdp_reg_write(mhdp, DP_AUX_SWAP_INVERSION_CONTROL,
//				  AUX_HOST_INVERT);

err_set_host_cap:
	if (ret)
		DRM_DEV_ERROR(mhdp->dev, "set host cap failed: %d\n", ret);
	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_set_host_cap);

int cdns_mhdp_event_config(struct cdns_mhdp_device *mhdp)
{
	u8 msg[5];
	int ret;

	memset(msg, 0, sizeof(msg));

	msg[0] = MHDP_EVENT_ENABLE_HPD | MHDP_EVENT_ENABLE_TRAINING;

	ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
				     DPTX_ENABLE_EVENT, sizeof(msg), msg);
	if (ret)
		DRM_DEV_ERROR(mhdp->dev, "set event config failed: %d\n", ret);

	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_event_config);

u32 cdns_mhdp_get_event(struct cdns_mhdp_device *mhdp)
{
	return cdns_mhdp_bus_read(mhdp, SW_EVENTS0);
}
EXPORT_SYMBOL(cdns_mhdp_get_event);

int cdns_mhdp_get_hpd_status(struct cdns_mhdp_device *mhdp)
{
	u8 status;
	int ret;

	ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
				     DPTX_HPD_STATE, 0, NULL);
	if (ret)
		goto err_get_hpd;

	ret = cdns_mhdp_mailbox_validate_receive(mhdp, MB_MODULE_ID_DP_TX,
						 DPTX_HPD_STATE,
						 sizeof(status));
	if (ret)
		goto err_get_hpd;

	ret = cdns_mhdp_mailbox_read_receive(mhdp, &status, sizeof(status));
	if (ret)
		goto err_get_hpd;

	return status;

err_get_hpd:
	DRM_DEV_ERROR(mhdp->dev, "get hpd status failed: %d\n", ret);
	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_get_hpd_status);

int cdns_mhdp_get_edid_block(void *data, u8 *edid,
			  unsigned int block, size_t length)
{
	struct cdns_mhdp_device *mhdp = data;
	u8 msg[2], reg[2], i;
	int ret;

	for (i = 0; i < 4; i++) {
		msg[0] = block / 2;
		msg[1] = block % 2;

		ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
					     DPTX_GET_EDID, sizeof(msg), msg);
		if (ret)
			continue;

		ret = cdns_mhdp_mailbox_validate_receive(mhdp,
							 MB_MODULE_ID_DP_TX,
							 DPTX_GET_EDID,
							 sizeof(reg) + length);
		if (ret)
			continue;

		ret = cdns_mhdp_mailbox_read_receive(mhdp, reg, sizeof(reg));
		if (ret)
			continue;

		ret = cdns_mhdp_mailbox_read_receive(mhdp, edid, length);
		if (ret)
			continue;

		if (reg[0] == length && reg[1] == block / 2)
			break;
	}

	if (ret)
		DRM_DEV_ERROR(mhdp->dev, "get block[%d] edid failed: %d\n",
			      block, ret);

	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_get_edid_block);

static int cdns_mhdp_training_start(struct cdns_mhdp_device *mhdp)
{
	unsigned long timeout;
	u8 msg, event[2];
	int ret;

	msg = LINK_TRAINING_RUN;

	/* start training */
	ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
				     DPTX_TRAINING_CONTROL, sizeof(msg), &msg);
	if (ret)
		goto err_training_start;

	timeout = jiffies + msecs_to_jiffies(LINK_TRAINING_TIMEOUT_MS);
	while (time_before(jiffies, timeout)) {
		msleep(LINK_TRAINING_RETRY_MS);
		ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
					     DPTX_READ_EVENT, 0, NULL);
		if (ret)
			goto err_training_start;

		ret = cdns_mhdp_mailbox_validate_receive(mhdp,
							 MB_MODULE_ID_DP_TX,
							 DPTX_READ_EVENT,
							 sizeof(event));
		if (ret)
			goto err_training_start;

		ret = cdns_mhdp_mailbox_read_receive(mhdp, event,
						     sizeof(event));
		if (ret)
			goto err_training_start;

		if (event[1] & EQ_PHASE_FINISHED)
			return 0;
	}

	ret = -ETIMEDOUT;

err_training_start:
	DRM_DEV_ERROR(mhdp->dev, "training failed: %d\n", ret);
	return ret;
}

static int cdns_mhdp_get_training_status(struct cdns_mhdp_device *mhdp)
{
	u8 status[10];
	int ret;

	ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
				     DPTX_READ_LINK_STAT, 0, NULL);
	if (ret)
		goto err_get_training_status;

	ret = cdns_mhdp_mailbox_validate_receive(mhdp, MB_MODULE_ID_DP_TX,
						 DPTX_READ_LINK_STAT,
						 sizeof(status));
	if (ret)
		goto err_get_training_status;

	ret = cdns_mhdp_mailbox_read_receive(mhdp, status, sizeof(status));
	if (ret)
		goto err_get_training_status;

	mhdp->dp.link.rate = drm_dp_bw_code_to_link_rate(status[0]); 
	mhdp->dp.link.num_lanes = status[1];

err_get_training_status:
	if (ret)
		DRM_DEV_ERROR(mhdp->dev, "get training status failed: %d\n",
			      ret);
	return ret;
}

int cdns_mhdp_train_link(struct cdns_mhdp_device *mhdp)
{
	int ret;

	ret = cdns_mhdp_training_start(mhdp);
	if (ret) {
		DRM_DEV_ERROR(mhdp->dev, "Failed to start training %d\n",
			      ret);
		return ret;
	}

	ret = cdns_mhdp_get_training_status(mhdp);
	if (ret) {
		DRM_DEV_ERROR(mhdp->dev, "Failed to get training stat %d\n",
			      ret);
		return ret;
	}

	DRM_DEV_DEBUG_KMS(mhdp->dev, "rate:0x%x, lanes:%d\n", mhdp->dp.link.rate,
			  mhdp->dp.link.num_lanes);
	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_train_link);

int cdns_mhdp_set_video_status(struct cdns_mhdp_device *mhdp, int active)
{
	u8 msg;
	int ret;

	msg = !!active;

	ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
				     DPTX_SET_VIDEO, sizeof(msg), &msg);
	if (ret)
		DRM_DEV_ERROR(mhdp->dev, "set video status failed: %d\n", ret);

	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_set_video_status);

static int cdns_mhdp_get_msa_misc(struct video_info *video,
				  struct drm_display_mode *mode)
{
	u32 msa_misc;
	u8 val[2] = {0};

	switch (video->color_fmt) {
	case PXL_RGB:
	case Y_ONLY:
		val[0] = 0;
		break;
	/* set YUV default color space conversion to BT601 */
	case YCBCR_4_4_4:
		val[0] = 6 + BT_601 * 8;
		break;
	case YCBCR_4_2_2:
		val[0] = 5 + BT_601 * 8;
		break;
	case YCBCR_4_2_0:
		val[0] = 5;
		break;
	};

	switch (video->color_depth) {
	case 6:
		val[1] = 0;
		break;
	case 8:
		val[1] = 1;
		break;
	case 10:
		val[1] = 2;
		break;
	case 12:
		val[1] = 3;
		break;
	case 16:
		val[1] = 4;
		break;
	};

	msa_misc = 2 * val[0] + 32 * val[1] +
		   ((video->color_fmt == Y_ONLY) ? (1 << 14) : 0);

	return msa_misc;
}

int cdns_mhdp_config_video(struct cdns_mhdp_device *mhdp)
{
	struct video_info *video = &mhdp->video_info;
	struct drm_display_mode *mode = &mhdp->mode;
	u64 symbol;
	u32 val, link_rate, rem;
	u8 bit_per_pix, tu_size_reg = TU_SIZE;
	int ret;

	bit_per_pix = (video->color_fmt == YCBCR_4_2_2) ?
		      (video->color_depth * 2) : (video->color_depth * 3);

	link_rate = mhdp->dp.link.rate / 1000;

	ret = cdns_mhdp_reg_write(mhdp, BND_HSYNC2VSYNC, VIF_BYPASS_INTERLACE);
	if (ret)
		goto err_config_video;

	ret = cdns_mhdp_reg_write(mhdp, HSYNC2VSYNC_POL_CTRL, 0);
	if (ret)
		goto err_config_video;

	/*
	 * get a best tu_size and valid symbol:
	 * 1. chose Lclk freq(162Mhz, 270Mhz, 540Mhz), set TU to 32
	 * 2. calculate VS(valid symbol) = TU * Pclk * Bpp / (Lclk * Lanes)
	 * 3. if VS > *.85 or VS < *.1 or VS < 2 or TU < VS + 4, then set
	 *    TU += 2 and repeat 2nd step.
	 */
	do {
		tu_size_reg += 2;
		symbol = (u64) tu_size_reg * mode->clock * bit_per_pix;
		do_div(symbol, mhdp->dp.link.num_lanes * link_rate * 8);
		rem = do_div(symbol, 1000);
		if (tu_size_reg > 64) {
			ret = -EINVAL;
			DRM_DEV_ERROR(mhdp->dev,
				      "tu error, clk:%d, lanes:%d, rate:%d\n",
				      mode->clock, mhdp->dp.link.num_lanes,
				      link_rate);
			goto err_config_video;
		}
	} while ((symbol <= 1) || (tu_size_reg - symbol < 4) ||
		 (rem > 850) || (rem < 100));

	val = symbol + (tu_size_reg << 8);
	val |= TU_CNT_RST_EN;
	ret = cdns_mhdp_reg_write(mhdp, DP_FRAMER_TU, val);
	if (ret)
		goto err_config_video;

	/* set the FIFO Buffer size */
	val = div_u64(mode->clock * (symbol + 1), 1000) + link_rate;
	val /= (mhdp->dp.link.num_lanes * link_rate);
	val = div_u64(8 * (symbol + 1), bit_per_pix) - val;
	val += 2;
	ret = cdns_mhdp_reg_write(mhdp, DP_VC_TABLE(15), val);

	switch (video->color_depth) {
	case 6:
		val = BCS_6;
		break;
	case 8:
		val = BCS_8;
		break;
	case 10:
		val = BCS_10;
		break;
	case 12:
		val = BCS_12;
		break;
	case 16:
		val = BCS_16;
		break;
	};

	val += video->color_fmt << 8;
	ret = cdns_mhdp_reg_write(mhdp, DP_FRAMER_PXL_REPR, val);
	if (ret)
		goto err_config_video;

	val = video->h_sync_polarity ? DP_FRAMER_SP_HSP : 0;
	val |= video->v_sync_polarity ? DP_FRAMER_SP_VSP : 0;
	ret = cdns_mhdp_reg_write(mhdp, DP_FRAMER_SP, val);
	if (ret)
		goto err_config_video;

	val = (mode->hsync_start - mode->hdisplay) << 16;
	val |= mode->htotal - mode->hsync_end;
	ret = cdns_mhdp_reg_write(mhdp, DP_FRONT_BACK_PORCH, val);
	if (ret)
		goto err_config_video;

	val = mode->hdisplay * bit_per_pix / 8;
	ret = cdns_mhdp_reg_write(mhdp, DP_BYTE_COUNT, val);
	if (ret)
		goto err_config_video;

	val = mode->htotal | ((mode->htotal - mode->hsync_start) << 16);
	ret = cdns_mhdp_reg_write(mhdp, MSA_HORIZONTAL_0, val);
	if (ret)
		goto err_config_video;

	val = mode->hsync_end - mode->hsync_start;
	val |= (mode->hdisplay << 16) | (video->h_sync_polarity << 15);
	ret = cdns_mhdp_reg_write(mhdp, MSA_HORIZONTAL_1, val);
	if (ret)
		goto err_config_video;

	val = mode->vtotal;
	val |= (mode->vtotal - mode->vsync_start) << 16;
	ret = cdns_mhdp_reg_write(mhdp, MSA_VERTICAL_0, val);
	if (ret)
		goto err_config_video;

	val = mode->vsync_end - mode->vsync_start;
	val |= (mode->vdisplay << 16) | (video->v_sync_polarity << 15);
	ret = cdns_mhdp_reg_write(mhdp, MSA_VERTICAL_1, val);
	if (ret)
		goto err_config_video;

	val = cdns_mhdp_get_msa_misc(video, mode);
	ret = cdns_mhdp_reg_write(mhdp, MSA_MISC, val);
	if (ret)
		goto err_config_video;

	ret = cdns_mhdp_reg_write(mhdp, STREAM_CONFIG, 1);
	if (ret)
		goto err_config_video;

	val = mode->hsync_end - mode->hsync_start;
	val |= mode->hdisplay << 16;
	ret = cdns_mhdp_reg_write(mhdp, DP_HORIZONTAL, val);
	if (ret)
		goto err_config_video;

	val = mode->vdisplay;
	val |= (mode->vtotal - mode->vsync_start) << 16;
	ret = cdns_mhdp_reg_write(mhdp, DP_VERTICAL_0, val);
	if (ret)
		goto err_config_video;

	val = mode->vtotal;
	ret = cdns_mhdp_reg_write(mhdp, DP_VERTICAL_1, val);
	if (ret)
		goto err_config_video;

	ret = cdns_mhdp_reg_write_bit(mhdp, DP_VB_ID, 2, 1, 0);

err_config_video:
	if (ret)
		DRM_DEV_ERROR(mhdp->dev, "config video failed: %d\n", ret);
	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_config_video);

int cdns_mhdp_adjust_lt(struct cdns_mhdp_device *mhdp,
			u8 nlanes, u16 udelay, u8 *lanes_data, u8 *dpcd)
{
	u8 payload[7];
	u8 hdr[5]; /* For DPCD read response header */
	u32 addr;
	u8 const nregs = 6; /* Registers 0x202-0x207 */
	int ret;

	if (nlanes != 4 && nlanes != 2 && nlanes != 1) {
		DRM_DEV_ERROR(mhdp->dev, "invalid number of lanes: %d\n",
			      nlanes);
		ret = -EINVAL;
		goto err_adjust_lt;
	}

	payload[0] = nlanes;
	put_unaligned_be16(udelay, payload + 1);
	memcpy(payload + 3, lanes_data, nlanes);

	ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_DP_TX,
				     DPTX_ADJUST_LT,
				     sizeof(payload), payload);
	if (ret)
		goto err_adjust_lt;

	/* Yes, read the DPCD read command response */
	ret = cdns_mhdp_mailbox_validate_receive(mhdp, MB_MODULE_ID_DP_TX,
						 DPTX_READ_DPCD,
						 sizeof(hdr) + nregs);
	if (ret)
		goto err_adjust_lt;

	ret = cdns_mhdp_mailbox_read_receive(mhdp, hdr, sizeof(hdr));
	if (ret)
		goto err_adjust_lt;

	addr = get_unaligned_be24(hdr + 2);
	if (addr != DP_LANE0_1_STATUS)
		goto err_adjust_lt;

	ret = cdns_mhdp_mailbox_read_receive(mhdp, dpcd, nregs);

err_adjust_lt:
	if (ret)
		DRM_DEV_ERROR(mhdp->dev, "Failed to adjust Link Training.\n");

	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_adjust_lt);

int cdns_phy_reg_write(struct cdns_mhdp_device *mhdp, u32 addr, u32 val)
{
	return cdns_mhdp_reg_write(mhdp, ADDR_PHY_AFE + (addr << 2), val);
}
EXPORT_SYMBOL(cdns_phy_reg_write);

u32 cdns_phy_reg_read(struct cdns_mhdp_device *mhdp, u32 addr)
{
	return cdns_mhdp_reg_read(mhdp, ADDR_PHY_AFE + (addr << 2));
}
EXPORT_SYMBOL(cdns_phy_reg_read);

int cdns_mhdp_read_hpd(struct cdns_mhdp_device *mhdp)
{
	u8 status;
	int ret;

	ret = cdns_mhdp_mailbox_send(mhdp, MB_MODULE_ID_GENERAL, GENERAL_GET_HPD_STATE,
				  0, NULL);
	if (ret)
		goto err_get_hpd;

	ret = cdns_mhdp_mailbox_validate_receive(mhdp, MB_MODULE_ID_GENERAL,
							GENERAL_GET_HPD_STATE, sizeof(status));
	if (ret)
		goto err_get_hpd;

	ret = cdns_mhdp_mailbox_read_receive(mhdp, &status, sizeof(status));
	if (ret)
		goto err_get_hpd;

	return status;

err_get_hpd:
	DRM_ERROR("read hpd  failed: %d\n", ret);
	return ret;
}
EXPORT_SYMBOL(cdns_mhdp_read_hpd);

bool cdns_mhdp_check_alive(struct cdns_mhdp_device *mhdp)
{
	u32  alive, newalive;
	u8 retries_left = 50;

	alive = cdns_mhdp_bus_read(mhdp, KEEP_ALIVE);

	while (retries_left--) {
		udelay(2);

		newalive = cdns_mhdp_bus_read(mhdp, KEEP_ALIVE);
		if (alive == newalive)
			continue;
		return true;
	}
	return false;
}
EXPORT_SYMBOL(cdns_mhdp_check_alive);
