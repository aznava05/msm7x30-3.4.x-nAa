/* Copyright (c) 2010-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/iopoll.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/types.h>
#include <mach/msm_hdmi_audio.h>

#define REG_DUMP 0

#include "mdss_fb.h"
#include "mdss_hdmi_tx.h"
#include "mdss_hdmi_edid.h"
#include "mdss.h"
#include "mdss_panel.h"

#define DRV_NAME "hdmi-tx"
#define COMPATIBLE_NAME "qcom,hdmi-tx"

#define DEFAULT_VIDEO_RESOLUTION HDMI_VFRMT_1920x1080p60_16_9;

/* HDMI PHY/PLL bit field macros */
#define SW_RESET BIT(2)
#define SW_RESET_PLL BIT(0)

#define HPD_DISCONNECT_POLARITY 0
#define HPD_CONNECT_POLARITY    1

#define IFRAME_CHECKSUM_32(d)			\
	((d & 0xff) + ((d >> 8) & 0xff) +	\
	((d >> 16) & 0xff) + ((d >> 24) & 0xff))

/* parameters for clock regeneration */
struct hdmi_tx_audio_acr {
	u32 n;
	u32 cts;
};

struct hdmi_tx_audio_acr_arry {
	u32 pclk;
	struct hdmi_tx_audio_acr lut[HDMI_SAMPLE_RATE_MAX];
};

static int hdmi_tx_sysfs_enable_hpd(struct hdmi_tx_ctrl *hdmi_ctrl, int on);
static irqreturn_t hdmi_tx_isr(int irq, void *data);
static void hdmi_tx_hpd_off(struct hdmi_tx_ctrl *hdmi_ctrl);

struct mdss_hw hdmi_tx_hw = {
	.hw_ndx = MDSS_HW_HDMI,
	.ptr = NULL,
	.irq_handler = hdmi_tx_isr,
};

struct dss_gpio hpd_gpio_config[] = {
	{0, 1, COMPATIBLE_NAME "-hpd"},
	{0, 1, COMPATIBLE_NAME "-ddc-clk"},
	{0, 1, COMPATIBLE_NAME "-ddc-data"},
	{0, 1, COMPATIBLE_NAME "-mux-en"},
	{0, 0, COMPATIBLE_NAME "-mux-sel"}
};

struct dss_gpio core_gpio_config[] = {
};

struct dss_gpio cec_gpio_config[] = {
	{0, 1, COMPATIBLE_NAME "-cec"}
};

const char *hdmi_pm_name(enum hdmi_tx_power_module_type module)
{
	switch (module) {
	case HDMI_TX_HPD_PM:	return "HDMI_TX_HPD_PM";
	case HDMI_TX_CORE_PM:	return "HDMI_TX_CORE_PM";
	case HDMI_TX_CEC_PM:	return "HDMI_TX_CEC_PM";
	default: return "???";
	}
} /* hdmi_pm_name */

static u8 hdmi_tx_avi_iframe_lut[][16] = {
/*	480p60	480i60	576p50	576i50	720p60	 720p50	1080p60	1080i60	1080p50
	1080i50	1080p24	1080p30	1080p25	640x480p 480p60_16_9 576p50_4_3 */
	{0x10,	0x10,	0x10,	0x10,	0x10,	 0x10,	0x10,	0x10,	0x10,
	 0x10,	0x10,	0x10,	0x10,	0x10, 0x10, 0x10}, /*00*/
	{0x18,	0x18,	0x28,	0x28,	0x28,	 0x28,	0x28,	0x28,	0x28,
	 0x28,	0x28,	0x28,	0x28,	0x18, 0x28, 0x18}, /*01*/
	{0x00,	0x04,	0x04,	0x04,	0x04,	 0x04,	0x04,	0x04,	0x04,
	 0x04,	0x04,	0x04,	0x04,	0x88, 0x00, 0x04}, /*02*/
	{0x02,	0x06,	0x11,	0x15,	0x04,	 0x13,	0x10,	0x05,	0x1F,
	 0x14,	0x20,	0x22,	0x21,	0x01, 0x03, 0x11}, /*03*/
	{0x00,	0x01,	0x00,	0x01,	0x00,	 0x00,	0x00,	0x00,	0x00,
	 0x00,	0x00,	0x00,	0x00,	0x00, 0x00, 0x00}, /*04*/
	{0x00,	0x00,	0x00,	0x00,	0x00,	 0x00,	0x00,	0x00,	0x00,
	 0x00,	0x00,	0x00,	0x00,	0x00, 0x00, 0x00}, /*05*/
	{0x00,	0x00,	0x00,	0x00,	0x00,	 0x00,	0x00,	0x00,	0x00,
	 0x00,	0x00,	0x00,	0x00,	0x00, 0x00, 0x00}, /*06*/
	{0xE1,	0xE1,	0x41,	0x41,	0xD1,	 0xd1,	0x39,	0x39,	0x39,
	 0x39,	0x39,	0x39,	0x39,	0xe1, 0xE1, 0x41}, /*07*/
	{0x01,	0x01,	0x02,	0x02,	0x02,	 0x02,	0x04,	0x04,	0x04,
	 0x04,	0x04,	0x04,	0x04,	0x01, 0x01, 0x02}, /*08*/
	{0x00,	0x00,	0x00,	0x00,	0x00,	 0x00,	0x00,	0x00,	0x00,
	 0x00,	0x00,	0x00,	0x00,	0x00, 0x00, 0x00}, /*09*/
	{0x00,	0x00,	0x00,	0x00,	0x00,	 0x00,	0x00,	0x00,	0x00,
	 0x00,	0x00,	0x00,	0x00,	0x00, 0x00, 0x00}, /*10*/
	{0xD1,	0xD1,	0xD1,	0xD1,	0x01,	 0x01,	0x81,	0x81,	0x81,
	 0x81,	0x81,	0x81,	0x81,	0x81, 0xD1, 0xD1}, /*11*/
	{0x02,	0x02,	0x02,	0x02,	0x05,	 0x05,	0x07,	0x07,	0x07,
	 0x07,	0x07,	0x07,	0x07,	0x02, 0x02, 0x02}  /*12*/
};

/* Audio constants lookup table for hdmi_tx_audio_acr_setup */
/* Valid Pixel-Clock rates: 25.2MHz, 27MHz, 27.03MHz, 74.25MHz, 148.5MHz */
static const struct hdmi_tx_audio_acr_arry hdmi_tx_audio_acr_lut[] = {
	/*  25.200MHz  */
	{25200, {{4096, 25200}, {6272, 28000}, {6144, 25200}, {12544, 28000},
		{12288, 25200}, {25088, 28000}, {24576, 25200} } },
	/*  27.000MHz  */
	{27000, {{4096, 27000}, {6272, 30000}, {6144, 27000}, {12544, 30000},
		{12288, 27000}, {25088, 30000}, {24576, 27000} } },
	/*  27.027MHz */
	{27030, {{4096, 27027}, {6272, 30030}, {6144, 27027}, {12544, 30030},
		{12288, 27027}, {25088, 30030}, {24576, 27027} } },
	/*  74.250MHz */
	{74250, {{4096, 74250}, {6272, 82500}, {6144, 74250}, {12544, 82500},
		{12288, 74250}, {25088, 82500}, {24576, 74250} } },
	/* 148.500MHz */
	{148500, {{4096, 148500}, {6272, 165000}, {6144, 148500},
		{12544, 165000}, {12288, 148500}, {25088, 165000},
		{24576, 148500} } },
};

const char *hdmi_tx_pm_name(enum hdmi_tx_power_module_type module)
{
	switch (module) {
	case HDMI_TX_HPD_PM:	return "HDMI_TX_HPD_PM";
	case HDMI_TX_CORE_PM:	return "HDMI_TX_CORE_PM";
	case HDMI_TX_CEC_PM:	return "HDMI_TX_CEC_PM";
	default: return "???";
	}
} /* hdmi_tx_pm_name */

static const char *hdmi_tx_io_name(u32 type)
{
	switch (type) {
	case HDMI_TX_CORE_IO:	return "core_physical";
	case HDMI_TX_PHY_IO:	return "phy_physical";
	case HDMI_TX_QFPROM_IO:	return "qfprom_physical";
	default:		return NULL;
	}
} /* hdmi_tx_io_name */

static struct hdmi_tx_ctrl *hdmi_tx_get_drvdata_from_panel_data(
	struct mdss_panel_data *mpd)
{
	struct hdmi_tx_ctrl *hdmi_ctrl = NULL;

	if (mpd) {
		hdmi_ctrl = container_of(mpd, struct hdmi_tx_ctrl, panel_data);
		if (hdmi_ctrl) {
			hdmi_ctrl->pixel_clk =
				mpd->panel_info.fbi->var.pixclock;
			hdmi_ctrl->xres = mpd->panel_info.fbi->var.xres;
			hdmi_ctrl->yres = mpd->panel_info.fbi->var.yres;
		} else {
			DEV_ERR("%s: hdmi_ctrl = NULL\n", __func__);
		}
	} else {
		DEV_ERR("%s: mdss_panel_data = NULL\n", __func__);
	}
	return hdmi_ctrl;
} /* hdmi_tx_get_drvdata_from_panel_data */

static struct hdmi_tx_ctrl *hdmi_tx_get_drvdata_from_sysfs_dev(
	struct device *device)
{
	struct msm_fb_data_type *mfd = NULL;
	struct mdss_panel_data *panel_data = NULL;
	struct fb_info *fbi = dev_get_drvdata(device);

	if (fbi) {
		mfd = (struct msm_fb_data_type *)fbi->par;
		panel_data = dev_get_platdata(&mfd->pdev->dev);

		return hdmi_tx_get_drvdata_from_panel_data(panel_data);
	} else {
		DEV_ERR("%s: fbi = NULL\n", __func__);
		return NULL;
	}
} /* hdmi_tx_get_drvdata_from_sysfs_dev */

/* todo: Fix this. Right now this is declared in hdmi_util.h */
void *hdmi_get_featuredata_from_sysfs_dev(struct device *device,
	u32 feature_type)
{
	struct hdmi_tx_ctrl *hdmi_ctrl = NULL;

	if (!device || feature_type > HDMI_TX_FEAT_MAX) {
		DEV_ERR("%s: invalid input\n", __func__);
		return NULL;
	}

	hdmi_ctrl = hdmi_tx_get_drvdata_from_sysfs_dev(device);
	if (hdmi_ctrl)
		return hdmi_ctrl->feature_data[feature_type];
	else
		return NULL;

} /* hdmi_tx_get_featuredata_from_sysfs_dev */
EXPORT_SYMBOL(hdmi_get_featuredata_from_sysfs_dev);

static ssize_t hdmi_tx_sysfs_rda_connected(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct hdmi_tx_ctrl *hdmi_ctrl =
		hdmi_tx_get_drvdata_from_sysfs_dev(dev);

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&hdmi_ctrl->mutex);
	ret = snprintf(buf, PAGE_SIZE, "%d\n", hdmi_ctrl->hpd_state);
	DEV_DBG("%s: '%d'\n", __func__, hdmi_ctrl->hpd_state);
	mutex_unlock(&hdmi_ctrl->mutex);

	return ret;
} /* hdmi_tx_sysfs_rda_connected */

static ssize_t hdmi_tx_sysfs_rda_hpd(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct hdmi_tx_ctrl *hdmi_ctrl =
		hdmi_tx_get_drvdata_from_sysfs_dev(dev);

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", hdmi_ctrl->hpd_feature_on);
	DEV_DBG("%s: '%d'\n", __func__, hdmi_ctrl->hpd_feature_on);

	return ret;
} /* hdmi_tx_sysfs_rda_hpd */

static ssize_t hdmi_tx_sysfs_wta_hpd(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int hpd, rc = 0;
	ssize_t ret = strnlen(buf, PAGE_SIZE);
	struct hdmi_tx_ctrl *hdmi_ctrl = NULL;

	DEV_DBG("%s:\n", __func__);
	hdmi_ctrl = hdmi_tx_get_drvdata_from_sysfs_dev(dev);

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	rc = kstrtoint(buf, 10, &hpd);
	if (rc) {
		DEV_ERR("%s: kstrtoint failed. rc=%d\n", __func__, rc);
		return rc;
	}

	if (0 == hpd && hdmi_ctrl->hpd_feature_on) {
		rc = hdmi_tx_sysfs_enable_hpd(hdmi_ctrl, false);
	} else if (1 == hpd && !hdmi_ctrl->hpd_feature_on) {
		rc = hdmi_tx_sysfs_enable_hpd(hdmi_ctrl, true);
	} else {
		rc = -EPERM;
		ret = rc;
	}

	if (!rc) {
		hdmi_ctrl->hpd_feature_on =
			(~hdmi_ctrl->hpd_feature_on) & BIT(0);
		DEV_DBG("%s: '%d'\n", __func__, hdmi_ctrl->hpd_feature_on);
	} else {
		DEV_DBG("%s: '%d' (unchanged)\n", __func__,
			hdmi_ctrl->hpd_feature_on);
	}

	return ret;
} /* hdmi_tx_sysfs_wta_hpd */

