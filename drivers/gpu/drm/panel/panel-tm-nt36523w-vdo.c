// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/backlight.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_modes.h>
#include <linux/delay.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"
#endif

#ifdef CONFIG_MTK_ROUND_CORNER_SUPPORT
#include "../mediatek/mediatek_v2/mtk_corner_pattern/mtk_data_hw_roundedpattern.h"
#endif

#if IS_ENABLED(CONFIG_OEM_DEVINFO)
#include <dev_info.h>
#endif

int hbm;
bool is_suspend;
//static unsigned char hbm_buf[16] = {0};
struct tianma *ptx;

struct tianma {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *vddio_gpio;
	struct gpio_desc *bias_pos, *bias_neg;
	//struct gpio_desc *bl_en_gpio;

	bool prepared;
	bool enabled;

	int error;
};

#define tianma_dcs_write_seq(ctx, seq...) \
({\
	const u8 d[] = { seq };\
	BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64, "DCS sequence too big for stack");\
	tianma_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

#define tianma_dcs_write_seq_static(ctx, seq...) \
({\
	static const u8 d[] = { seq };\
	tianma_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

static inline struct tianma *panel_to_lcm(struct drm_panel *panel)
{
	return container_of(panel, struct tianma, panel);
}

static void tianma_dcs_write(struct tianma *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;
	char *addr;

	if (ctx->error < 0)
		return;

	addr = (char *)data;
	if ((int)*addr < 0xB0)
		ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	else
		ret = mipi_dsi_generic_write(dsi, data, len);
	if (ret < 0) {
		dev_info(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}

#ifdef PANEL_SUPPORT_READBACK
static int tianma_dcs_read(struct tianma *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return 0;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_info(ctx->dev, "error %d reading dcs seq:(%#x)\n", ret, cmd);
		ctx->error = ret;
	}

	return ret;
}

static void tianma_panel_get_data(struct tianma *ctx)
{
	u8 buffer[3] = {0};
	static int ret;

	if (ret == 0) {
		ret = tianma_dcs_read(ctx,  0x0A, buffer, 1);
		dev_info(ctx->dev, "return %d data(0x%08x) to dsi engine\n",
			 ret, buffer[0] | (buffer[1] << 8));
	}
}
#endif

#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
static struct regulator *disp_bias_pos;
static struct regulator *disp_bias_neg;


static int tianma_panel_bias_regulator_init(void)
{
	static int regulator_inited;
	int ret = 0;

	if (regulator_inited)
		return ret;

	/* please only get regulator once in a driver */
	disp_bias_pos = regulator_get(NULL, "dsv_pos");
	if (IS_ERR(disp_bias_pos)) { /* handle return value */
		ret = PTR_ERR(disp_bias_pos);
		pr_info("get dsv_pos fail, error: %d\n", ret);
		return ret;
	}

	disp_bias_neg = regulator_get(NULL, "dsv_neg");
	if (IS_ERR(disp_bias_neg)) { /* handle return value */
		ret = PTR_ERR(disp_bias_neg);
		pr_info("get dsv_neg fail, error: %d\n", ret);
		return ret;
	}

	regulator_inited = 1;
	return ret; /* must be 0 */

}

static int tianma_panel_bias_enable(void)
{
	int ret = 0;
	int retval = 0;

	tianma_panel_bias_regulator_init();

	/* set voltage with min & max*/
	ret = regulator_set_voltage(disp_bias_pos, 5400000, 5400000);
	if (ret < 0)
		pr_info("set voltage disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_set_voltage(disp_bias_neg, 5400000, 5400000);
	if (ret < 0)
		pr_info("set voltage disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	/* enable regulator */
	ret = regulator_enable(disp_bias_pos);
	if (ret < 0)
		pr_info("enable regulator disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_enable(disp_bias_neg);
	if (ret < 0)
		pr_info("enable regulator disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}

static int tianma_panel_bias_disable(void)
{
	int ret = 0;
	int retval = 0;

	tianma_panel_bias_regulator_init();

	ret = regulator_disable(disp_bias_neg);
	if (ret < 0)
		pr_info("disable regulator disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_disable(disp_bias_pos);
	if (ret < 0)
		pr_info("disable regulator disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}
#endif

static void tianma_panel_init(struct tianma *ctx)
{
	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_info(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return;
	}

	udelay(10 * 1000);
	gpiod_set_value(ctx->reset_gpio, 1);
	udelay(10 * 1000);
	gpiod_set_value(ctx->reset_gpio, 0);
	udelay(10 * 1000);
	gpiod_set_value(ctx->reset_gpio, 1);
	udelay(10 * 1000);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

#ifdef TINNO_LCM_OEM_CONFIG
	tianma_resume_by_ddi();
#endif

	tianma_dcs_write_seq_static(ctx, 0xFF, 0x20);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x05, 0x69);
	tianma_dcs_write_seq_static(ctx, 0x06, 0xC0);
	tianma_dcs_write_seq_static(ctx, 0x07, 0x37);
	tianma_dcs_write_seq_static(ctx, 0x08, 0x1E);
	tianma_dcs_write_seq_static(ctx, 0x0E, 0x55);
	tianma_dcs_write_seq_static(ctx, 0x0F, 0x37);
	tianma_dcs_write_seq_static(ctx, 0x89, 0x20);
	tianma_dcs_write_seq_static(ctx, 0x8A, 0x20);
	tianma_dcs_write_seq_static(ctx, 0x95, 0xD7);
	tianma_dcs_write_seq_static(ctx, 0x96, 0xD7);
	tianma_dcs_write_seq_static(ctx, 0x9D, 0x0F);
	tianma_dcs_write_seq_static(ctx, 0x9E, 0x0F);
	tianma_dcs_write_seq_static(ctx, 0x30, 0x10);
	tianma_dcs_write_seq_static(ctx, 0x58, 0x40);
	tianma_dcs_write_seq_static(ctx, 0x5A, 0x14);
	tianma_dcs_write_seq_static(ctx, 0x6D, 0x66);
	tianma_dcs_write_seq_static(ctx, 0x75, 0x91);
	tianma_dcs_write_seq_static(ctx, 0x77, 0xB3);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0x24);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x00, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x01, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x02, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x03, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x04, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x05, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x06, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x07, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x08, 0x27);
	tianma_dcs_write_seq_static(ctx, 0x09, 0x26);
	tianma_dcs_write_seq_static(ctx, 0x0A, 0x23);
	tianma_dcs_write_seq_static(ctx, 0x0B, 0x22);
	tianma_dcs_write_seq_static(ctx, 0x0C, 0x0F);
	tianma_dcs_write_seq_static(ctx, 0x0D, 0x0E);
	tianma_dcs_write_seq_static(ctx, 0x0E, 0x0D);
	tianma_dcs_write_seq_static(ctx, 0x0F, 0x0C);
	tianma_dcs_write_seq_static(ctx, 0x10, 0x28);
	tianma_dcs_write_seq_static(ctx, 0x11, 0x28);
	tianma_dcs_write_seq_static(ctx, 0x12, 0x05);
	tianma_dcs_write_seq_static(ctx, 0x13, 0x04);
	tianma_dcs_write_seq_static(ctx, 0x14, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x15, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x16, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x17, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x18, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x19, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x1A, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x1B, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x1C, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x1D, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x1E, 0x27);
	tianma_dcs_write_seq_static(ctx, 0x1F, 0x26);
	tianma_dcs_write_seq_static(ctx, 0x20, 0x23);
	tianma_dcs_write_seq_static(ctx, 0x21, 0x22);
	tianma_dcs_write_seq_static(ctx, 0x22, 0x0F);
	tianma_dcs_write_seq_static(ctx, 0x23, 0x0E);
	tianma_dcs_write_seq_static(ctx, 0x24, 0x0D);
	tianma_dcs_write_seq_static(ctx, 0x25, 0x0C);
	tianma_dcs_write_seq_static(ctx, 0x26, 0x28);
	tianma_dcs_write_seq_static(ctx, 0x27, 0x28);
	tianma_dcs_write_seq_static(ctx, 0x28, 0x05);
	tianma_dcs_write_seq_static(ctx, 0x29, 0x04);
	tianma_dcs_write_seq_static(ctx, 0x2A, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x2B, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x2F, 0x04);
	tianma_dcs_write_seq_static(ctx, 0x30, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x33, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x34, 0x04);
	tianma_dcs_write_seq_static(ctx, 0x37, 0x33);
	tianma_dcs_write_seq_static(ctx, 0x39, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x3A, 0x4E);
	tianma_dcs_write_seq_static(ctx, 0x3B, 0x1B);
	tianma_dcs_write_seq_static(ctx, 0x3D, 0x02);
	tianma_dcs_write_seq_static(ctx, 0x4D, 0x43);
	tianma_dcs_write_seq_static(ctx, 0x4E, 0x21);
	tianma_dcs_write_seq_static(ctx, 0x51, 0x12);
	tianma_dcs_write_seq_static(ctx, 0x52, 0x34);
	tianma_dcs_write_seq_static(ctx, 0x55, 0x84, 0x04);
	tianma_dcs_write_seq_static(ctx, 0x56, 0x04);
	tianma_dcs_write_seq_static(ctx, 0x58, 0x10);
	tianma_dcs_write_seq_static(ctx, 0x59, 0x10);
	tianma_dcs_write_seq_static(ctx, 0x5A, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x5B, 0x6A);
	tianma_dcs_write_seq_static(ctx, 0x5C, 0x80);
	tianma_dcs_write_seq_static(ctx, 0x5D, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x5E, 0x00, 0x04);
	tianma_dcs_write_seq_static(ctx, 0x5F, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x60, 0x96);
	tianma_dcs_write_seq_static(ctx, 0x61, 0xD0);
	tianma_dcs_write_seq_static(ctx, 0x65, 0x82);
	tianma_dcs_write_seq_static(ctx, 0x91, 0x44);
	tianma_dcs_write_seq_static(ctx, 0x92, 0x75);
	tianma_dcs_write_seq_static(ctx, 0x93, 0x1A);
	tianma_dcs_write_seq_static(ctx, 0x94, 0x5F);
	tianma_dcs_write_seq_static(ctx, 0x95, 0x80);
	tianma_dcs_write_seq_static(ctx, 0x96, 0x51);
	tianma_dcs_write_seq_static(ctx, 0xAA, 0x30);
	tianma_dcs_write_seq_static(ctx, 0xAB, 0x33);
	tianma_dcs_write_seq_static(ctx, 0xB5, 0x90);
	tianma_dcs_write_seq_static(ctx, 0xB6, 0x05,0x00,0x05,0x00,0x00,0x00,0x00,0x00,0x05,0x05,0x00,0x00);
	tianma_dcs_write_seq_static(ctx, 0xC2, 0xC6);
	tianma_dcs_write_seq_static(ctx, 0xC4, 0x1A);
	tianma_dcs_write_seq_static(ctx, 0xDA, 0x09);
	tianma_dcs_write_seq_static(ctx, 0xDE, 0x09);
	tianma_dcs_write_seq_static(ctx, 0xDB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0xDC, 0x75);
	tianma_dcs_write_seq_static(ctx, 0xDD, 0x22);
	tianma_dcs_write_seq_static(ctx, 0xDF, 0x01);
	tianma_dcs_write_seq_static(ctx, 0xE0, 0x75);
	tianma_dcs_write_seq_static(ctx, 0xE1, 0x01);
	tianma_dcs_write_seq_static(ctx, 0xE2, 0x75);
	tianma_dcs_write_seq_static(ctx, 0xE3, 0x01);
	tianma_dcs_write_seq_static(ctx, 0xE4, 0x75);
	tianma_dcs_write_seq_static(ctx, 0xE5, 0x01);
	tianma_dcs_write_seq_static(ctx, 0xE6, 0x75);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0x25);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x05, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x13, 0x02);
	tianma_dcs_write_seq_static(ctx, 0x14, 0xF3);
	tianma_dcs_write_seq_static(ctx, 0x19, 0x07);
	tianma_dcs_write_seq_static(ctx, 0x1B, 0x11);
	tianma_dcs_write_seq_static(ctx, 0x1F, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x20, 0x6A);
	tianma_dcs_write_seq_static(ctx, 0x26, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x27, 0x6A);
	tianma_dcs_write_seq_static(ctx, 0x33, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x34, 0x6A);
	tianma_dcs_write_seq_static(ctx, 0x3F, 0x80);
	tianma_dcs_write_seq_static(ctx, 0x40, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x48, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x49, 0x6A);
	tianma_dcs_write_seq_static(ctx, 0x5B, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x5C, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x61, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x62, 0x6A);
	tianma_dcs_write_seq_static(ctx, 0x68, 0x04);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0x26);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x00, 0xA1);
	tianma_dcs_write_seq_static(ctx, 0x02, 0x31);
	tianma_dcs_write_seq_static(ctx, 0x04, 0x28);
	tianma_dcs_write_seq_static(ctx, 0x06, 0x28);
	tianma_dcs_write_seq_static(ctx, 0x0A, 0xF2);
	tianma_dcs_write_seq_static(ctx, 0x0C, 0x13);
	tianma_dcs_write_seq_static(ctx, 0x0D, 0x0A);
	tianma_dcs_write_seq_static(ctx, 0x0F, 0x0A);
	tianma_dcs_write_seq_static(ctx, 0x11, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x12, 0x50);
	tianma_dcs_write_seq_static(ctx, 0x13, 0x51);
	tianma_dcs_write_seq_static(ctx, 0x14, 0x65);
	tianma_dcs_write_seq_static(ctx, 0x16, 0x10);
	tianma_dcs_write_seq_static(ctx, 0x19, 0x11);
	tianma_dcs_write_seq_static(ctx, 0x1A, 0x80);
	tianma_dcs_write_seq_static(ctx, 0x1B, 0x0F);
	tianma_dcs_write_seq_static(ctx, 0x1C, 0xE0);
	tianma_dcs_write_seq_static(ctx, 0x1D, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x1E, 0x75);
	tianma_dcs_write_seq_static(ctx, 0x1F, 0x75);
	tianma_dcs_write_seq_static(ctx, 0x20, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x21, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x24, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x25, 0x75);
	tianma_dcs_write_seq_static(ctx, 0x2A, 0x11);
	tianma_dcs_write_seq_static(ctx, 0x2B, 0x80);
	tianma_dcs_write_seq_static(ctx, 0x2F, 0x06);
	tianma_dcs_write_seq_static(ctx, 0x30, 0x75);
	tianma_dcs_write_seq_static(ctx, 0x31, 0x06);
	tianma_dcs_write_seq_static(ctx, 0x32, 0x85);
	tianma_dcs_write_seq_static(ctx, 0x33, 0x66);
	tianma_dcs_write_seq_static(ctx, 0x34, 0x66);
	tianma_dcs_write_seq_static(ctx, 0x35, 0x66);
	tianma_dcs_write_seq_static(ctx, 0x39, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x3A, 0x75);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0x27);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x56, 0x06);
	tianma_dcs_write_seq_static(ctx, 0x58, 0x80);
	tianma_dcs_write_seq_static(ctx, 0x59, 0x5E);
	tianma_dcs_write_seq_static(ctx, 0x5A, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x5B, 0x14);
	tianma_dcs_write_seq_static(ctx, 0x5C, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x5D, 0x07);
	tianma_dcs_write_seq_static(ctx, 0x5E, 0x20);
	tianma_dcs_write_seq_static(ctx, 0x5F, 0x10);
	tianma_dcs_write_seq_static(ctx, 0x60, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x61, 0x28);
	tianma_dcs_write_seq_static(ctx, 0x62, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x63, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x64, 0x22);
	tianma_dcs_write_seq_static(ctx, 0x65, 0x26);
	tianma_dcs_write_seq_static(ctx, 0x66, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x67, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x68, 0x23);
	tianma_dcs_write_seq_static(ctx, 0x75, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x76, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x77, 0x02);
	tianma_dcs_write_seq_static(ctx, 0xC0, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xD1, 0x24);
	tianma_dcs_write_seq_static(ctx, 0xD2, 0x53);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0x2A);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x22, 0x2F);
	tianma_dcs_write_seq_static(ctx, 0x23, 0x08);
	tianma_dcs_write_seq_static(ctx, 0x24, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x25, 0x75);
	tianma_dcs_write_seq_static(ctx, 0x27, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x28, 0x1A);
	tianma_dcs_write_seq_static(ctx, 0x29, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x2A, 0x1A);
	tianma_dcs_write_seq_static(ctx, 0x2B, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x2D, 0x1A);
	tianma_dcs_write_seq_static(ctx, 0x64, 0x26);
	tianma_dcs_write_seq_static(ctx, 0x65, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x66, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x6A, 0x26);
	tianma_dcs_write_seq_static(ctx, 0x6B, 0x43);
	tianma_dcs_write_seq_static(ctx, 0x6C, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x79, 0x26);
	tianma_dcs_write_seq_static(ctx, 0x7A, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x7B, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x7C, 0x26);
	tianma_dcs_write_seq_static(ctx, 0x7D, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x7E, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x7F, 0x2E);
	tianma_dcs_write_seq_static(ctx, 0x80, 0x03);
	tianma_dcs_write_seq_static(ctx, 0x81, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x82, 0x26);
	tianma_dcs_write_seq_static(ctx, 0x83, 0x30);
	tianma_dcs_write_seq_static(ctx, 0x84, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x85, 0xA6);
	tianma_dcs_write_seq_static(ctx, 0x86, 0x30);
	tianma_dcs_write_seq_static(ctx, 0x87, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x97, 0x3C);
	tianma_dcs_write_seq_static(ctx, 0x98, 0x02);
	tianma_dcs_write_seq_static(ctx, 0x99, 0x95);
	tianma_dcs_write_seq_static(ctx, 0x9A, 0x06);
	tianma_dcs_write_seq_static(ctx, 0x9B, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x9C, 0x0B);
	tianma_dcs_write_seq_static(ctx, 0x9D, 0x0A);
	tianma_dcs_write_seq_static(ctx, 0x9E, 0x90);
	tianma_dcs_write_seq_static(ctx, 0xA2, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xA3, 0xA0);
	tianma_dcs_write_seq_static(ctx, 0xA4, 0x28);
	tianma_dcs_write_seq_static(ctx, 0xA5, 0x80);
	tianma_dcs_write_seq_static(ctx, 0xA6, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xA9, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xE8, 0x2A);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0xE0);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x14, 0x60);
	tianma_dcs_write_seq_static(ctx, 0x16, 0xC0);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0xF0);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x3A, 0x08);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0x10);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0xB9, 0x01);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0x20);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x18, 0x40);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0x20);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0xC2, 0xDF);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0x10);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0xB9, 0x02);
	tianma_dcs_write_seq_static(ctx, 0xBB, 0x13);
	tianma_dcs_write_seq_static(ctx, 0x3B, 0x03,0x5F,0x1A,0x04,0x04);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0x25);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0xDF, 0x01);
	tianma_dcs_write_seq_static(ctx, 0xE0, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xE1, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xE2, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xE3, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xE4, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xE5, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xE6, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xE7, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xE8, 0xFF);
	tianma_dcs_write_seq_static(ctx, 0xE9, 0xFF);
	tianma_dcs_write_seq_static(ctx, 0xEA, 0xFF);
	tianma_dcs_write_seq_static(ctx, 0xEB, 0x20);
	tianma_dcs_write_seq_static(ctx, 0xEC, 0x00);
	tianma_dcs_write_seq_static(ctx, 0xFF, 0x10);
	tianma_dcs_write_seq_static(ctx, 0xFB, 0x01);
	tianma_dcs_write_seq_static(ctx, 0x35, 0x00);
	tianma_dcs_write_seq_static(ctx, 0x51, 0x07,0xFF);
	tianma_dcs_write_seq_static(ctx, 0x11);
	msleep(100);
	tianma_dcs_write_seq_static(ctx, 0x29);
	msleep(100);

}