static DEVICE_ATTR(connected, S_IRUGO, hdmi_tx_sysfs_rda_connected, NULL);
static DEVICE_ATTR(hpd, S_IRUGO | S_IWUSR, hdmi_tx_sysfs_rda_hpd,
	hdmi_tx_sysfs_wta_hpd);

static struct attribute *hdmi_tx_fs_attrs[] = {
	&dev_attr_connected.attr,
	&dev_attr_hpd.attr,
	NULL,
};
static struct attribute_group hdmi_tx_fs_attrs_group = {
	.attrs = hdmi_tx_fs_attrs,
};

static int hdmi_tx_sysfs_create(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int rc;
	struct mdss_panel_info *pinfo = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -ENODEV;
	}
	pinfo = &hdmi_ctrl->panel_data.panel_info;

	rc = sysfs_create_group(&pinfo->fbi->dev->kobj,
		&hdmi_tx_fs_attrs_group);
	if (rc) {
		DEV_ERR("%s: failed, rc=%d\n", __func__, rc);
		return rc;
	}
	hdmi_ctrl->kobj = &pinfo->fbi->dev->kobj;
	DEV_DBG("%s: sysfs group %p\n", __func__, hdmi_ctrl->kobj);

	kobject_uevent(hdmi_ctrl->kobj, KOBJ_ADD);
	DEV_DBG("%s: kobject_uevent(KOBJ_ADD)\n", __func__);

	return 0;
} /* hdmi_tx_sysfs_create */

static void hdmi_tx_sysfs_remove(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}
	if (hdmi_ctrl->kobj)
		sysfs_remove_group(hdmi_ctrl->kobj, &hdmi_tx_fs_attrs_group);
	hdmi_ctrl->kobj = NULL;
} /* hdmi_tx_sysfs_remove */

/* Enable HDMI features */
static int hdmi_tx_init_features(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	struct hdmi_edid_init_data edid_init_data;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	edid_init_data.io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	edid_init_data.mutex = &hdmi_ctrl->mutex;
	edid_init_data.sysfs_kobj = hdmi_ctrl->kobj;
	edid_init_data.ddc_ctrl = &hdmi_ctrl->ddc_ctrl;

	hdmi_ctrl->feature_data[HDMI_TX_FEAT_EDID] =
		hdmi_edid_init(&edid_init_data);
	if (!hdmi_ctrl->feature_data[HDMI_TX_FEAT_EDID]) {
		DEV_ERR("%s: hdmi_edid_init failed\n", __func__);
		return -EPERM;
	}
	hdmi_edid_set_video_resolution(
		hdmi_ctrl->feature_data[HDMI_TX_FEAT_EDID],
		hdmi_ctrl->video_resolution);

	return 0;
} /* hdmi_tx_init_features */

static inline u32 hdmi_tx_is_controller_on(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	struct dss_io_data *io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	return DSS_REG_R_ND(io, HDMI_CTRL) & BIT(0);
} /* hdmi_tx_is_controller_on */

static inline u32 hdmi_tx_is_dvi_mode(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	return hdmi_edid_get_sink_mode(
		hdmi_ctrl->feature_data[HDMI_TX_FEAT_EDID]) ? 0 : 1;
} /* hdmi_tx_is_dvi_mode */

static int hdmi_tx_init_panel_info(uint32_t resolution,
	struct mdss_panel_info *pinfo)
{
	const struct hdmi_disp_mode_timing_type *timing =
		hdmi_get_supported_mode(resolution);

	if (!timing || !pinfo) {
		DEV_ERR("%s: invalid input.\n", __func__);
		return -EINVAL;
	}

	pinfo->xres = timing->active_h;
	pinfo->yres = timing->active_v;
	pinfo->clk_rate = timing->pixel_freq*1000;

	pinfo->lcdc.h_back_porch = timing->back_porch_h;
	pinfo->lcdc.h_front_porch = timing->front_porch_h;
	pinfo->lcdc.h_pulse_width = timing->pulse_width_h;
	pinfo->lcdc.v_back_porch = timing->back_porch_v;
	pinfo->lcdc.v_front_porch = timing->front_porch_v;
	pinfo->lcdc.v_pulse_width = timing->pulse_width_v;

	pinfo->type = DTV_PANEL;
	pinfo->pdest = DISPLAY_2;
	pinfo->wait_cycle = 0;
	pinfo->bpp = 24;
	pinfo->fb_num = 1;

	pinfo->lcdc.border_clr = 0; /* blk */
	pinfo->lcdc.underflow_clr = 0xff; /* blue */
	pinfo->lcdc.hsync_skew = 0;

	return 0;
} /* hdmi_tx_init_panel_info */

/* Table indicating the video format supported by the HDMI TX Core */
/* Valid Pixel-Clock rates: 25.2MHz, 27MHz, 27.03MHz, 74.25MHz, 148.5MHz */
static void hdmi_tx_setup_video_mode_lut(void)
{
	hdmi_set_supported_mode(HDMI_VFRMT_640x480p60_4_3);
	hdmi_set_supported_mode(HDMI_VFRMT_720x480p60_4_3);
	hdmi_set_supported_mode(HDMI_VFRMT_720x480p60_16_9);
	hdmi_set_supported_mode(HDMI_VFRMT_720x576p50_4_3);
	hdmi_set_supported_mode(HDMI_VFRMT_720x576p50_16_9);
	hdmi_set_supported_mode(HDMI_VFRMT_1440x480i60_4_3);
	hdmi_set_supported_mode(HDMI_VFRMT_1440x480i60_16_9);
	hdmi_set_supported_mode(HDMI_VFRMT_1440x576i50_4_3);
	hdmi_set_supported_mode(HDMI_VFRMT_1440x576i50_16_9);
	hdmi_set_supported_mode(HDMI_VFRMT_1280x720p50_16_9);
	hdmi_set_supported_mode(HDMI_VFRMT_1280x720p60_16_9);
	hdmi_set_supported_mode(HDMI_VFRMT_1920x1080p24_16_9);
	hdmi_set_supported_mode(HDMI_VFRMT_1920x1080p25_16_9);
	hdmi_set_supported_mode(HDMI_VFRMT_1920x1080p30_16_9);
	hdmi_set_supported_mode(HDMI_VFRMT_1920x1080p50_16_9);
	hdmi_set_supported_mode(HDMI_VFRMT_1920x1080i60_16_9);
	hdmi_set_supported_mode(HDMI_VFRMT_1920x1080p60_16_9);
} /* hdmi_tx_setup_video_mode_lut */

static int hdmi_tx_read_sink_info(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int status;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	if (!hdmi_tx_is_controller_on(hdmi_ctrl)) {
		DEV_ERR("%s: failed: HDMI controller is off", __func__);
		status = -ENXIO;
		goto error;
	}

	hdmi_ddc_config(&hdmi_ctrl->ddc_ctrl);

	status = hdmi_edid_read(hdmi_ctrl->feature_data[HDMI_TX_FEAT_EDID]);
	if (!status)
		DEV_DBG("%s: hdmi_edid_read success\n", __func__);
	else
		DEV_ERR("%s: hdmi_edid_read failed\n", __func__);

error:
	return status;
} /* hdmi_tx_read_sink_info */

static void hdmi_tx_hpd_int_work(struct work_struct *work)
{
	struct hdmi_tx_ctrl *hdmi_ctrl = NULL;
	struct dss_io_data *io = NULL;

	hdmi_ctrl = container_of(work, struct hdmi_tx_ctrl, hpd_int_work);
	if (!hdmi_ctrl || !hdmi_ctrl->hpd_initialized) {
		DEV_DBG("%s: invalid input\n", __func__);
		return;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: Core io is not initialized\n", __func__);
		return;
	}

	DEV_DBG("%s: Got HPD interrupt\n", __func__);

	hdmi_ctrl->hpd_state =
		(DSS_REG_R(io, HDMI_HPD_INT_STATUS) & BIT(1)) >> 1;
	if (hdmi_ctrl->hpd_state) {
		hdmi_tx_read_sink_info(hdmi_ctrl);
		DEV_INFO("HDMI HPD: sense CONNECTED: send ONLINE\n");
		kobject_uevent(hdmi_ctrl->kobj, KOBJ_ONLINE);
		switch_set_state(&hdmi_ctrl->sdev, 1);
		DEV_INFO("%s: Hdmi state switch to %d\n", __func__,
			hdmi_ctrl->sdev.state);
	} else {
		DEV_INFO("HDMI HPD: sense DISCONNECTED: send OFFLINE\n");
		kobject_uevent(hdmi_ctrl->kobj, KOBJ_OFFLINE);
		switch_set_state(&hdmi_ctrl->sdev, 0);
		DEV_INFO("%s: Hdmi state switch to %d\n", __func__,
			hdmi_ctrl->sdev.state);
	}

	if (!completion_done(&hdmi_ctrl->hpd_done))
		complete_all(&hdmi_ctrl->hpd_done);
} /* hdmi_tx_hpd_int_work */

static int hdmi_tx_check_capability(struct dss_io_data *io)
{
	u32 hdmi_disabled, hdcp_disabled;

	if (!io) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	hdcp_disabled = DSS_REG_R_ND(io,
		QFPROM_RAW_FEAT_CONFIG_ROW0_LSB) & BIT(31);

	hdmi_disabled = DSS_REG_R_ND(io,
		QFPROM_RAW_FEAT_CONFIG_ROW0_MSB) & BIT(0);

	DEV_DBG("%s: Features <HDMI:%s, HDCP:%s>\n", __func__,
		hdmi_disabled ? "OFF" : "ON", hdcp_disabled ? "OFF" : "ON");

	if (hdmi_disabled) {
		DEV_ERR("%s: HDMI disabled\n", __func__);
		return -ENODEV;
	}

	if (hdcp_disabled)
		DEV_WARN("%s: HDCP disabled\n", __func__);

	return 0;
} /* hdmi_tx_check_capability */

static int hdmi_tx_set_video_fmt(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int rc = 0;
	const struct hdmi_disp_mode_timing_type *timing = NULL;
	struct hdmi_tx_platform_data *pdata = NULL;
	u32 format = DEFAULT_VIDEO_RESOLUTION;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		rc = -EINVAL;
		goto end;
	}

	pdata = &hdmi_ctrl->pdata;

	DEV_DBG("%s: Resolution wanted=%dx%d\n", __func__, hdmi_ctrl->xres,
		hdmi_ctrl->yres);
	switch (hdmi_ctrl->xres) {
	default:
	case 640:
		format = HDMI_VFRMT_640x480p60_4_3;
		break;
	case 720:
		format = (hdmi_ctrl->yres == 480)
			? HDMI_VFRMT_720x480p60_16_9
			: HDMI_VFRMT_720x576p50_16_9;
		break;
	case 1280:
		if (hdmi_ctrl->frame_rate == 50000)
			format = HDMI_VFRMT_1280x720p50_16_9;
		else
			format = HDMI_VFRMT_1280x720p60_16_9;
		break;
	case 1440:
		/* interlaced has half of y res. */
		format = (hdmi_ctrl->yres == 240)
			? HDMI_VFRMT_1440x480i60_16_9
			: HDMI_VFRMT_1440x576i50_16_9;
		break;
	case 1920:
		if (hdmi_ctrl->yres == 540) {/* interlaced */
			format = HDMI_VFRMT_1920x1080i60_16_9;
		} else if (hdmi_ctrl->yres == 1080) {
			if (hdmi_ctrl->frame_rate == 50000)
				format = HDMI_VFRMT_1920x1080p50_16_9;
			else if (hdmi_ctrl->frame_rate == 24000)
				format = HDMI_VFRMT_1920x1080p24_16_9;
			else if (hdmi_ctrl->frame_rate == 25000)
				format = HDMI_VFRMT_1920x1080p25_16_9;
			else if (hdmi_ctrl->frame_rate == 30000)
				format = HDMI_VFRMT_1920x1080p30_16_9;
			else
				format = HDMI_VFRMT_1920x1080p60_16_9;
		}
		break;
	}

	if (hdmi_ctrl->video_resolution != format)
		DEV_DBG("%s: switching %s => %s", __func__,
			hdmi_get_video_fmt_2string(
			hdmi_ctrl->video_resolution),
			hdmi_get_video_fmt_2string(format));
	else
		DEV_DBG("resolution %s", hdmi_get_video_fmt_2string(
			hdmi_ctrl->video_resolution));

	timing = hdmi_get_supported_mode(format);
	if (!timing) {
		DEV_ERR("%s: invalid video fmt=%d\n", __func__,
			hdmi_ctrl->video_resolution);
		rc = -EPERM;
		goto end;
	}

	/* todo: find a better way */
	hdmi_ctrl->pdata.power_data[HDMI_TX_CORE_PM].clk_config[0].rate =
		timing->pixel_freq * 1000;

	hdmi_ctrl->video_resolution = format;
	hdmi_edid_set_video_resolution(
		hdmi_ctrl->feature_data[HDMI_TX_FEAT_EDID], format);