static int tianma_disable(struct drm_panel *panel)
{
	struct tianma *ctx = panel_to_lcm(panel);

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	return 0;
}

static int tianma_unprepare(struct drm_panel *panel)
{
	struct tianma *ctx = panel_to_lcm(panel);

pr_info("kexin ************ %s ****************\n", __func__);
	pr_info("%s +\n", __func__);

return 0;

	if (!ctx->prepared)
		return 0;

	is_suspend = 1;

	tianma_dcs_write_seq_static(ctx, 0x28);
	msleep(100);
	tianma_dcs_write_seq_static(ctx, 0x10);
	msleep(100);

	ctx->error = 0;
	ctx->prepared = false;

#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
	tianma_panel_bias_disable();
#else
#if 0
	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_info(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	gpiod_set_value(ctx->reset_gpio, 0);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	udelay(10000);
#endif

	ctx->bias_neg = devm_gpiod_get_index(ctx->dev,
		"bias", 1, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_neg)) {
		dev_info(ctx->dev, "%s: cannot get bias_neg %ld\n",
			__func__, PTR_ERR(ctx->bias_neg));
		return PTR_ERR(ctx->bias_neg);
	}
	gpiod_set_value(ctx->bias_neg, 0);
	devm_gpiod_put(ctx->dev, ctx->bias_neg);

	udelay(1000);

	ctx->bias_pos = devm_gpiod_get_index(ctx->dev,
		"bias", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_pos)) {
		dev_info(ctx->dev, "%s: cannot get bias_pos %ld\n",
			__func__, PTR_ERR(ctx->bias_pos));
		return PTR_ERR(ctx->bias_pos);
	}
	gpiod_set_value(ctx->bias_pos, 0);
	devm_gpiod_put(ctx->dev, ctx->bias_pos);
#endif

	udelay(5000);

	ctx->vddio_gpio =
		devm_gpiod_get(ctx->dev, "vddio", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->vddio_gpio)) {
		dev_info(ctx->dev, "%s: cannot get vddio_gpio %ld\n",
			__func__, PTR_ERR(ctx->vddio_gpio));
		return PTR_ERR(ctx->vddio_gpio);
	}
	gpiod_set_value(ctx->vddio_gpio, 0);
	devm_gpiod_put(ctx->dev, ctx->vddio_gpio);

	pr_info("%s -\n", __func__);

	return 0;
}

static int tianma_prepare(struct drm_panel *panel)
{
	struct tianma *ctx = panel_to_lcm(panel);
	int ret;

pr_info("kexin ************ %s ****************\n", __func__);
	pr_info("%s +\n", __func__);

return 0;

	if (ctx->prepared)
		return 0;

	ctx->vddio_gpio =
		devm_gpiod_get(ctx->dev, "vddio", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->vddio_gpio)) {
		dev_info(ctx->dev, "%s: cannot get vddio_gpio %ld\n",
			__func__, PTR_ERR(ctx->vddio_gpio));
		return PTR_ERR(ctx->vddio_gpio);
	}
	gpiod_set_value(ctx->vddio_gpio, 1);
	devm_gpiod_put(ctx->dev, ctx->vddio_gpio);

	udelay(5000);