end:
	return rc;
} /* hdmi_tx_set_video_fmt */

static void hdmi_tx_video_setup(struct hdmi_tx_ctrl *hdmi_ctrl,
	int video_format)
{
	u32 total_v   = 0;
	u32 total_h   = 0;
	u32 start_h   = 0;
	u32 end_h     = 0;
	u32 start_v   = 0;
	u32 end_v     = 0;
	struct dss_io_data *io = NULL;

	const struct hdmi_disp_mode_timing_type *timing =
		hdmi_get_supported_mode(video_format);
	if (timing == NULL) {
		DEV_ERR("%s: video format not supported: %d\n", __func__,
			video_format);
		return;
	}

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: Core io is not initialized\n", __func__);
		return;
	}

	total_h = timing->active_h + timing->front_porch_h +
		timing->back_porch_h + timing->pulse_width_h - 1;
	total_v = timing->active_v + timing->front_porch_v +
		timing->back_porch_v + timing->pulse_width_v - 1;
	DSS_REG_W(io, HDMI_TOTAL,
		((total_v << 16) & 0x0FFF0000) |
		((total_h << 0) & 0x00000FFF));

	start_h = timing->back_porch_h + timing->pulse_width_h;
	end_h   = (total_h + 1) - timing->front_porch_h;
	DSS_REG_W(io, HDMI_ACTIVE_H,
		((end_h << 16) & 0x0FFF0000) |
		((start_h << 0) & 0x00000FFF));

	start_v = timing->back_porch_v + timing->pulse_width_v - 1;
	end_v   = total_v - timing->front_porch_v;
	DSS_REG_W(io, HDMI_ACTIVE_V,
		((end_v << 16) & 0x0FFF0000) |
		((start_v << 0) & 0x00000FFF));

	if (timing->interlaced) {
		DSS_REG_W(io, HDMI_V_TOTAL_F2,
			((total_v + 1) << 0) & 0x00000FFF);

		DSS_REG_W(io, HDMI_ACTIVE_V_F2,
			(((start_v + 1) << 0) & 0x00000FFF) |
			(((end_v + 1) << 16) & 0x0FFF0000));
	} else {
		DSS_REG_W(io, HDMI_V_TOTAL_F2, 0);
		DSS_REG_W(io, HDMI_ACTIVE_V_F2, 0);
	}

	DSS_REG_W(io, HDMI_FRAME_CTRL,
		((timing->interlaced << 31) & 0x80000000) |
		((timing->active_low_h << 29) & 0x20000000) |
		((timing->active_low_v << 28) & 0x10000000));
} /* hdmi_tx_video_setup */

static void hdmi_tx_set_avi_infoframe(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int i, mode = 0;
	u8 avi_iframe[16]; /* two header + length + 13 data */
	u8 checksum;
	u32 sum, regVal;
	struct dss_io_data *io = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: Core io is not initialized\n", __func__);
		return;
	}

	switch (hdmi_ctrl->video_resolution) {
	case HDMI_VFRMT_720x480p60_4_3:
		mode = 0;
		break;
	case HDMI_VFRMT_720x480i60_16_9:
		mode = 1;
		break;
	case HDMI_VFRMT_720x576p50_16_9:
		mode = 2;
		break;
	case HDMI_VFRMT_720x576i50_16_9:
		mode = 3;
		break;
	case HDMI_VFRMT_1280x720p60_16_9:
		mode = 4;
		break;
	case HDMI_VFRMT_1280x720p50_16_9:
		mode = 5;
		break;
	case HDMI_VFRMT_1920x1080p60_16_9:
		mode = 6;
		break;
	case HDMI_VFRMT_1920x1080i60_16_9:
		mode = 7;
		break;
	case HDMI_VFRMT_1920x1080p50_16_9:
		mode = 8;
		break;
	case HDMI_VFRMT_1920x1080i50_16_9:
		mode = 9;
		break;
	case HDMI_VFRMT_1920x1080p24_16_9:
		mode = 10;
		break;
	case HDMI_VFRMT_1920x1080p30_16_9:
		mode = 11;
		break;
	case HDMI_VFRMT_1920x1080p25_16_9:
		mode = 12;
		break;
	case HDMI_VFRMT_640x480p60_4_3:
		mode = 13;
		break;
	case HDMI_VFRMT_720x480p60_16_9:
		mode = 14;
		break;
	case HDMI_VFRMT_720x576p50_4_3:
		mode = 15;
		break;
	default:
		DEV_INFO("%s: mode %d not supported\n", __func__,
			hdmi_ctrl->video_resolution);
		return;
	}

	/* InfoFrame Type = 82 */
	avi_iframe[0]  = 0x82;
	/* Version = 2 */
	avi_iframe[1]  = 2;
	/* Length of AVI InfoFrame = 13 */
	avi_iframe[2]  = 13;

	/* Data Byte 01: 0 Y1 Y0 A0 B1 B0 S1 S0 */
	avi_iframe[3]  = hdmi_tx_avi_iframe_lut[0][mode];
	avi_iframe[3] |= hdmi_edid_get_sink_scaninfo(
		hdmi_ctrl->feature_data[HDMI_TX_FEAT_EDID],
		hdmi_ctrl->video_resolution);

	/* Data Byte 02: C1 C0 M1 M0 R3 R2 R1 R0 */
	avi_iframe[4]  = hdmi_tx_avi_iframe_lut[1][mode];
	/* Data Byte 03: ITC EC2 EC1 EC0 Q1 Q0 SC1 SC0 */
	avi_iframe[5]  = hdmi_tx_avi_iframe_lut[2][mode];
	/* Data Byte 04: 0 VIC6 VIC5 VIC4 VIC3 VIC2 VIC1 VIC0 */
	avi_iframe[6]  = hdmi_tx_avi_iframe_lut[3][mode];
	/* Data Byte 05: 0 0 0 0 PR3 PR2 PR1 PR0 */
	avi_iframe[7]  = hdmi_tx_avi_iframe_lut[4][mode];
	/* Data Byte 06: LSB Line No of End of Top Bar */
	avi_iframe[8]  = hdmi_tx_avi_iframe_lut[5][mode];
	/* Data Byte 07: MSB Line No of End of Top Bar */
	avi_iframe[9]  = hdmi_tx_avi_iframe_lut[6][mode];
	/* Data Byte 08: LSB Line No of Start of Bottom Bar */
	avi_iframe[10] = hdmi_tx_avi_iframe_lut[7][mode];
	/* Data Byte 09: MSB Line No of Start of Bottom Bar */
	avi_iframe[11] = hdmi_tx_avi_iframe_lut[8][mode];
	/* Data Byte 10: LSB Pixel Number of End of Left Bar */
	avi_iframe[12] = hdmi_tx_avi_iframe_lut[9][mode];
	/* Data Byte 11: MSB Pixel Number of End of Left Bar */
	avi_iframe[13] = hdmi_tx_avi_iframe_lut[10][mode];
	/* Data Byte 12: LSB Pixel Number of Start of Right Bar */
	avi_iframe[14] = hdmi_tx_avi_iframe_lut[11][mode];
	/* Data Byte 13: MSB Pixel Number of Start of Right Bar */
	avi_iframe[15] = hdmi_tx_avi_iframe_lut[12][mode];

	sum = 0;
	for (i = 0; i < 16; i++)
		sum += avi_iframe[i];
	sum &= 0xFF;
	sum = 256 - sum;
	checksum = (u8) sum;

	regVal = avi_iframe[5];
	regVal = regVal << 8 | avi_iframe[4];
	regVal = regVal << 8 | avi_iframe[3];
	regVal = regVal << 8 | checksum;
	DSS_REG_W(io, HDMI_AVI_INFO0, regVal);

	regVal = avi_iframe[9];
	regVal = regVal << 8 | avi_iframe[8];
	regVal = regVal << 8 | avi_iframe[7];
	regVal = regVal << 8 | avi_iframe[6];
	DSS_REG_W(io, HDMI_AVI_INFO1, regVal);

	regVal = avi_iframe[13];
	regVal = regVal << 8 | avi_iframe[12];
	regVal = regVal << 8 | avi_iframe[11];
	regVal = regVal << 8 | avi_iframe[10];
	DSS_REG_W(io, HDMI_AVI_INFO2, regVal);

	regVal = avi_iframe[1];
	regVal = regVal << 16 | avi_iframe[15];
	regVal = regVal << 8 | avi_iframe[14];
	DSS_REG_W(io, HDMI_AVI_INFO3, regVal);

	/* 0x3 for AVI InfFrame enable (every frame) */
	DSS_REG_W(io, HDMI_INFOFRAME_CTRL0,
		DSS_REG_R(io, HDMI_INFOFRAME_CTRL0) |
		0x00000003L);
} /* hdmi_tx_set_avi_infoframe */

static void hdmi_tx_set_spd_infoframe(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	u32 packet_header  = 0;
	u32 check_sum      = 0;
	u32 packet_payload = 0;
	u32 packet_control = 0;

	u8 *vendor_name = NULL;
	u8 *product_description = NULL;
	struct dss_io_data *io = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: Core io is not initialized\n", __func__);
		return;
	}

	vendor_name = hdmi_ctrl->spd_vendor_name;
	product_description = hdmi_ctrl->spd_product_description;

	/* Setup Packet header and payload */
	/*
	 * 0x83 InfoFrame Type Code
	 * 0x01 InfoFrame Version Number
	 * 0x19 Length of Source Product Description InfoFrame
	 */
	packet_header  = 0x83 | (0x01 << 8) | (0x19 << 16);
	DSS_REG_W(io, HDMI_GENERIC1_HDR, packet_header);
	check_sum += IFRAME_CHECKSUM_32(packet_header);

	packet_payload = (vendor_name[3] & 0x7f)
		| ((vendor_name[4] & 0x7f) << 8)
		| ((vendor_name[5] & 0x7f) << 16)
		| ((vendor_name[6] & 0x7f) << 24);
	DSS_REG_W(io, HDMI_GENERIC1_1, packet_payload);
	check_sum += IFRAME_CHECKSUM_32(packet_payload);

	/* Product Description (7-bit ASCII code) */
	packet_payload = (vendor_name[7] & 0x7f)
		| ((product_description[0] & 0x7f) << 8)
		| ((product_description[1] & 0x7f) << 16)
		| ((product_description[2] & 0x7f) << 24);
	DSS_REG_W(io, HDMI_GENERIC1_2, packet_payload);
	check_sum += IFRAME_CHECKSUM_32(packet_payload);

	packet_payload = (product_description[3] & 0x7f)
		| ((product_description[4] & 0x7f) << 8)
		| ((product_description[5] & 0x7f) << 16)
		| ((product_description[6] & 0x7f) << 24);
	DSS_REG_W(io, HDMI_GENERIC1_3, packet_payload);
	check_sum += IFRAME_CHECKSUM_32(packet_payload);

	packet_payload = (product_description[7] & 0x7f)
		| ((product_description[8] & 0x7f) << 8)
		| ((product_description[9] & 0x7f) << 16)
		| ((product_description[10] & 0x7f) << 24);
	DSS_REG_W(io, HDMI_GENERIC1_4, packet_payload);
	check_sum += IFRAME_CHECKSUM_32(packet_payload);

	packet_payload = (product_description[11] & 0x7f)
		| ((product_description[12] & 0x7f) << 8)
		| ((product_description[13] & 0x7f) << 16)
		| ((product_description[14] & 0x7f) << 24);
	DSS_REG_W(io, HDMI_GENERIC1_5, packet_payload);
	check_sum += IFRAME_CHECKSUM_32(packet_payload);

	/*
	 * Source Device Information
	 * 00h unknown
	 * 01h Digital STB
	 * 02h DVD
	 * 03h D-VHS
	 * 04h HDD Video
	 * 05h DVC
	 * 06h DSC
	 * 07h Video CD
	 * 08h Game
	 * 09h PC general
	 */
	packet_payload = (product_description[15] & 0x7f) | 0x00 << 8;
	DSS_REG_W(io, HDMI_GENERIC1_6, packet_payload);
	check_sum += IFRAME_CHECKSUM_32(packet_payload);

	/* Vendor Name (7bit ASCII code) */
	packet_payload = ((vendor_name[0] & 0x7f) << 8)
		| ((vendor_name[1] & 0x7f) << 16)
		| ((vendor_name[2] & 0x7f) << 24);
	check_sum += IFRAME_CHECKSUM_32(packet_payload);
	packet_payload |= ((0x100 - (0xff & check_sum)) & 0xff);
	DSS_REG_W(io, HDMI_GENERIC1_0, packet_payload);

	/*
	 * GENERIC1_LINE | GENERIC1_CONT | GENERIC1_SEND
	 * Setup HDMI TX generic packet control
	 * Enable this packet to transmit every frame
	 * Enable HDMI TX engine to transmit Generic packet 1
	 */
	packet_control = DSS_REG_R_ND(io, HDMI_GEN_PKT_CTRL);
	packet_control |= ((0x1 << 24) | (1 << 5) | (1 << 4));
	DSS_REG_W(io, HDMI_GEN_PKT_CTRL, packet_control);
} /* hdmi_tx_set_spd_infoframe */