#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
	tianma_panel_bias_enable();
#else

	ctx->bias_pos = devm_gpiod_get_index(ctx->dev,
		"bias", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_pos)) {
		dev_info(ctx->dev, "%s: cannot get bias_pos %ld\n",
			__func__, PTR_ERR(ctx->bias_pos));
		return PTR_ERR(ctx->bias_pos);
	}
	gpiod_set_value(ctx->bias_pos, 1);
	devm_gpiod_put(ctx->dev, ctx->bias_pos);

	udelay(2000);
	ctx->bias_neg = devm_gpiod_get_index(ctx->dev,
		"bias", 1, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_neg)) {
		dev_info(ctx->dev, "%s: cannot get bias_neg %ld\n",
			__func__, PTR_ERR(ctx->bias_neg));
		return PTR_ERR(ctx->bias_neg);
	}
	gpiod_set_value(ctx->bias_neg, 1);
	devm_gpiod_put(ctx->dev, ctx->bias_neg);
#endif

	udelay(10000);
	tianma_panel_init(ctx);

	ret = ctx->error;
	if (ret < 0)
		tianma_unprepare(panel);

	ctx->prepared = true;

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_rst(panel);
#endif
#ifdef PANEL_SUPPORT_READBACK
	tianma_panel_get_data(ctx);
#endif

	is_suspend = 0;
	pr_info("%s -\n", __func__);

	return ret;
}

static int tianma_enable(struct drm_panel *panel)
{
	struct tianma *ctx = panel_to_lcm(panel);

	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;

	return 0;
}

#define HFP (60)
#define HSA (4)
#define HBP (60)
#define VFP_60 (26)
//#define VFP_90 (370)
#define VSA (2)
#define VBP (93)
#define VAC (2000)
#define HAC (1200)
#define FPS (60)

static const struct drm_display_mode default_mode = {
	.clock = 168412,
	.hdisplay = HAC,
	.hsync_start = HAC + HFP,
	.hsync_end = HAC + HFP + HSA,
	.htotal = HAC + HFP + HSA + HBP,
	.vdisplay = VAC,
	.vsync_start = VAC + VFP_60,
	.vsync_end = VAC + VFP_60 + VSA,
	.vtotal = VAC + VFP_60 + VSA + VBP,
};

/*static const struct drm_display_mode performance_mode = {
	.clock = 139932,
	.hdisplay = HAC,
	.hsync_start = HAC + HFP,
	.hsync_end = HAC + HFP + HSA,
	.htotal = HAC + HFP + HSA + HBP,
	.vdisplay = VAC,
	.vsync_start = VAC + VFP_90,
	.vsync_end = VAC + VFP_90 + VSA,
	.vtotal = VAC + VFP_90 + VSA + VBP,
};*/