static void hdmi_tx_set_mode(struct hdmi_tx_ctrl *hdmi_ctrl, u32 power_on)
{
	u32 reg_val = 0;
	struct dss_io_data *io = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}
	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: Core io is not initialized\n", __func__);
		return;
	}

	if (power_on) {
		/* ENABLE */
		reg_val |= BIT(0); /* Enable the block */
		if (hdmi_edid_get_sink_mode(
			hdmi_ctrl->feature_data[HDMI_TX_FEAT_EDID]) == 0) {
			if (hdmi_ctrl->present_hdcp)
				/* HDMI Encryption */
				reg_val |= BIT(2);
			reg_val |= BIT(1);
		} else {
			if (hdmi_ctrl->present_hdcp)
				/* HDMI_Encryption_ON */
				reg_val |= BIT(1) | BIT(2);
			else
				reg_val |= BIT(1);
		}
	} else {
		reg_val = BIT(1);
	}

	DSS_REG_W(io, HDMI_CTRL, reg_val);

	DEV_DBG("HDMI Core: %s, HDMI_CTRL=0x%08x\n",
		power_on ? "Enable" : "Disable", reg_val);
} /* hdmi_tx_set_mode */

static int hdmi_tx_config_power(struct hdmi_tx_ctrl *hdmi_ctrl,
	enum hdmi_tx_power_module_type module, int config)
{
	int rc = 0;
	struct dss_module_power *power_data = NULL;

	if (!hdmi_ctrl || module >= HDMI_TX_MAX_PM) {
		DEV_ERR("%s: Error: invalid input\n", __func__);
		rc = -EINVAL;
		goto exit;
	}

	power_data = &hdmi_ctrl->pdata.power_data[module];
	if (!power_data) {
		DEV_ERR("%s: Error: invalid power data\n", __func__);
		rc = -EINVAL;
		goto exit;
	}

	if (config) {
		rc = msm_dss_config_vreg(&hdmi_ctrl->pdev->dev,
			power_data->vreg_config, power_data->num_vreg, 1);
		if (rc) {
			DEV_ERR("%s: Failed to config %s vreg. Err=%d\n",
				__func__, hdmi_tx_pm_name(module), rc);
			goto exit;
		}

		rc = msm_dss_get_clk(&hdmi_ctrl->pdev->dev,
			power_data->clk_config, power_data->num_clk);
		if (rc) {
			DEV_ERR("%s: Failed to get %s clk. Err=%d\n",
				__func__, hdmi_tx_pm_name(module), rc);

			msm_dss_config_vreg(&hdmi_ctrl->pdev->dev,
			power_data->vreg_config, power_data->num_vreg, 0);
		}
	} else {
		msm_dss_put_clk(power_data->clk_config, power_data->num_clk);

		rc = msm_dss_config_vreg(&hdmi_ctrl->pdev->dev,
			power_data->vreg_config, power_data->num_vreg, 0);
		if (rc)
			DEV_ERR("%s: Fail to deconfig %s vreg. Err=%d\n",
				__func__, hdmi_tx_pm_name(module), rc);
	}

exit:
	return rc;
} /* hdmi_tx_config_power */

static int hdmi_tx_enable_power(struct hdmi_tx_ctrl *hdmi_ctrl,
	enum hdmi_tx_power_module_type module, int enable)
{
	int rc = 0;
	struct dss_module_power *power_data = NULL;

	if (!hdmi_ctrl || module >= HDMI_TX_MAX_PM) {
		DEV_ERR("%s: Error: invalid input\n", __func__);
		rc = -EINVAL;
		goto error;
	}

	power_data = &hdmi_ctrl->pdata.power_data[module];
	if (!power_data) {
		DEV_ERR("%s: Error: invalid power data\n", __func__);
		rc = -EINVAL;
		goto error;
	}

	if (enable) {
		rc = msm_dss_enable_vreg(power_data->vreg_config,
			power_data->num_vreg, 1);
		if (rc) {
			DEV_ERR("%s: Failed to enable %s vreg. Error=%d\n",
				__func__, hdmi_tx_pm_name(module), rc);
			goto error;
		}

		rc = msm_dss_enable_gpio(power_data->gpio_config,
			power_data->num_gpio, 1);
		if (rc) {
			DEV_ERR("%s: Failed to enable %s gpio. Error=%d\n",
				__func__, hdmi_tx_pm_name(module), rc);
			goto disable_vreg;
		}

		rc = msm_dss_clk_set_rate(power_data->clk_config,
			power_data->num_clk);
		if (rc) {
			DEV_ERR("%s: failed to set clks rate for %s. err=%d\n",
				__func__, hdmi_tx_pm_name(module), rc);
			goto disable_gpio;
		}

		rc = msm_dss_enable_clk(power_data->clk_config,
			power_data->num_clk, 1);
		if (rc) {
			DEV_ERR("%s: Failed to enable clks for %s. Error=%d\n",
				__func__, hdmi_tx_pm_name(module), rc);
			goto disable_gpio;
		}
	} else {
		msm_dss_enable_clk(power_data->clk_config,
			power_data->num_clk, 0);
		msm_dss_clk_set_rate(power_data->clk_config,
			power_data->num_clk);
		msm_dss_enable_gpio(power_data->gpio_config,
			power_data->num_gpio, 0);
		msm_dss_enable_vreg(power_data->vreg_config,
			power_data->num_vreg, 0);
	}

	return rc;

disable_gpio:
	msm_dss_enable_gpio(power_data->gpio_config, power_data->num_gpio, 0);
disable_vreg:
	msm_dss_enable_vreg(power_data->vreg_config, power_data->num_vreg, 0);
error:
	return rc;
} /* hdmi_tx_enable_power */

static void hdmi_tx_core_off(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	hdmi_tx_enable_power(hdmi_ctrl, HDMI_TX_CEC_PM, 0);
	hdmi_tx_enable_power(hdmi_ctrl, HDMI_TX_CORE_PM, 0);
} /* hdmi_tx_core_off */

static int hdmi_tx_core_on(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int rc = 0;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	rc = hdmi_tx_enable_power(hdmi_ctrl, HDMI_TX_CORE_PM, 1);
	if (rc) {
		DEV_ERR("%s: core hdmi_msm_enable_power failed rc = %d\n",
			__func__, rc);
		return rc;
	}
	rc = hdmi_tx_enable_power(hdmi_ctrl, HDMI_TX_CEC_PM, 1);
	if (rc) {
		DEV_ERR("%s: cec hdmi_msm_enable_power failed rc = %d\n",
			__func__, rc);
		goto disable_core_power;
	}

	return rc;
disable_core_power:
	hdmi_tx_enable_power(hdmi_ctrl, HDMI_TX_CORE_PM, 0);
	return rc;
} /* hdmi_tx_core_on */

static void hdmi_tx_phy_reset(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	unsigned int phy_reset_polarity = 0x0;
	unsigned int pll_reset_polarity = 0x0;
	unsigned int val;
	struct dss_io_data *io = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: core io not inititalized\n", __func__);
		return;
	}

	val = DSS_REG_R_ND(io, HDMI_PHY_CTRL);

	phy_reset_polarity = val >> 3 & 0x1;
	pll_reset_polarity = val >> 1 & 0x1;

	if (phy_reset_polarity == 0)
		DSS_REG_W_ND(io, HDMI_PHY_CTRL, val | SW_RESET);
	else
		DSS_REG_W_ND(io, HDMI_PHY_CTRL, val & (~SW_RESET));

	if (pll_reset_polarity == 0)
		DSS_REG_W_ND(io, HDMI_PHY_CTRL, val | SW_RESET_PLL);
	else
		DSS_REG_W_ND(io, HDMI_PHY_CTRL, val & (~SW_RESET_PLL));

	if (phy_reset_polarity == 0)
		DSS_REG_W_ND(io, HDMI_PHY_CTRL, val & (~SW_RESET));
	else
		DSS_REG_W_ND(io, HDMI_PHY_CTRL, val | SW_RESET);

	if (pll_reset_polarity == 0)
		DSS_REG_W_ND(io, HDMI_PHY_CTRL, val & (~SW_RESET_PLL));
	else
		DSS_REG_W_ND(io, HDMI_PHY_CTRL, val | SW_RESET_PLL);
} /* hdmi_tx_phy_reset */

static void hdmi_tx_init_phy(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	struct dss_io_data *io = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_PHY_IO];
	if (!io->base) {
		DEV_ERR("%s: phy io is not initialized\n", __func__);
		return;
	}

	DSS_REG_W_ND(io, HDMI_PHY_ANA_CFG0, 0x1B);
	DSS_REG_W_ND(io, HDMI_PHY_ANA_CFG1, 0xF2);
	DSS_REG_W_ND(io, HDMI_PHY_BIST_CFG0, 0x0);
	DSS_REG_W_ND(io, HDMI_PHY_BIST_PATN0, 0x0);
	DSS_REG_W_ND(io, HDMI_PHY_BIST_PATN1, 0x0);
	DSS_REG_W_ND(io, HDMI_PHY_BIST_PATN2, 0x0);
	DSS_REG_W_ND(io, HDMI_PHY_BIST_PATN3, 0x0);

	DSS_REG_W_ND(io, HDMI_PHY_PD_CTRL1, 0x20);
} /* hdmi_tx_init_phy */

static void hdmi_tx_powerdown_phy(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	struct dss_io_data *io = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}
	io = &hdmi_ctrl->pdata.io[HDMI_TX_PHY_IO];
	if (!io->base) {
		DEV_ERR("%s: phy io is not initialized\n", __func__);
		return;
	}

	DSS_REG_W_ND(io, HDMI_PHY_PD_CTRL0, 0x7F);
} /* hdmi_tx_powerdown_phy */

static int hdmi_tx_audio_acr_setup(struct hdmi_tx_ctrl *hdmi_ctrl,
	bool enabled, int num_of_channels)
{
	/* Read first before writing */
	u32 acr_pck_ctrl_reg;
	struct dss_io_data *io = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: Invalid input\n", __func__);
		return -EINVAL;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: core io not inititalized\n", __func__);
		return -EINVAL;
	}

	acr_pck_ctrl_reg = DSS_REG_R(io, HDMI_ACR_PKT_CTRL);

	if (enabled) {
		const struct hdmi_disp_mode_timing_type *timing =
			hdmi_get_supported_mode(hdmi_ctrl->video_resolution);
		const struct hdmi_tx_audio_acr_arry *audio_acr =
			&hdmi_tx_audio_acr_lut[0];
		const int lut_size = sizeof(hdmi_tx_audio_acr_lut)
			/ sizeof(*hdmi_tx_audio_acr_lut);
		u32 i, n, cts, layout, multiplier, aud_pck_ctrl_2_reg;

		if (timing == NULL) {
			DEV_WARN("%s: video format %d not supported\n",
				__func__, hdmi_ctrl->video_resolution);
			return -EPERM;
		}

		for (i = 0; i < lut_size;
			audio_acr = &hdmi_tx_audio_acr_lut[++i]) {
			if (audio_acr->pclk == timing->pixel_freq)
				break;
		}
		if (i >= lut_size) {
			DEV_WARN("%s: pixel clk %d not supported\n", __func__,
				timing->pixel_freq);
			return -EPERM;
		}

		n = audio_acr->lut[hdmi_ctrl->audio_sample_rate].n;
		cts = audio_acr->lut[hdmi_ctrl->audio_sample_rate].cts;
		layout = (MSM_HDMI_AUDIO_CHANNEL_2 == num_of_channels) ? 0 : 1;

		if (
		(HDMI_SAMPLE_RATE_192KHZ == hdmi_ctrl->audio_sample_rate) ||
		(HDMI_SAMPLE_RATE_176_4KHZ == hdmi_ctrl->audio_sample_rate)) {
			multiplier = 4;
			n >>= 2; /* divide N by 4 and use multiplier */
		} else if (
		(HDMI_SAMPLE_RATE_96KHZ == hdmi_ctrl->audio_sample_rate) ||
		(HDMI_SAMPLE_RATE_88_2KHZ == hdmi_ctrl->audio_sample_rate)) {
			multiplier = 2;
			n >>= 1; /* divide N by 2 and use multiplier */
		} else {
			multiplier = 1;
		}
		DEV_DBG("%s: n=%u, cts=%u, layout=%u\n", __func__, n, cts,
			layout);

		/* AUDIO_PRIORITY | SOURCE */
		acr_pck_ctrl_reg |= 0x80000100;
		/* N_MULTIPLE(multiplier) */
		acr_pck_ctrl_reg |= (multiplier & 7) << 16;

		if ((HDMI_SAMPLE_RATE_48KHZ == hdmi_ctrl->audio_sample_rate) ||
		(HDMI_SAMPLE_RATE_96KHZ == hdmi_ctrl->audio_sample_rate) ||
		(HDMI_SAMPLE_RATE_192KHZ == hdmi_ctrl->audio_sample_rate)) {
			/* SELECT(3) */
			acr_pck_ctrl_reg |= 3 << 4;
			/* CTS_48 */
			cts <<= 12;

			/* CTS: need to determine how many fractional bits */
			DSS_REG_W(io, HDMI_ACR_48_0, cts);
			/* N */
			DSS_REG_W(io, HDMI_ACR_48_1, n);
		} else if (
		(HDMI_SAMPLE_RATE_44_1KHZ == hdmi_ctrl->audio_sample_rate) ||
		(HDMI_SAMPLE_RATE_88_2KHZ == hdmi_ctrl->audio_sample_rate) ||
		(HDMI_SAMPLE_RATE_176_4KHZ == hdmi_ctrl->audio_sample_rate)) {
			/* SELECT(2) */
			acr_pck_ctrl_reg |= 2 << 4;
			/* CTS_44 */
			cts <<= 12;

			/* CTS: need to determine how many fractional bits */
			DSS_REG_W(io, HDMI_ACR_44_0, cts);
			/* N */
			DSS_REG_W(io, HDMI_ACR_44_1, n);
		} else {	/* default to 32k */
			/* SELECT(1) */
			acr_pck_ctrl_reg |= 1 << 4;
			/* CTS_32 */
			cts <<= 12;

			/* CTS: need to determine how many fractional bits */
			DSS_REG_W(io, HDMI_ACR_32_0, cts);
			/* N */
			DSS_REG_W(io, HDMI_ACR_32_1, n);
		}
		/* Payload layout depends on number of audio channels */
		/* LAYOUT_SEL(layout) */
		aud_pck_ctrl_2_reg = 1 | (layout << 1);
		/* override | layout */
		DSS_REG_W(io, HDMI_AUDIO_PKT_CTRL2, aud_pck_ctrl_2_reg);

		/* SEND | CONT */
		acr_pck_ctrl_reg |= 0x00000003;
	} else {
		/* ~(SEND | CONT) */
		acr_pck_ctrl_reg &= ~0x00000003;
	}
	DSS_REG_W(io, HDMI_ACR_PKT_CTRL, acr_pck_ctrl_reg);

	return 0;
} /* hdmi_tx_audio_acr_setup */