#if defined(CONFIG_MTK_PANEL_EXT)
static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct tianma *ctx = panel_to_lcm(panel);

	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_info(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	gpiod_set_value(ctx->reset_gpio, on);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	return 0;
}

static int panel_ata_check(struct drm_panel *panel)
{
	struct tianma *ctx = panel_to_lcm(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	unsigned char data[3] = {0x00, 0x00, 0x00};
	unsigned char id[3] = {0x00, 0x80, 0x00};
	ssize_t ret;

	ret = mipi_dsi_dcs_read(dsi, 0x4, data, 3);
	if (ret < 0) {
		pr_info("%s error\n", __func__);
		return 0;
	}

	pr_info("ATA read data %x %x %x\n", data[0], data[1], data[2]);

	if (data[0] == id[0] &&
			data[1] == id[1] &&
			data[2] == id[2])
		return 1;

	pr_info("ATA expect read data is %x %x %x\n",
			id[0], id[1], id[2]);

	return 0;
}

static int tianma_setbacklight_cmdq(void *dsi, dcs_write_gce cb,
	void *handle, unsigned int level)
{
	char bl_tb0[] = {0x51, 0x07, 0xFF};

	if (!cb)
		return -1;

	bl_tb0[1] = (u8)((level >> 8) & 0x07);
	bl_tb0[2] = (u8)(level & 0xFF);

	cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));

	return 0;
}

static struct mtk_panel_params ext_params = {
	.pll_clk = 540,
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
};

/*static struct mtk_panel_params ext_params_90hz = {
	.pll_clk = 469,
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
};*/

struct drm_display_mode *get_mode_by_id_hfp(struct drm_connector *connector,
	unsigned int mode)
{
	struct drm_display_mode *m;
	unsigned int i = 0;

	list_for_each_entry(m, &connector->modes, head) {
		if (i == mode)
			return m;
		i++;
	}
	return NULL;
}

static int mtk_panel_ext_param_set(struct drm_panel *panel,
			struct drm_connector *connector, unsigned int mode)
{
	struct mtk_panel_ext *ext = find_panel_ext(panel);
	int ret = 0;
	struct drm_display_mode *m = get_mode_by_id_hfp(connector, mode);

	if (m == NULL) {
		pr_err("%s:%d invalid display_mode\n", __func__, __LINE__);
		return -1;
	}

	if (drm_mode_vrefresh(m) == 60)
		ext->params = &ext_params;
	/*else if (drm_mode_vrefresh(m) == 90)
		ext->params = &ext_params_90hz;*/
	else
		ret = 1;

	return ret;
}

static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.set_backlight_cmdq = tianma_setbacklight_cmdq,
	.ext_param_set = mtk_panel_ext_param_set,
	.ata_check = panel_ata_check,
};
#endif

struct panel_desc {
	const struct drm_display_mode *modes;
	unsigned int num_modes;

	unsigned int bpc;

	struct {
		unsigned int width;
		unsigned int height;
	} size;

	struct {
		unsigned int prepare;
		unsigned int enable;
		unsigned int disable;
		unsigned int unprepare;
	} delay;
};

static int tianma_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	//struct drm_display_mode *mode2;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

/*	mode2 = drm_mode_duplicate(connector->dev, &performance_mode);
	if (!mode2) {
		dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			 performance_mode.hdisplay, performance_mode.vdisplay,
			 drm_mode_vrefresh(&performance_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode2);
	mode2->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_probed_add(connector, mode2);*/

	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 152;

	return 1;
}

static const struct drm_panel_funcs tianma_drm_funcs = {
	.disable = tianma_disable,
	.unprepare = tianma_unprepare,
	.prepare = tianma_prepare,
	.enable = tianma_enable,
	.get_modes = tianma_get_modes,
};