static int hdmi_tx_audio_info_setup(void *priv_d, bool enabled,
	u32 num_of_channels, u32 channel_allocation, u32 level_shift,
	bool down_mix)
{
	struct hdmi_tx_ctrl *hdmi_ctrl = (struct hdmi_tx_ctrl *)priv_d;
	struct dss_io_data *io = NULL;

	u32 channel_count = 1; /* Def to 2 channels -> Table 17 in CEA-D */
	u32 check_sum, audio_info_0_reg, audio_info_1_reg;
	u32 audio_info_ctrl_reg;
	u32 aud_pck_ctrl_2_reg;
	u32 layout;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: core io not inititalized\n", __func__);
		return -EINVAL;
	}

	layout = (MSM_HDMI_AUDIO_CHANNEL_2 == num_of_channels) ? 0 : 1;
	aud_pck_ctrl_2_reg = 1 | (layout << 1);
	DSS_REG_W(io, HDMI_AUDIO_PKT_CTRL2, aud_pck_ctrl_2_reg);

	/*
	 * Please see table 20 Audio InfoFrame in HDMI spec
	 * FL  = front left
	 * FC  = front Center
	 * FR  = front right
	 * FLC = front left center
	 * FRC = front right center
	 * RL  = rear left
	 * RC  = rear center
	 * RR  = rear right
	 * RLC = rear left center
	 * RRC = rear right center
	 * LFE = low frequency effect
	 */

	/* Read first then write because it is bundled with other controls */
	audio_info_ctrl_reg = DSS_REG_R(io, HDMI_INFOFRAME_CTRL0);

	if (enabled) {
		switch (num_of_channels) {
		case MSM_HDMI_AUDIO_CHANNEL_2:
			channel_allocation = 0;	/* Default to FR, FL */
			break;
		case MSM_HDMI_AUDIO_CHANNEL_4:
			channel_count = 3;
			/* FC, LFE, FR, FL */
			channel_allocation = 0x3;
			break;
		case MSM_HDMI_AUDIO_CHANNEL_6:
			channel_count = 5;
			/* RR, RL, FC, LFE, FR, FL */
			channel_allocation = 0xB;
			break;
		case MSM_HDMI_AUDIO_CHANNEL_8:
			channel_count = 7;
			/* FRC, FLC, RR, RL, FC, LFE, FR, FL */
			channel_allocation = 0x1f;
			break;
		default:
			DEV_ERR("%s: Unsupported num_of_channels = %u\n",
				__func__, num_of_channels);
			return -EINVAL;
		}

		/* Program the Channel-Speaker allocation */
		audio_info_1_reg = 0;
		/* CA(channel_allocation) */
		audio_info_1_reg |= channel_allocation & 0xff;
		/* Program the Level shifter */
		audio_info_1_reg |= (level_shift << 11) & 0x00007800;
		/* Program the Down-mix Inhibit Flag */
		audio_info_1_reg |= (down_mix << 15) & 0x00008000;

		DSS_REG_W(io, HDMI_AUDIO_INFO1, audio_info_1_reg);

		/*
		 * Calculate CheckSum: Sum of all the bytes in the
		 * Audio Info Packet (See table 8.4 in HDMI spec)
		 */
		check_sum = 0;
		/* HDMI_AUDIO_INFO_FRAME_PACKET_HEADER_TYPE[0x84] */
		check_sum += 0x84;
		/* HDMI_AUDIO_INFO_FRAME_PACKET_HEADER_VERSION[0x01] */
		check_sum += 1;
		/* HDMI_AUDIO_INFO_FRAME_PACKET_LENGTH[0x0A] */
		check_sum += 0x0A;
		check_sum += channel_count;
		check_sum += channel_allocation;
		/* See Table 8.5 in HDMI spec */
		check_sum += (level_shift & 0xF) << 3 | (down_mix & 0x1) << 7;
		check_sum &= 0xFF;
		check_sum = (u8) (256 - check_sum);

		audio_info_0_reg = 0;
		/* CHECKSUM(check_sum) */
		audio_info_0_reg |= check_sum & 0xff;
		/* CC(channel_count) */
		audio_info_0_reg |= (channel_count << 8) & 0x00000700;

		DSS_REG_W(io, HDMI_AUDIO_INFO0, audio_info_0_reg);

		/*
		 * Set these flags
		 * AUDIO_INFO_UPDATE |
		 * AUDIO_INFO_SOURCE |
		 * AUDIO_INFO_CONT   |
		 * AUDIO_INFO_SEND
		 */
		audio_info_ctrl_reg |= 0x000000F0;
	} else {
		/*Clear these flags
		 * ~(AUDIO_INFO_UPDATE |
		 *   AUDIO_INFO_SOURCE |
		 *   AUDIO_INFO_CONT   |
		 *   AUDIO_INFO_SEND)
		 */
		audio_info_ctrl_reg &= ~0x000000F0;
	}
	DSS_REG_W(io, HDMI_INFOFRAME_CTRL0, audio_info_ctrl_reg);

	dss_reg_dump(io->base, io->len,
		enabled ? "HDMI-AUDIO-ON: " : "HDMI-AUDIO-OFF: ", REG_DUMP);

	return 0;
} /* hdmi_tx_audio_info_setup */

static int hdmi_tx_audio_setup(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int rc = 0;
	const int channels = MSM_HDMI_AUDIO_CHANNEL_2;
	struct dss_io_data *io = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: core io not inititalized\n", __func__);
		return -EINVAL;
	}

	rc = hdmi_tx_audio_acr_setup(hdmi_ctrl, true, channels);
	if (rc) {
		DEV_ERR("%s: hdmi_tx_audio_acr_setup failed. rc=%d\n",
			__func__, rc);
		return rc;
	}

	rc = hdmi_tx_audio_info_setup(hdmi_ctrl, true, channels, 0, 0, false);
	if (rc) {
		DEV_ERR("%s: hdmi_tx_audio_info_setup failed. rc=%d\n",
			__func__, rc);
		return rc;
	}

	DEV_INFO("HDMI Audio: Enabled\n");

	return 0;
} /* hdmi_tx_audio_setup */

static void hdmi_tx_audio_off(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	u32 i, status, max_reads, timeout_us, timeout_sec = 15;
	struct dss_io_data *io = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: core io not inititalized\n", __func__);
		return;
	}

	/* Check if audio engine is turned off by QDSP or not */
	/* send off notification after every 1 sec for 15 seconds */
	for (i = 0; i < timeout_sec; i++) {
		max_reads = 500;
		timeout_us = 1000 * 2;

		if (readl_poll_timeout_noirq((io->base + HDMI_AUDIO_CFG),
			status, ((status & BIT(0)) == 0),
			max_reads, timeout_us)) {

			DEV_ERR("%s: audio still on after %d sec. try again\n",
				__func__, i+1);

			switch_set_state(&hdmi_ctrl->audio_sdev, 0);
			continue;
		}
		break;
	}
	if (i == timeout_sec)
		DEV_ERR("%s: Error: cannot turn off audio engine\n", __func__);

	if (hdmi_tx_audio_info_setup(hdmi_ctrl, false, 0, 0, 0, false))
		DEV_ERR("%s: hdmi_tx_audio_info_setup failed.\n", __func__);

	if (hdmi_tx_audio_acr_setup(hdmi_ctrl, false, 0))
		DEV_ERR("%s: hdmi_tx_audio_acr_setup failed.\n", __func__);

	DEV_INFO("HDMI Audio: Disabled\n");
} /* hdmi_tx_audio_off */

static int hdmi_tx_start(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int rc = 0;
	struct dss_io_data *io = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}
	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: core io is not initialized\n", __func__);
		return -EINVAL;
	}

	hdmi_tx_set_mode(hdmi_ctrl, false);
	hdmi_tx_init_phy(hdmi_ctrl);
	DSS_REG_W(io, HDMI_USEC_REFTIMER, 0x0001001B);

	hdmi_tx_set_mode(hdmi_ctrl, true);

	hdmi_tx_video_setup(hdmi_ctrl, hdmi_ctrl->video_resolution);

	if (!hdmi_tx_is_dvi_mode(hdmi_ctrl)) {
		rc = hdmi_tx_audio_setup(hdmi_ctrl);
		if (rc) {
			DEV_ERR("%s: hdmi_msm_audio_setup failed. rc=%d\n",
				__func__, rc);
			hdmi_tx_set_mode(hdmi_ctrl, false);
			return rc;
		}

		switch_set_state(&hdmi_ctrl->audio_sdev, 1);
		DEV_INFO("%s: hdmi_audio state switch to %d\n", __func__,
			hdmi_ctrl->audio_sdev.state);
	}

	hdmi_tx_set_avi_infoframe(hdmi_ctrl);
	/* todo: CONFIG_FB_MSM_HDMI_3D */
	hdmi_tx_set_spd_infoframe(hdmi_ctrl);

	/* todo: HDCP/CEC */

	DEV_INFO("%s: HDMI Core: Initialized\n", __func__);

	return rc;
} /* hdmi_tx_start */

static void hdmi_tx_hpd_polarity_setup(struct hdmi_tx_ctrl *hdmi_ctrl,
	bool polarity)
{
	struct dss_io_data *io = NULL;
	u32 cable_sense;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}
	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: core io is not initialized\n", __func__);
		return;
	}

	if (polarity)
		DSS_REG_W(io, HDMI_HPD_INT_CTRL, BIT(2) | BIT(1));
	else
		DSS_REG_W(io, HDMI_HPD_INT_CTRL, BIT(2));

	cable_sense = (DSS_REG_R(io, HDMI_HPD_INT_STATUS) & BIT(1)) >> 1;
	DEV_DBG("%s: listen = %s, sense = %s\n", __func__,
		polarity ? "connect" : "disconnect",
		cable_sense ? "connect" : "disconnect");

	if (cable_sense == polarity) {
		u32 reg_val = DSS_REG_R(io, HDMI_HPD_CTRL);

		/* Toggle HPD circuit to trigger HPD sense */
		DSS_REG_W(io, HDMI_HPD_CTRL, reg_val & ~BIT(28));
		DSS_REG_W(io, HDMI_HPD_CTRL, reg_val | BIT(28));
	}
} /* hdmi_tx_hpd_polarity_setup */