/*static struct proc_dir_entry *proc_dir_tianmatek_lcd_info;

typedef struct {
	char *name;
	struct proc_dir_entry *node;
	struct proc_ops *fops;
	bool isCreated;
} tianma_proc_node;

static ssize_t tianma_hbm_read(struct file *filp, char __user *buff, size_t size, loff_t *pos)
{
	u32 len = 0;

	pr_info("%s enter! hbm=%d\n", __func__, hbm);

	if (*pos != 0)
		return 0;

	memset(hbm_buf, 0, 16 * sizeof(unsigned char));
	len += snprintf(hbm_buf + len, 16 - len, "%d\n", hbm);

	if (copy_to_user((char *)buff, hbm_buf, len))
		pr_err("Failed to copy data to user space\n");

	*pos += len;

	return len;
}

static ssize_t tianma_hbm_write(struct file *filp, const char *buff, size_t size, loff_t *pos)
{

	char cmd[16] = { 0 };
	int hbm_en;
	ssize_t ret;

	pr_info("%s enter!\n", __func__);

	if (is_suspend) {
		pr_info("In suspend, no write hbm, return now");
		return -1;
	}

	if ((size - 1) > sizeof(cmd)) {
		pr_err("ERROR! input length is larger than local buffer\n");
		return -1;
	}
	if (buff != NULL) {
		if (copy_from_user(cmd, buff, size)) {
			pr_err("Failed to copy data from user space\n");
			size = -1;
			goto out;
		}
	}

	hbm_en = simple_strtol(cmd,NULL, 0);

	if (0 == hbm_en) {
		pr_info("%s Disable hbm!\n", __func__);

		tianma_dcs_write_seq_static(ptx, 0x51,0x07,0xFF);

		ptx->bl_en_gpio =
				devm_gpiod_get(ptx->dev, "bl-enable", GPIOD_OUT_LOW);
		if (IS_ERR(ptx->bl_en_gpio)) {
			dev_info(ptx->dev, "%s: cannot get bl_en_gpio %ld\n",
							__func__, PTR_ERR(ptx->bl_en_gpio));
			return PTR_ERR(ptx->bl_en_gpio);
		}
		gpiod_set_value(ptx->bl_en_gpio, 0);
		devm_gpiod_put(ptx->dev, ptx->bl_en_gpio);
	} else if (1 == hbm_en) {
		pr_info("%s Enable hbm!\n", __func__);

		tianma_dcs_write_seq_static(ptx, 0x51,0x06,0xB8);

		ptx->bl_en_gpio =
				devm_gpiod_get(ptx->dev, "bl-enable", GPIOD_OUT_LOW);
		if (IS_ERR(ptx->bl_en_gpio)) {
			dev_info(ptx->dev, "%s: cannot get bl_en_gpio %ld\n",
							__func__, PTR_ERR(ptx->bl_en_gpio));
			return PTR_ERR(ptx->bl_en_gpio);
		}
		gpiod_set_value(ptx->bl_en_gpio, 1);
		devm_gpiod_put(ptx->dev, ptx->bl_en_gpio);
	} else if (2 == hbm_en) {
		pr_info("%s Enable hbm & ultra hbm!\n", __func__);

		tianma_dcs_write_seq_static(ptx, 0x51,0x07,0xFF);

		ptx->bl_en_gpio =
				devm_gpiod_get(ptx->dev, "bl-enable", GPIOD_OUT_LOW);
		if (IS_ERR(ptx->bl_en_gpio)) {
			dev_info(ptx->dev, "%s: cannot get bl_en_gpio %ld\n",
							__func__, PTR_ERR(ptx->bl_en_gpio));
			return PTR_ERR(ptx->bl_en_gpio);
		}
		gpiod_set_value(ptx->bl_en_gpio, 1);
		devm_gpiod_put(ptx->dev, ptx->bl_en_gpio);
	} else {
		pr_err("%s Wrong hbm parameter hbm_en=%d !\n", __func__, hbm_en);
		goto out;
	}
	hbm = hbm_en;

	pr_info("%s end! hbm=%d\n", __func__, hbm);

out:
	ret =size;
	return ret;
}

static struct proc_ops proc_tianma_hbm_fops = {
	.proc_read = tianma_hbm_read,
	.proc_write = tianma_hbm_write,
	.proc_lseek = default_llseek,
};

tianma_proc_node lcd_info_proc[] = {
	{"backlight_hbm", NULL, &proc_tianma_hbm_fops, false},
};*/

static int tianma_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct tianma *ctx;
	struct device_node *backlight;
	int ret;
	//int i = 0;
	struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;

pr_info("kexin ************ %s ****************\n", __func__);
	pr_info("nt36523w %s --- begin\n", __func__);
	dsi_node = of_get_parent(dev->of_node);
	if (dsi_node) {
		endpoint = of_graph_get_next_endpoint(dsi_node, NULL);
		if (endpoint) {
			remote_node = of_graph_get_remote_port_parent(endpoint);
			if (!remote_node) {
				pr_info("No panel connected,skip probe lcm\n");
				return -ENODEV;
			}
			pr_info("device node name:%s\n", remote_node->name);
		}
	}
	if (remote_node != dev->of_node) {
		pr_info("%s+ skip probe due to not current lcm\n", __func__);
		return -ENODEV;
	}

	ctx = devm_kzalloc(dev, sizeof(struct tianma), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE;

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

/*	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_info(dev, "%s: cannot get reset-gpios %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	devm_gpiod_put(dev, ctx->reset_gpio);

	ctx->vddio_gpio = devm_gpiod_get(dev, "vddio", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->vddio_gpio)) {
		dev_info(dev, "%s: cannot get vddio-gpios %ld\n",
			__func__, PTR_ERR(ctx->vddio_gpio));
		return PTR_ERR(ctx->vddio_gpio);
	}
	devm_gpiod_put(dev, ctx->vddio_gpio);

	ctx->bias_pos = devm_gpiod_get_index(dev, "bias", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_pos)) {
		dev_info(dev, "%s: cannot get bias-pos 0 %ld\n",
			__func__, PTR_ERR(ctx->bias_pos));
		return PTR_ERR(ctx->bias_pos);
	}
	devm_gpiod_put(dev, ctx->bias_pos);

	ctx->bias_neg = devm_gpiod_get_index(dev, "bias", 1, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_neg)) {
		dev_info(dev, "%s: cannot get bias-neg 1 %ld\n",
			__func__, PTR_ERR(ctx->bias_neg));
		return PTR_ERR(ctx->bias_neg);
	}
	devm_gpiod_put(dev, ctx->bias_neg);

	ctx->bl_en_gpio = devm_gpiod_get(dev, "bl-enable", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bl_en_gpio)) {
		dev_info(dev, "%s: cannot get bl-enable-gpios %ld\n",
			__func__, PTR_ERR(ctx->bl_en_gpio));
		return PTR_ERR(ctx->bl_en_gpio);
	}
	devm_gpiod_put(dev, ctx->bl_en_gpio);*/

	ctx->prepared = true;
	ctx->enabled = true;

	drm_panel_init(&ctx->panel, dev, &tianma_drm_funcs, DRM_MODE_CONNECTOR_DSI);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &tianma_drm_funcs;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->panel);

/*	pr_info("lcd_info_node_init\n");
	proc_dir_tianmatek_lcd_info = proc_mkdir("lcd_info", NULL);
	for (; i < ARRAY_SIZE(lcd_info_proc); i++) {
		lcd_info_proc[i].node = proc_create(lcd_info_proc[i].name, 0644,
					proc_dir_tianmatek_lcd_info, lcd_info_proc[i].fops);
		if (lcd_info_proc[i].node == NULL) {
			lcd_info_proc[i].isCreated = false;
			pr_err("Failed to create %s under /proc\n", lcd_info_proc[i].name);
		} else {
			lcd_info_proc[i].isCreated = true;
			pr_err("Succeed to create %s under /proc\n", lcd_info_proc[i].name);
		}
	}*/

#if defined(CONFIG_MTK_PANEL_EXT)
	//mtk_panel_tch_handle_reg(&ctx->panel);
	ret = mtk_panel_ext_create(dev, &ext_params, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;
#endif

#if IS_ENABLED(CONFIG_OEM_DEVINFO)
	FULL_PRODUCT_DEVICE_INFO(ID_LCD, "TM-NT36523W-VDO");
#endif

	ptx = ctx;
	hbm = 0;

	pr_info("nt36523w %s --- end\n", __func__);

	return ret;
}

static int tianma_remove(struct mipi_dsi_device *dsi)
{
	struct tianma *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id tianma_of_match[] = {
	{ .compatible = "tm,nt36523w,vdo", },
	{ }
};

MODULE_DEVICE_TABLE(of, tianma_of_match);

static struct mipi_dsi_driver tianma_driver = {
	.probe = tianma_probe,
	.remove = tianma_remove,
	.driver = {
		.name = "nt36523w_dsi_vdo_boe",
		.owner = THIS_MODULE,
		.of_match_table = tianma_of_match,
	},
};

/*static int __init tianma_drv_init(void)
{
	int ret = 0;

	pr_notice("%s+\n", __func__);
	mtk_panel_lock();
	ret = mipi_dsi_driver_register(&tianma_driver);
	if (ret < 0)
		pr_notice("%s, Failed to register jdi driver: %d\n",
			__func__, ret);

	mtk_panel_unlock();
	pr_notice("%s- ret:%d\n", __func__, ret);
	return 0;
}

static void __exit tianma_drv_exit(void)
{
	pr_notice("%s+\n", __func__);
	mtk_panel_lock();
	mipi_dsi_driver_unregister(&tianma_driver);
	mtk_panel_unlock();
	pr_notice("%s-\n", __func__);
}
module_init(tianma_drv_init);
module_exit(tianma_drv_exit);*/

module_mipi_dsi_driver(tianma_driver);

MODULE_AUTHOR("Ning Feng <Ning.Feng@mediatek.com>");
MODULE_DESCRIPTION("TM NT36523W VDO LCD Panel Driver");
MODULE_LICENSE("GPL v2");