static void hdmi_tx_power_off_work(struct work_struct *work)
{
	struct hdmi_tx_ctrl *hdmi_ctrl = NULL;
	struct dss_io_data *io = NULL;

	hdmi_ctrl = container_of(work, struct hdmi_tx_ctrl, power_off_work);
	if (!hdmi_ctrl) {
		DEV_DBG("%s: invalid input\n", __func__);
		return;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: Core io is not initialized\n", __func__);
		return;
	}

	if (!hdmi_tx_is_dvi_mode(hdmi_ctrl)) {
		switch_set_state(&hdmi_ctrl->audio_sdev, 0);
		DEV_INFO("%s: hdmi_audio state switch to %d\n", __func__,
			hdmi_ctrl->audio_sdev.state);

		hdmi_tx_audio_off(hdmi_ctrl);
	}

	hdmi_tx_powerdown_phy(hdmi_ctrl);

	/*
	 * this is needed to avoid pll lock failure due to
	 * clk framework's rate caching.
	 */
	hdmi_ctrl->pdata.power_data[HDMI_TX_CORE_PM].clk_config[0].rate = 0;

	hdmi_tx_core_off(hdmi_ctrl);

	mutex_lock(&hdmi_ctrl->mutex);
	hdmi_ctrl->panel_power_on = false;
	mutex_unlock(&hdmi_ctrl->mutex);

	if (hdmi_ctrl->hpd_off_pending) {
		hdmi_tx_hpd_off(hdmi_ctrl);
		hdmi_ctrl->hpd_off_pending = false;
	} else {
		hdmi_tx_hpd_polarity_setup(hdmi_ctrl, HPD_CONNECT_POLARITY);
	}

	DEV_INFO("%s: HDMI Core: OFF\n", __func__);
} /* hdmi_tx_power_off_work */

static int hdmi_tx_power_off(struct mdss_panel_data *panel_data)
{
	struct hdmi_tx_ctrl *hdmi_ctrl =
		hdmi_tx_get_drvdata_from_panel_data(panel_data);

	if (!hdmi_ctrl || !hdmi_ctrl->panel_power_on) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	/*
	 * Queue work item to handle power down sequence.
	 * This is needed since we need to wait for the audio engine
	 * to shutdown first before we shutdown the HDMI core.
	 */
	DEV_DBG("%s: Queuing work to power off HDMI core\n", __func__);
	queue_work(hdmi_ctrl->workq, &hdmi_ctrl->power_off_work);

	return 0;
} /* hdmi_tx_power_off */

static int hdmi_tx_power_on(struct mdss_panel_data *panel_data)
{
	int rc = 0;
	struct dss_io_data *io = NULL;
	struct hdmi_tx_ctrl *hdmi_ctrl =
		hdmi_tx_get_drvdata_from_panel_data(panel_data);

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}
	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: core io is not initialized\n", __func__);
		return -EINVAL;
	}

	if (!hdmi_ctrl->hpd_initialized) {
		DEV_ERR("%s: HDMI on is not possible w/o cable detection.\n",
			__func__);
		return -EPERM;
	}

	/* If a power down is already underway, wait for it to finish */
	flush_work_sync(&hdmi_ctrl->power_off_work);

	DEV_INFO("power: ON (%dx%d %ld)\n", hdmi_ctrl->xres, hdmi_ctrl->yres,
		hdmi_ctrl->pixel_clk);

	rc = hdmi_tx_set_video_fmt(hdmi_ctrl);
	if (rc) {
		DEV_ERR("%s: cannot set video_fmt.rc=%d\n", __func__, rc);
		return rc;
	}

	rc = hdmi_tx_core_on(hdmi_ctrl);
	if (rc) {
		DEV_ERR("%s: hdmi_msm_core_on failed\n", __func__);
		return rc;
	}

	mutex_lock(&hdmi_ctrl->mutex);
	hdmi_ctrl->panel_power_on = true;

	if (hdmi_ctrl->hpd_state) {
		DEV_DBG("%s: Turning HDMI on\n", __func__);
		mutex_unlock(&hdmi_ctrl->mutex);
		rc = hdmi_tx_start(hdmi_ctrl);
		if (rc) {
			DEV_ERR("%s: hdmi_tx_start failed. rc=%d\n",
				__func__, rc);
			hdmi_tx_power_off(panel_data);
			return rc;
		}
		/* todo: HDCP */
	} else {
		mutex_unlock(&hdmi_ctrl->mutex);
	}

	dss_reg_dump(io->base, io->len, "HDMI-ON: ", REG_DUMP);

	DEV_INFO("%s: HDMI=%s DVI= %s\n", __func__,
		hdmi_tx_is_controller_on(hdmi_ctrl) ? "ON" : "OFF" ,
		hdmi_tx_is_dvi_mode(hdmi_ctrl) ? "ON" : "OFF");

	hdmi_tx_hpd_polarity_setup(hdmi_ctrl, HPD_DISCONNECT_POLARITY);

	return 0;
} /* hdmi_tx_power_on */

static void hdmi_tx_hpd_off(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int rc = 0;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	if (!hdmi_ctrl->hpd_initialized) {
		DEV_DBG("%s: HPD is already OFF, returning\n", __func__);
		return;
	}

	mdss_disable_irq(&hdmi_tx_hw);

	hdmi_tx_set_mode(hdmi_ctrl, false);

	rc = hdmi_tx_enable_power(hdmi_ctrl, HDMI_TX_HPD_PM, 0);
	if (rc)
		DEV_INFO("%s: Failed to disable hpd power. Error=%d\n",
			__func__, rc);

	hdmi_ctrl->hpd_initialized = false;
} /* hdmi_tx_hpd_off */

static int hdmi_tx_hpd_on(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	u32 reg_val;
	int rc = 0;
	struct dss_io_data *io = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_ERR("%s: core io not inititalized\n", __func__);
		return -EINVAL;
	}

	if (hdmi_ctrl->hpd_initialized) {
		DEV_DBG("%s: HPD is already ON\n", __func__);
	} else {
		rc = hdmi_tx_enable_power(hdmi_ctrl, HDMI_TX_HPD_PM, true);
		if (rc) {
			DEV_ERR("%s: Failed to enable hpd power. rc=%d\n",
				__func__, rc);
			return rc;
		}

		dss_reg_dump(io->base, io->len, "HDMI-INIT: ", REG_DUMP);

		hdmi_tx_set_mode(hdmi_ctrl, false);
		hdmi_tx_phy_reset(hdmi_ctrl);
		hdmi_tx_set_mode(hdmi_ctrl, true);

		DSS_REG_W(io, HDMI_USEC_REFTIMER, 0x0001001B);

		mdss_enable_irq(&hdmi_tx_hw);

		hdmi_ctrl->hpd_initialized = true;

		/* set timeout to 4.1ms (max) for hardware debounce */
		reg_val = DSS_REG_R(io, HDMI_HPD_CTRL) | 0x1FFF;

		/* Turn on HPD HW circuit */
		DSS_REG_W(io, HDMI_HPD_CTRL, reg_val | BIT(28));

		hdmi_tx_hpd_polarity_setup(hdmi_ctrl, HPD_CONNECT_POLARITY);
	}

	return rc;
} /* hdmi_tx_hpd_on */

static int hdmi_tx_sysfs_enable_hpd(struct hdmi_tx_ctrl *hdmi_ctrl, int on)
{
	int rc = 0;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	DEV_INFO("%s: %d\n", __func__, on);
	if (on) {
		rc = hdmi_tx_hpd_on(hdmi_ctrl);
	} else {
		/* If power down is already underway, wait for it to finish */
		flush_work_sync(&hdmi_ctrl->power_off_work);

		if (!hdmi_ctrl->panel_power_on) {
			hdmi_tx_hpd_off(hdmi_ctrl);
		} else {
			hdmi_ctrl->hpd_off_pending = true;

			switch_set_state(&hdmi_ctrl->sdev, 0);
			DEV_DBG("%s: Hdmi state switch to %d\n", __func__,
				hdmi_ctrl->sdev.state);
			DEV_DBG("HDMI HPD: sent fake OFFLINE event\n");
			kobject_uevent(hdmi_ctrl->kobj, KOBJ_OFFLINE);
		}
	}

	return rc;
} /* hdmi_tx_sysfs_enable_hpd */

static irqreturn_t hdmi_tx_isr(int irq, void *data)
{
	struct dss_io_data *io = NULL;
	struct hdmi_tx_ctrl *hdmi_ctrl = (struct hdmi_tx_ctrl *)data;

	if (!hdmi_ctrl || !hdmi_ctrl->hpd_initialized) {
		DEV_WARN("%s: invalid input data, ISR ignored\n", __func__);
		return IRQ_HANDLED;
	}

	io = &hdmi_ctrl->pdata.io[HDMI_TX_CORE_IO];
	if (!io->base) {
		DEV_WARN("%s: core io not initialized, ISR ignored\n",
			__func__);
		return IRQ_HANDLED;
	}

	if (DSS_REG_R(io, HDMI_HPD_INT_STATUS) & BIT(0)) {
		/*
		 * Ack the current hpd interrupt and stop listening to
		 * new hpd interrupt.
		 */
		DSS_REG_W(io, HDMI_HPD_INT_CTRL, BIT(0));
		queue_work(hdmi_ctrl->workq, &hdmi_ctrl->hpd_int_work);
	}

	if (hdmi_ddc_isr(&hdmi_ctrl->ddc_ctrl) < 0)
		DEV_ERR("%s: hdmi_ddc_isr failed\n", __func__);

	return IRQ_HANDLED;
} /* hdmi_tx_isr */

static void hdmi_tx_dev_deinit(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	if (hdmi_ctrl->feature_data[HDMI_TX_FEAT_EDID])
		hdmi_edid_deinit(hdmi_ctrl->feature_data[HDMI_TX_FEAT_EDID]);

	switch_dev_unregister(&hdmi_ctrl->audio_sdev);
	switch_dev_unregister(&hdmi_ctrl->sdev);
	if (hdmi_ctrl->workq)
		destroy_workqueue(hdmi_ctrl->workq);
	mutex_destroy(&hdmi_ctrl->mutex);

	hdmi_tx_hw.ptr = NULL;
} /* hdmi_tx_dev_deinit */

static int hdmi_tx_dev_init(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int rc = 0;
	struct hdmi_tx_platform_data *pdata = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	pdata = &hdmi_ctrl->pdata;

	rc = hdmi_tx_check_capability(&pdata->io[HDMI_TX_QFPROM_IO]);
	if (rc) {
		DEV_ERR("%s: no HDMI device\n", __func__);
		goto fail_no_hdmi;
	}

	/* irq enable/disable will be handled in hpd on/off */
	hdmi_tx_hw.ptr = (void *)hdmi_ctrl;

	hdmi_tx_setup_video_mode_lut();
	mutex_init(&hdmi_ctrl->mutex);
	hdmi_ctrl->workq = create_workqueue("hdmi_tx_workq");
	if (!hdmi_ctrl->workq) {
		DEV_ERR("%s: hdmi_tx_workq creation failed.\n", __func__);
		rc = -EPERM;
		goto fail_create_workq;
	}

	hdmi_ctrl->ddc_ctrl.io = &pdata->io[HDMI_TX_CORE_IO];
	init_completion(&hdmi_ctrl->ddc_ctrl.ddc_sw_done);

	hdmi_ctrl->panel_power_on = false;
	hdmi_ctrl->panel_suspend = false;

	hdmi_ctrl->hpd_state = false;
	hdmi_ctrl->hpd_initialized = false;
	hdmi_ctrl->hpd_off_pending = false;
	init_completion(&hdmi_ctrl->hpd_done);
	INIT_WORK(&hdmi_ctrl->hpd_int_work, hdmi_tx_hpd_int_work);

	INIT_WORK(&hdmi_ctrl->power_off_work, hdmi_tx_power_off_work);

	hdmi_ctrl->audio_sample_rate = HDMI_SAMPLE_RATE_48KHZ;

	hdmi_ctrl->sdev.name = "hdmi";
	if (switch_dev_register(&hdmi_ctrl->sdev) < 0) {
		DEV_ERR("%s: Hdmi switch registration failed\n", __func__);
		rc = -ENODEV;
		goto fail_create_workq;
	}

	hdmi_ctrl->audio_sdev.name = "hdmi_audio";
	if (switch_dev_register(&hdmi_ctrl->audio_sdev) < 0) {
		DEV_ERR("%s: hdmi_audio switch registration failed\n",
			__func__);
		rc = -ENODEV;
		goto fail_audio_switch_dev;
	}

	return 0;

fail_audio_switch_dev:
	switch_dev_unregister(&hdmi_ctrl->sdev);
fail_create_workq:
	if (hdmi_ctrl->workq)
		destroy_workqueue(hdmi_ctrl->workq);
	mutex_destroy(&hdmi_ctrl->mutex);
fail_no_hdmi:
	return rc;
} /* hdmi_tx_dev_init */

static int hdmi_tx_panel_event_handler(struct mdss_panel_data *panel_data,
	int event, void *arg)
{
	int rc = 0;
	struct hdmi_tx_ctrl *hdmi_ctrl =
		hdmi_tx_get_drvdata_from_panel_data(panel_data);

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	DEV_DBG("%s: event = %d suspend=%d, hpd_feature=%d\n", __func__,
		event, hdmi_ctrl->panel_suspend, hdmi_ctrl->hpd_feature_on);

	switch (event) {
	case MDSS_EVENT_RESUME:
		if (hdmi_ctrl->hpd_feature_on) {
			INIT_COMPLETION(hdmi_ctrl->hpd_done);

			rc = hdmi_tx_hpd_on(hdmi_ctrl);
			if (rc)
				DEV_ERR("%s: hdmi_tx_hpd_on failed. rc=%d\n",
					__func__, rc);
		}
		break;

	case MDSS_EVENT_RESET:
		if (hdmi_ctrl->panel_suspend) {
			u32 timeout;
			hdmi_ctrl->panel_suspend = false;

			timeout = wait_for_completion_interruptible_timeout(
				&hdmi_ctrl->hpd_done, HZ/10);
			if (!timeout & !hdmi_ctrl->hpd_state) {
				DEV_INFO("%s: cable removed during suspend\n",
					__func__);

				kobject_uevent(hdmi_ctrl->kobj, KOBJ_OFFLINE);
				switch_set_state(&hdmi_ctrl->sdev, 0);

				rc = -EPERM;
			} else {
				DEV_DBG("%s: cable present after resume\n",
					__func__);
			}
		}
		break;

	case MDSS_EVENT_UNBLANK:
		rc = hdmi_tx_power_on(panel_data);
		if (rc)
			DEV_ERR("%s: hdmi_tx_power_on failed. rc=%d\n",
				__func__, rc);
		break;

	case MDSS_EVENT_TIMEGEN_ON:
		break;

	case MDSS_EVENT_SUSPEND:
		if (!hdmi_ctrl->panel_power_on) {
			if (hdmi_ctrl->hpd_feature_on)
				hdmi_tx_hpd_off(hdmi_ctrl);
			else
				DEV_ERR("%s: invalid state\n", __func__);

			hdmi_ctrl->panel_suspend = false;
		} else {
			hdmi_ctrl->hpd_off_pending = true;
			hdmi_ctrl->panel_suspend = true;
		}
		break;

	case MDSS_EVENT_BLANK:
		if (hdmi_ctrl->panel_power_on) {
			rc = hdmi_tx_power_off(panel_data);
			if (rc)
				DEV_ERR("%s: hdmi_tx_power_off failed.rc=%d\n",
					__func__, rc);

		} else {
			DEV_DBG("%s: hdmi is already powered off\n", __func__);
		}
		break;

	case MDSS_EVENT_TIMEGEN_OFF:
		/* If a power off is already underway, wait for it to finish */
		if (hdmi_ctrl->panel_suspend)
			flush_work_sync(&hdmi_ctrl->power_off_work);
		break;
	}

	return rc;
} /* hdmi_tx_panel_event_handler */

static int hdmi_tx_register_panel(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int rc = 0;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	hdmi_ctrl->panel_data.event_handler = hdmi_tx_panel_event_handler;

	hdmi_ctrl->video_resolution = DEFAULT_VIDEO_RESOLUTION;
	rc = hdmi_tx_init_panel_info(hdmi_ctrl->video_resolution,
		&hdmi_ctrl->panel_data.panel_info);
	if (rc) {
		DEV_ERR("%s: hdmi_init_panel_info failed\n", __func__);
		return rc;
	}

	rc = mdss_register_panel(&hdmi_ctrl->panel_data);
	if (rc) {
		DEV_ERR("%s: FAILED: to register HDMI panel\n", __func__);
		return rc;
	}

	return rc;
} /* hdmi_tx_register_panel */

static void hdmi_tx_deinit_resource(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int i;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	/* VREG & CLK */
	for (i = HDMI_TX_MAX_PM - 1; i >= 0; i--) {
		if (hdmi_tx_config_power(hdmi_ctrl, i, 0))
			DEV_ERR("%s: '%s' power deconfig fail\n",
				__func__, hdmi_tx_pm_name(i));
	}

	/* IO */
	for (i = HDMI_TX_MAX_IO - 1; i >= 0; i--)
		msm_dss_iounmap(&hdmi_ctrl->pdata.io[i]);
} /* hdmi_tx_deinit_resource */

static int hdmi_tx_init_resource(struct hdmi_tx_ctrl *hdmi_ctrl)
{
	int i, rc = 0;
	struct hdmi_tx_platform_data *pdata = NULL;

	if (!hdmi_ctrl) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	pdata = &hdmi_ctrl->pdata;

	/* IO */
	for (i = 0; i < HDMI_TX_MAX_IO; i++) {
		rc = msm_dss_ioremap_byname(hdmi_ctrl->pdev, &pdata->io[i],
			hdmi_tx_io_name(i));
		if (rc) {
			DEV_ERR("%s: '%s' remap failed\n", __func__,
				hdmi_tx_io_name(i));
			goto error;
		}
		DEV_INFO("%s: '%s': start = 0x%x, len=0x%x\n", __func__,
			hdmi_tx_io_name(i), (u32)pdata->io[i].base,
			pdata->io[i].len);
	}

	/* VREG & CLK */
	for (i = 0; i < HDMI_TX_MAX_PM; i++) {
		rc = hdmi_tx_config_power(hdmi_ctrl, i, 1);
		if (rc) {
			DEV_ERR("%s: '%s' power config failed.rc=%d\n",
				__func__, hdmi_tx_pm_name(i), rc);
			goto error;
		}
	}

	return rc;

error:
	hdmi_tx_deinit_resource(hdmi_ctrl);
	return rc;
} /* hdmi_tx_init_resource */

static void hdmi_tx_put_dt_clk_data(struct device *dev,
	struct dss_module_power *module_power)
{
	if (!module_power) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	if (module_power->clk_config) {
		devm_kfree(dev, module_power->clk_config);
		module_power->clk_config = NULL;
	}
	module_power->num_clk = 0;
} /* hdmi_tx_put_dt_clk_data */

/* todo: once clk are moved to device tree then change this implementation */
static int hdmi_tx_get_dt_clk_data(struct device *dev,
	struct dss_module_power *mp, u32 module_type)
{
	int rc = 0;

	if (!dev || !mp) {
		DEV_ERR("%s: invalid input\n", __func__);
		rc = -EINVAL;
		goto error;
	}

	DEV_DBG("%s: module: '%s'\n", __func__, hdmi_tx_pm_name(module_type));

	switch (module_type) {
	case HDMI_TX_HPD_PM:
		mp->num_clk = 2;
		mp->clk_config = devm_kzalloc(dev, sizeof(struct dss_clk) *
			mp->num_clk, GFP_KERNEL);
		if (!mp->clk_config) {
			DEV_ERR("%s: can't alloc '%s' clk mem\n", __func__,
				hdmi_tx_pm_name(module_type));
			goto error;
		}

		snprintf(mp->clk_config[0].clk_name, 32, "%s", "iface_clk");
		mp->clk_config[0].type = DSS_CLK_AHB;
		mp->clk_config[0].rate = 0;

		snprintf(mp->clk_config[1].clk_name, 32, "%s", "core_clk");
		mp->clk_config[1].type = DSS_CLK_OTHER;
		mp->clk_config[1].rate = 19200000;
		break;

	case HDMI_TX_CORE_PM:
		mp->num_clk = 2;
		mp->clk_config = devm_kzalloc(dev, sizeof(struct dss_clk) *
			mp->num_clk, GFP_KERNEL);
		if (!mp->clk_config) {
			DEV_ERR("%s: can't alloc '%s' clk mem\n", __func__,
				hdmi_tx_pm_name(module_type));
			goto error;
		}

		snprintf(mp->clk_config[0].clk_name, 32, "%s", "extp_clk");
		mp->clk_config[0].type = DSS_CLK_PCLK;
		/* This rate will be overwritten when core is powered on */
		mp->clk_config[0].rate = 148500000;

		snprintf(mp->clk_config[1].clk_name, 32, "%s", "alt_iface_clk");
		mp->clk_config[1].type = DSS_CLK_AHB;
		mp->clk_config[1].rate = 0;
		break;

	case HDMI_TX_CEC_PM:
		mp->num_clk = 0;
		DEV_DBG("%s: no clk\n", __func__);
		break;

	default:
		DEV_ERR("%s: invalid module type=%d\n", __func__,
			module_type);
		return -EINVAL;
	}

	return rc;

error:
	if (mp->clk_config) {
		devm_kfree(dev, mp->clk_config);
		mp->clk_config = NULL;
	}
	mp->num_clk = 0;

	return rc;
} /* hdmi_tx_get_dt_clk_data */

static void hdmi_tx_put_dt_vreg_data(struct device *dev,
	struct dss_module_power *module_power)
{
	if (!module_power) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	if (module_power->vreg_config) {
		devm_kfree(dev, module_power->vreg_config);
		module_power->vreg_config = NULL;
	}
	module_power->num_vreg = 0;
} /* hdmi_tx_put_dt_vreg_data */

static int hdmi_tx_get_dt_vreg_data(struct device *dev,
	struct dss_module_power *mp, u32 module_type)
{
	int i, j, rc = 0;
	int dt_vreg_total = 0, mod_vreg_total = 0;
	u32 ndx_mask = 0;
	u32 *val_array = NULL;
	const char *mod_name = NULL;
	struct device_node *of_node = NULL;
	char prop_name[32];

	if (!dev || !mp) {
		DEV_ERR("%s: invalid input\n", __func__);
		rc = -EINVAL;
		goto error;
	}

	switch (module_type) {
	case HDMI_TX_HPD_PM:
		mod_name = "hpd";
		break;
	case HDMI_TX_CORE_PM:
		mod_name = "core";
		break;
	case HDMI_TX_CEC_PM:
		mod_name = "cec";
		break;
	default:
		DEV_ERR("%s: invalid module type=%d\n", __func__,
			module_type);
		return -EINVAL;
	}

	DEV_DBG("%s: module: '%s'\n", __func__, hdmi_tx_pm_name(module_type));

	of_node = dev->of_node;

	memset(prop_name, 0, sizeof(prop_name));
	snprintf(prop_name, 32, "%s-%s", COMPATIBLE_NAME, "supply-names");
	dt_vreg_total = of_property_count_strings(of_node, prop_name);
	if (dt_vreg_total < 0) {
		DEV_ERR("%s: vreg not found. rc=%d\n", __func__,
			dt_vreg_total);
		rc = dt_vreg_total;
		goto error;
	}

	/* count how many vreg for particular hdmi module */
	for (i = 0; i < dt_vreg_total; i++) {
		const char *st = NULL;
		memset(prop_name, 0, sizeof(prop_name));
		snprintf(prop_name, 32, "%s-%s", COMPATIBLE_NAME,
			"supply-names");
		rc = of_property_read_string_index(of_node,
			prop_name, i, &st);
		if (rc) {
			DEV_ERR("%s: error reading name. i=%d, rc=%d\n",
				__func__, i, rc);
			goto error;
		}

		if (strnstr(st, mod_name, strlen(st))) {
			ndx_mask |= BIT(i);
			mod_vreg_total++;
		}
	}

	if (mod_vreg_total > 0) {
		mp->num_vreg = mod_vreg_total;
		mp->vreg_config = devm_kzalloc(dev, sizeof(struct dss_vreg) *
			mod_vreg_total, GFP_KERNEL);
		if (!mp->vreg_config) {
			DEV_ERR("%s: can't alloc '%s' vreg mem\n", __func__,
				hdmi_tx_pm_name(module_type));
			goto error;
		}
	} else {
		DEV_DBG("%s: no vreg\n", __func__);
		return 0;
	}

	val_array = devm_kzalloc(dev, sizeof(u32) * dt_vreg_total, GFP_KERNEL);
	if (!val_array) {
		DEV_ERR("%s: can't allocate vreg scratch mem\n", __func__);
		rc = -ENOMEM;
		goto error;
	}

	for (i = 0, j = 0; (i < dt_vreg_total) && (j < mod_vreg_total); i++) {
		const char *st = NULL;

		if (!(ndx_mask & BIT(0))) {
			ndx_mask >>= 1;
			continue;
		}

		/* vreg-name */
		memset(prop_name, 0, sizeof(prop_name));
		snprintf(prop_name, 32, "%s-%s", COMPATIBLE_NAME,
			"supply-names");
		rc = of_property_read_string_index(of_node,
			prop_name, i, &st);
		if (rc) {
			DEV_ERR("%s: error reading name. i=%d, rc=%d\n",
				__func__, i, rc);
			goto error;
		}
		snprintf(mp->vreg_config[j].vreg_name, 32, "%s", st);

		/* vreg-type */
		memset(prop_name, 0, sizeof(prop_name));
		snprintf(prop_name, 32, "%s-%s", COMPATIBLE_NAME,
			"supply-type");
		memset(val_array, 0, sizeof(u32) * dt_vreg_total);
		rc = of_property_read_u32_array(of_node,
			prop_name, val_array, dt_vreg_total);
		if (rc) {
			DEV_ERR("%s: error read '%s' vreg type. rc=%d\n",
				__func__, hdmi_tx_pm_name(module_type), rc);
			goto error;
		}
		mp->vreg_config[j].type = val_array[i];

		/* vreg-min-voltage */
		memset(prop_name, 0, sizeof(prop_name));
		snprintf(prop_name, 32, "%s-%s", COMPATIBLE_NAME,
			"min-voltage-level");
		memset(val_array, 0, sizeof(u32) * dt_vreg_total);
		rc = of_property_read_u32_array(of_node,
			prop_name, val_array,
			dt_vreg_total);
		if (rc) {
			DEV_ERR("%s: error read '%s' min volt. rc=%d\n",
				__func__, hdmi_tx_pm_name(module_type), rc);
			goto error;
		}
		mp->vreg_config[j].min_voltage = val_array[i];

		/* vreg-max-voltage */
		memset(prop_name, 0, sizeof(prop_name));
		snprintf(prop_name, 32, "%s-%s", COMPATIBLE_NAME,
			"max-voltage-level");
		memset(val_array, 0, sizeof(u32) * dt_vreg_total);
		rc = of_property_read_u32_array(of_node,
			prop_name, val_array,
			dt_vreg_total);
		if (rc) {
			DEV_ERR("%s: error read '%s' max volt. rc=%d\n",
				__func__, hdmi_tx_pm_name(module_type), rc);
			goto error;
		}
		mp->vreg_config[j].max_voltage = val_array[i];

		/* vreg-op-mode */
		memset(prop_name, 0, sizeof(prop_name));
		snprintf(prop_name, 32, "%s-%s", COMPATIBLE_NAME,
			"op-mode");
		memset(val_array, 0, sizeof(u32) * dt_vreg_total);
		rc = of_property_read_u32_array(of_node,
			prop_name, val_array,
			dt_vreg_total);
		if (rc) {
			DEV_ERR("%s: error read '%s' min volt. rc=%d\n",
				__func__, hdmi_tx_pm_name(module_type), rc);
			goto error;
		}
		mp->vreg_config[j].optimum_voltage = val_array[i];

		DEV_DBG("%s: %s type=%d, min=%d, max=%d, op=%d\n",
			__func__, mp->vreg_config[j].vreg_name,
			mp->vreg_config[j].type,
			mp->vreg_config[j].min_voltage,
			mp->vreg_config[j].max_voltage,
			mp->vreg_config[j].optimum_voltage);

		ndx_mask >>= 1;
		j++;
	}

	devm_kfree(dev, val_array);

	return rc;

error:
	if (mp->vreg_config) {
		devm_kfree(dev, mp->vreg_config);
		mp->vreg_config = NULL;
	}
	mp->num_vreg = 0;

	if (val_array)
		devm_kfree(dev, val_array);
	return rc;
} /* hdmi_tx_get_dt_vreg_data */

static void hdmi_tx_put_dt_gpio_data(struct device *dev,
	struct dss_module_power *module_power)
{
	if (!module_power) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	if (module_power->gpio_config) {
		devm_kfree(dev, module_power->gpio_config);
		module_power->gpio_config = NULL;
	}
	module_power->num_gpio = 0;
} /* hdmi_tx_put_dt_gpio_data */

static int hdmi_tx_get_dt_gpio_data(struct device *dev,
	struct dss_module_power *mp, u32 module_type)
{
	int i, j;
	int mp_gpio_cnt = 0, gpio_list_size = 0;
	struct dss_gpio *gpio_list = NULL;
	struct device_node *of_node = NULL;

	DEV_DBG("%s: module: '%s'\n", __func__, hdmi_tx_pm_name(module_type));

	if (!dev || !mp) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	of_node = dev->of_node;

	switch (module_type) {
	case HDMI_TX_HPD_PM:
		gpio_list_size = ARRAY_SIZE(hpd_gpio_config);
		gpio_list = hpd_gpio_config;
		break;
	case HDMI_TX_CORE_PM:
		gpio_list_size = ARRAY_SIZE(core_gpio_config);
		gpio_list = core_gpio_config;
		break;
	case HDMI_TX_CEC_PM:
		gpio_list_size = ARRAY_SIZE(cec_gpio_config);
		gpio_list = cec_gpio_config;
		break;
	default:
		DEV_ERR("%s: invalid module type=%d\n", __func__,
			module_type);
		return -EINVAL;
	}

	for (i = 0; i < gpio_list_size; i++)
		if (of_find_property(of_node, gpio_list[i].gpio_name, NULL))
			mp_gpio_cnt++;

	if (!mp_gpio_cnt) {
		DEV_DBG("%s: no gpio\n", __func__);
		return 0;
	}

	DEV_DBG("%s: mp_gpio_cnt = %d\n", __func__, mp_gpio_cnt);
	mp->num_gpio = mp_gpio_cnt;

	mp->gpio_config = devm_kzalloc(dev, sizeof(struct dss_gpio) *
		mp_gpio_cnt, GFP_KERNEL);
	if (!mp->gpio_config) {
		DEV_ERR("%s: can't alloc '%s' gpio mem\n", __func__,
			hdmi_tx_pm_name(module_type));

		mp->num_gpio = 0;
		return -ENOMEM;
	}

	for (i = 0, j = 0; i < gpio_list_size; i++) {
		int gpio = of_get_named_gpio(of_node,
			gpio_list[i].gpio_name, 0);
		if (gpio < 0) {
			DEV_DBG("%s: no gpio named %s\n", __func__,
				gpio_list[i].gpio_name);
			continue;
		}
		memcpy(&mp->gpio_config[j], &gpio_list[i],
			sizeof(struct dss_gpio));

		mp->gpio_config[j].gpio = (unsigned)gpio;

		DEV_DBG("%s: gpio num=%d, name=%s, value=%d\n",
			__func__, mp->gpio_config[j].gpio,
			mp->gpio_config[j].gpio_name,
			mp->gpio_config[j].value);
		j++;
	}

	return 0;
} /* hdmi_tx_get_dt_gpio_data */

static void hdmi_tx_put_dt_data(struct device *dev,
	struct hdmi_tx_platform_data *pdata)
{
	int i;
	if (!dev || !pdata) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	for (i = HDMI_TX_MAX_PM - 1; i >= 0; i--)
		hdmi_tx_put_dt_clk_data(dev, &pdata->power_data[i]);

	for (i = HDMI_TX_MAX_PM - 1; i >= 0; i--)
		hdmi_tx_put_dt_vreg_data(dev, &pdata->power_data[i]);

	for (i = HDMI_TX_MAX_PM - 1; i >= 0; i--)
		hdmi_tx_put_dt_gpio_data(dev, &pdata->power_data[i]);
} /* hdmi_tx_put_dt_data */

static int hdmi_tx_get_dt_data(struct platform_device *pdev,
	struct hdmi_tx_platform_data *pdata)
{
	int i, rc = 0;
	struct device_node *of_node = NULL;

	if (!pdev || !pdata) {
		DEV_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	of_node = pdev->dev.of_node;

	rc = of_property_read_u32(of_node, "cell-index", &pdev->id);
	if (rc) {
		DEV_ERR("%s: dev id from dt not found.rc=%d\n",
			__func__, rc);
		goto error;
	}
	DEV_DBG("%s: id=%d\n", __func__, pdev->id);

	/* GPIO */
	for (i = 0; i < HDMI_TX_MAX_PM; i++) {
		rc = hdmi_tx_get_dt_gpio_data(&pdev->dev,
			&pdata->power_data[i], i);
		if (rc) {
			DEV_ERR("%s: '%s' get_dt_gpio_data failed.rc=%d\n",
				__func__, hdmi_tx_pm_name(i), rc);
			goto error;
		}
	}

	/* VREG */
	for (i = 0; i < HDMI_TX_MAX_PM; i++) {
		rc = hdmi_tx_get_dt_vreg_data(&pdev->dev,
			&pdata->power_data[i], i);
		if (rc) {
			DEV_ERR("%s: '%s' get_dt_vreg_data failed.rc=%d\n",
				__func__, hdmi_tx_pm_name(i), rc);
			goto error;
		}
	}

	/* CLK */
	for (i = 0; i < HDMI_TX_MAX_PM; i++) {
		rc = hdmi_tx_get_dt_clk_data(&pdev->dev,
			&pdata->power_data[i], i);
		if (rc) {
			DEV_ERR("%s: '%s' get_dt_clk_data failed.rc=%d\n",
				__func__, hdmi_tx_pm_name(i), rc);
			goto error;
		}
	}

	return rc;

error:
	hdmi_tx_put_dt_data(&pdev->dev, pdata);
	return rc;
} /* hdmi_tx_get_dt_data */

static int __devinit hdmi_tx_probe(struct platform_device *pdev)
{
	int rc = 0;
	struct device_node *of_node = pdev->dev.of_node;
	struct hdmi_tx_ctrl *hdmi_ctrl = NULL;

	if (!of_node) {
		DEV_ERR("%s: FAILED: of_node not found\n", __func__);
		rc = -ENODEV;
		return rc;
	}

	hdmi_ctrl = devm_kzalloc(&pdev->dev, sizeof(*hdmi_ctrl), GFP_KERNEL);
	if (!hdmi_ctrl) {
		DEV_ERR("%s: FAILED: cannot alloc hdmi tx ctrl\n", __func__);
		rc = -ENOMEM;
		goto failed_no_mem;
	}

	platform_set_drvdata(pdev, hdmi_ctrl);
	hdmi_ctrl->pdev = pdev;

	rc = hdmi_tx_get_dt_data(pdev, &hdmi_ctrl->pdata);
	if (rc) {
		DEV_ERR("%s: FAILED: parsing device tree data. rc=%d\n",
			__func__, rc);
		goto failed_dt_data;
	}

	rc = hdmi_tx_init_resource(hdmi_ctrl);
	if (rc) {
		DEV_ERR("%s: FAILED: resource init. rc=%d\n",
			__func__, rc);
		goto failed_res_init;
	}

	rc = hdmi_tx_dev_init(hdmi_ctrl);
	if (rc) {
		DEV_ERR("%s: FAILED: hdmi_tx_dev_init. rc=%d\n", __func__, rc);
		goto failed_dev_init;
	}

	rc = hdmi_tx_register_panel(hdmi_ctrl);
	if (rc) {
		DEV_ERR("%s: FAILED: register_panel. rc=%d\n", __func__, rc);
		goto failed_reg_panel;
	}

	rc = hdmi_tx_sysfs_create(hdmi_ctrl);
	if (rc) {
		DEV_ERR("%s: hdmi_tx_sysfs_create failed.rc=%d\n",
			__func__, rc);
		goto failed_reg_panel;
	}

	rc = hdmi_tx_init_features(hdmi_ctrl);
	if (rc) {
		DEV_ERR("%s: init_features failed.rc=%d\n", __func__, rc);
		goto failed_init_features;
	}

	return rc;

failed_init_features:
	hdmi_tx_sysfs_remove(hdmi_ctrl);
failed_reg_panel:
	hdmi_tx_dev_deinit(hdmi_ctrl);
failed_dev_init:
	hdmi_tx_deinit_resource(hdmi_ctrl);
failed_res_init:
	hdmi_tx_put_dt_data(&pdev->dev, &hdmi_ctrl->pdata);
failed_dt_data:
	devm_kfree(&pdev->dev, hdmi_ctrl);
failed_no_mem:
	return rc;
} /* hdmi_tx_probe */

static int __devexit hdmi_tx_remove(struct platform_device *pdev)
{
	struct hdmi_tx_ctrl *hdmi_ctrl = platform_get_drvdata(pdev);
	if (!hdmi_ctrl) {
		DEV_ERR("%s: no driver data\n", __func__);
		return -ENODEV;
	}

	hdmi_tx_sysfs_remove(hdmi_ctrl);
	hdmi_tx_dev_deinit(hdmi_ctrl);
	hdmi_tx_deinit_resource(hdmi_ctrl);
	hdmi_tx_put_dt_data(&pdev->dev, &hdmi_ctrl->pdata);
	devm_kfree(&hdmi_ctrl->pdev->dev, hdmi_ctrl);

	return 0;
} /* hdmi_tx_remove */

static const struct of_device_id hdmi_tx_dt_match[] = {
	{.compatible = COMPATIBLE_NAME,},
};
MODULE_DEVICE_TABLE(of, hdmi_tx_dt_match);

static struct platform_driver this_driver = {
	.probe = hdmi_tx_probe,
	.remove = hdmi_tx_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = hdmi_tx_dt_match,
	},
};

static int __init hdmi_tx_drv_init(void)
{
	int rc;

	rc = platform_driver_register(&this_driver);
	if (rc)
		DEV_ERR("%s: FAILED: rc=%d\n", __func__, rc);

	return rc;
} /* hdmi_tx_drv_init */

static void __exit hdmi_tx_drv_exit(void)
{
	platform_driver_unregister(&this_driver);
} /* hdmi_tx_drv_exit */

module_init(hdmi_tx_drv_init);
module_exit(hdmi_tx_drv_exit);

MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.3");
MODULE_DESCRIPTION("HDMI MSM TX driver");