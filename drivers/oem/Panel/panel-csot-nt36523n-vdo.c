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

#include <linux/i2c-dev.h>
#include <linux/i2c.h>

#if IS_ENABLED(CONFIG_TINNO_DEVINFO)
#include "../dev_info/dev_info.h"
#endif

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../../gpu/drm/mediatek/mediatek_v2/mtk_panel_ext.h"
#include "../../gpu/drm/mediatek/mediatek_v2/mtk_drm_graphics_base.h"
#endif

#ifdef CONFIG_MTK_ROUND_CORNER_SUPPORT
#include "../../gpu/drm/mediatek/mediatek_v2/mtk_corner_pattern/mtk_data_hw_roundedpattern.h"
#endif

#include "Backlight_I2C_map.h"

#define BRIGHTNESS_HIGN_OFFSET		0x3
#define BRIGHTNESS_HIGN_MASK		0xFF
#define BRIGHTNESS_LOW_OFFSET		0x0
#define BRIGHTNESS_LOW_MASK		0x7

extern int double_click_wakeup_support(void);
int gesture_mode = -1;
//#define BYPASSI2C
#ifndef BYPASSI2C
/* i2c control start */
#define LCM_I2C_ID_NAME "I2C_LCD_BIAS"
static struct i2c_client *_lcm_i2c_client;
 
/*****************************************************************************
 * Function Prototype
*****************************************************************************/
static int _lcm_i2c_probe(struct i2c_client *client,
            const struct i2c_device_id *id);
static int _lcm_i2c_remove(struct i2c_client *client);
 
 /*****************************************************************************
  * Data Structure
  *****************************************************************************/
 struct _lcm_i2c_dev {
    struct i2c_client *client;
 };
 
 static const struct of_device_id _lcm_i2c_of_match[] = {
    {
        .compatible = "mediatek,I2C_LCD_BIAS",
    },
    {},
 };
 
 static const struct i2c_device_id _lcm_i2c_id[] = { { LCM_I2C_ID_NAME, 0 },
 						    {} };
 
 static struct i2c_driver _lcm_i2c_driver = {
    .id_table = _lcm_i2c_id,
    .probe = _lcm_i2c_probe,
    .remove = _lcm_i2c_remove,
    /* .detect		   = _lcm_i2c_detect, */
    .driver = {
        .owner = THIS_MODULE,
        .name = LCM_I2C_ID_NAME,
        .of_match_table = _lcm_i2c_of_match,
    },
 };
 
 /*****************************************************************************
  * Function
  *****************************************************************************/
 
  static int _lcm_i2c_probe(struct i2c_client *client,
        const struct i2c_device_id *id)
  {
    printk("[LCM][I2C] %s\n", __func__);
    printk("[LCM][I2C] NT: info==>name=%s addr=0x%x\n", client->name,
        client->addr);
    _lcm_i2c_client = client;
    return 0;
  }
  
  static int _lcm_i2c_remove(struct i2c_client *client)
  {
    printk("[LCM][I2C] %s\n", __func__);
    _lcm_i2c_client = NULL;
    i2c_unregister_device(client);
    return 0;
  }
  
  static int _lcm_i2c_write_bytes(unsigned char addr, unsigned char value)
  {
    int ret = 0;
    struct i2c_client *client = _lcm_i2c_client;
    char write_data[2] = { 0 };
  
    if (client == NULL) {
        printk("ERROR!! _lcm_i2c_client is null\n");
        return 0;
    }
  
    write_data[0] = addr;
    write_data[1] = value;
    printk("i2c wirte addr:0x%x,value:0x%x\n",addr,value);
    ret = i2c_master_send(client, write_data, 2);
    if (ret < 0)
        printk("[LCM][ERROR] _lcm_i2c write data fail !!\n");
  
    return ret;
  }
#endif

struct csot {
    struct device *dev;
    struct drm_panel panel;
    struct backlight_device *backlight;
    struct gpio_desc *reset_gpio;
    struct gpio_desc *vddio_gpio;
    struct gpio_desc *bias_pos, *bias_neg;
    struct gpio_desc *bl_en_gpio;

    bool prepared;
    bool enabled;

    int error;
};

#define csot_dcs_write_seq(ctx, seq...) \
({\
	const u8 d[] = { seq };\
	BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64, "DCS sequence too big for stack");\
	csot_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

#define csot_dcs_write_seq_static(ctx, seq...) \
({\
    static const u8 d[] = { seq };\
    csot_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

static inline struct csot *panel_to_lcm(struct drm_panel *panel)
{
    return container_of(panel, struct csot, panel);
}

static void csot_dcs_write(struct csot *ctx, const void *data, size_t len)
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

static void csot_panel_init(struct csot *ctx)
{
    ctx->reset_gpio =
        devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->reset_gpio)) {
        dev_info(ctx->dev, "%s: cannot get reset_gpio %ld\n",
            __func__, PTR_ERR(ctx->reset_gpio));
        return;
    }
        gpiod_set_value(ctx->reset_gpio, 0);
        udelay(10 * 1000);
        gpiod_set_value(ctx->reset_gpio, 1);
        udelay(10 * 1000);
        gpiod_set_value(ctx->reset_gpio, 0);
        udelay(10 * 1000);
        gpiod_set_value(ctx->reset_gpio, 1);
        udelay(10 * 1000);
        devm_gpiod_put(ctx->dev, ctx->reset_gpio);

#ifdef TINNO_LCM_OEM_CONFIG
    csot_resume_by_ddi();
#endif

	csot_dcs_write_seq_static(ctx,0xFF,0x20);

	csot_dcs_write_seq_static(ctx,0xFB,0x01);

	csot_dcs_write_seq_static(ctx,0x05,0xD1);
	csot_dcs_write_seq_static(ctx,0x06,0xC0);
	csot_dcs_write_seq_static(ctx,0x07,0x8C);
	csot_dcs_write_seq_static(ctx,0x08,0x46);
	csot_dcs_write_seq_static(ctx,0x0D,0x63);
	csot_dcs_write_seq_static(ctx,0x0E,0xAA);
	csot_dcs_write_seq_static(ctx,0x0F,0x64);
	csot_dcs_write_seq_static(ctx,0x30,0x10);
	csot_dcs_write_seq_static(ctx,0x31,0x02);
	csot_dcs_write_seq_static(ctx,0x32,0x42);
	csot_dcs_write_seq_static(ctx,0x45,0xDD);
	csot_dcs_write_seq_static(ctx,0x58,0x43);

	csot_dcs_write_seq_static(ctx,0x69,0xDD);
	csot_dcs_write_seq_static(ctx,0x65,0x55);
	csot_dcs_write_seq_static(ctx,0x6D,0xAA);
	csot_dcs_write_seq_static(ctx,0x6E,0x00);
	csot_dcs_write_seq_static(ctx,0x75,0xC4);

	csot_dcs_write_seq_static(ctx,0x94,0xC0);
	csot_dcs_write_seq_static(ctx,0x95,0x09);
	csot_dcs_write_seq_static(ctx,0x96,0x09);

	csot_dcs_write_seq_static(ctx,0xFF,0x24);

	csot_dcs_write_seq_static(ctx,0xFB,0x01);

	csot_dcs_write_seq_static(ctx,0x00,0x03);
	csot_dcs_write_seq_static(ctx,0x01,0x03);
	csot_dcs_write_seq_static(ctx,0x02,0x26);
	csot_dcs_write_seq_static(ctx,0x03,0x26);
	csot_dcs_write_seq_static(ctx,0x04,0x03);
	csot_dcs_write_seq_static(ctx,0x05,0x0F);
	csot_dcs_write_seq_static(ctx,0x06,0x0F);
	csot_dcs_write_seq_static(ctx,0x07,0x0E);
	csot_dcs_write_seq_static(ctx,0x08,0x0E);
	csot_dcs_write_seq_static(ctx,0x09,0x0D);
	csot_dcs_write_seq_static(ctx,0x0A,0x0D);
	csot_dcs_write_seq_static(ctx,0x0B,0x0C);
	csot_dcs_write_seq_static(ctx,0x0C,0x0C);
	csot_dcs_write_seq_static(ctx,0x0D,0x03);
	csot_dcs_write_seq_static(ctx,0x0E,0x04);
	csot_dcs_write_seq_static(ctx,0x0F,0x04);
	csot_dcs_write_seq_static(ctx,0x10,0x1D);
	csot_dcs_write_seq_static(ctx,0x11,0x1D);
	csot_dcs_write_seq_static(ctx,0x12,0x1C);
	csot_dcs_write_seq_static(ctx,0x13,0x1C);
	csot_dcs_write_seq_static(ctx,0x14,0x08);
	csot_dcs_write_seq_static(ctx,0x15,0x08);
	csot_dcs_write_seq_static(ctx,0x16,0x03);
	csot_dcs_write_seq_static(ctx,0x17,0x03);
	csot_dcs_write_seq_static(ctx,0x18,0x26);
	csot_dcs_write_seq_static(ctx,0x19,0x26);
	csot_dcs_write_seq_static(ctx,0x1A,0x03);
	csot_dcs_write_seq_static(ctx,0x1B,0x0F);
	csot_dcs_write_seq_static(ctx,0x1C,0x0F);
	csot_dcs_write_seq_static(ctx,0x1D,0x0E);
	csot_dcs_write_seq_static(ctx,0x1E,0x0E);
	csot_dcs_write_seq_static(ctx,0x1F,0x0D);
	csot_dcs_write_seq_static(ctx,0x20,0x0D);
	csot_dcs_write_seq_static(ctx,0x21,0x0C);
	csot_dcs_write_seq_static(ctx,0x22,0x0C);
	csot_dcs_write_seq_static(ctx,0x23,0x03);
	csot_dcs_write_seq_static(ctx,0x24,0x04);
	csot_dcs_write_seq_static(ctx,0x25,0x04);
	csot_dcs_write_seq_static(ctx,0x26,0x1D);
	csot_dcs_write_seq_static(ctx,0x27,0x1D);
	csot_dcs_write_seq_static(ctx,0x28,0x1C);
	csot_dcs_write_seq_static(ctx,0x29,0x1C);
	csot_dcs_write_seq_static(ctx,0x2A,0x08);
	csot_dcs_write_seq_static(ctx,0x2B,0x08);

	csot_dcs_write_seq_static(ctx,0x2D,0x00);
	csot_dcs_write_seq_static(ctx,0x2F,0x04);
	csot_dcs_write_seq_static(ctx,0x30,0x00);
	csot_dcs_write_seq_static(ctx,0x33,0x00);
	csot_dcs_write_seq_static(ctx,0x34,0x00);
	csot_dcs_write_seq_static(ctx,0x37,0x44);
	csot_dcs_write_seq_static(ctx,0x39,0x00);
	csot_dcs_write_seq_static(ctx,0x3A,0x02);
	csot_dcs_write_seq_static(ctx,0x3B,0x88);
	csot_dcs_write_seq_static(ctx,0x3D,0x91);
	csot_dcs_write_seq_static(ctx,0xAB,0x44);

	csot_dcs_write_seq_static(ctx,0x3F,0x0B);
	csot_dcs_write_seq_static(ctx,0x43,0x00);
	csot_dcs_write_seq_static(ctx,0x47,0x40);
	csot_dcs_write_seq_static(ctx,0x49,0x00);
	csot_dcs_write_seq_static(ctx,0x4A,0x02);
	csot_dcs_write_seq_static(ctx,0x4B,0x88);
	csot_dcs_write_seq_static(ctx,0x4C,0x01);

	csot_dcs_write_seq_static(ctx,0x4D,0x21);
	csot_dcs_write_seq_static(ctx,0x4E,0x43);
	csot_dcs_write_seq_static(ctx,0x4F,0x00);
	csot_dcs_write_seq_static(ctx,0x50,0x00);
	csot_dcs_write_seq_static(ctx,0x51,0x00);
	csot_dcs_write_seq_static(ctx,0x52,0x00);
	csot_dcs_write_seq_static(ctx,0x53,0x00);
	csot_dcs_write_seq_static(ctx,0x54,0x00);
	csot_dcs_write_seq_static(ctx,0x55,0x83,0x00);
	csot_dcs_write_seq_static(ctx,0x56,0x04);
	csot_dcs_write_seq_static(ctx,0x58,0x21);
	csot_dcs_write_seq_static(ctx,0x59,0x40);
	csot_dcs_write_seq_static(ctx,0x5A,0x08);
	csot_dcs_write_seq_static(ctx,0x5B,0x74);
	csot_dcs_write_seq_static(ctx,0x5C,0x00);
	csot_dcs_write_seq_static(ctx,0x5D,0x00);
	csot_dcs_write_seq_static(ctx,0x5E,0x00,0x04);

	csot_dcs_write_seq_static(ctx,0x60,0x96);
	csot_dcs_write_seq_static(ctx,0x61,0xD0);
	csot_dcs_write_seq_static(ctx,0x63,0x70);

	csot_dcs_write_seq_static(ctx,0x7A,0x00);
	csot_dcs_write_seq_static(ctx,0x7B,0x44);
	csot_dcs_write_seq_static(ctx,0x7E,0x20);
	csot_dcs_write_seq_static(ctx,0x7F,0xB4);
	csot_dcs_write_seq_static(ctx,0x80,0x00);
	csot_dcs_write_seq_static(ctx,0x81,0x00);
	csot_dcs_write_seq_static(ctx,0x82,0x08);
	csot_dcs_write_seq_static(ctx,0x83,0x1B);
	csot_dcs_write_seq_static(ctx,0x97,0xC2);
	csot_dcs_write_seq_static(ctx,0x9F,0x00);
	csot_dcs_write_seq_static(ctx,0xA0,0x00);

	csot_dcs_write_seq_static(ctx,0x91,0x40);
	csot_dcs_write_seq_static(ctx,0x92,0x8E);
	csot_dcs_write_seq_static(ctx,0x93,0x1E,0x00);
	csot_dcs_write_seq_static(ctx,0x94,0x8C,0x00);

	csot_dcs_write_seq_static(ctx,0x95,0x81);

	csot_dcs_write_seq_static(ctx,0x96,0x00);

	csot_dcs_write_seq_static(ctx,0xAA,0x8A);

	csot_dcs_write_seq_static(ctx,0xB6,0x05,0x00,0x05,0x00,0x00,0x00,0x05,0x05,0x05,0x05,0x00,0x00);

	csot_dcs_write_seq_static(ctx,0xBB,0x80);
	csot_dcs_write_seq_static(ctx,0xBC,0x00,0x00,0x43,0x00,0x00);

	csot_dcs_write_seq_static(ctx,0xC2,0xCE,0x00,0x00);

	csot_dcs_write_seq_static(ctx,0xDB,0x01);
	csot_dcs_write_seq_static(ctx,0xDC,0x8F);
	csot_dcs_write_seq_static(ctx,0xDF,0x01);
	csot_dcs_write_seq_static(ctx,0xE0,0x8F);
	csot_dcs_write_seq_static(ctx,0xE1,0x01);
	csot_dcs_write_seq_static(ctx,0xE2,0x8F);
	csot_dcs_write_seq_static(ctx,0xE3,0x01);
	csot_dcs_write_seq_static(ctx,0xE4,0x8F);
	csot_dcs_write_seq_static(ctx,0xE5,0x01);
	csot_dcs_write_seq_static(ctx,0xE6,0x8F);
	csot_dcs_write_seq_static(ctx,0xEF,0x01);
	csot_dcs_write_seq_static(ctx,0xF0,0x8F);

	csot_dcs_write_seq_static(ctx,0xFF,0x25);

	csot_dcs_write_seq_static(ctx,0xFB,0x01);

	csot_dcs_write_seq_static(ctx,0x05,0x00);

	csot_dcs_write_seq_static(ctx,0x13,0x02);
	csot_dcs_write_seq_static(ctx,0x14,0xE1);

	csot_dcs_write_seq_static(ctx,0xDB,0x02);
	csot_dcs_write_seq_static(ctx,0xDC,0x44);

	csot_dcs_write_seq_static(ctx,0x19,0x07);
	csot_dcs_write_seq_static(ctx,0x1B,0x11);

	csot_dcs_write_seq_static(ctx,0x1E,0x00);
	csot_dcs_write_seq_static(ctx,0x1F,0x08);
	csot_dcs_write_seq_static(ctx,0x20,0x74);
	csot_dcs_write_seq_static(ctx,0x25,0x00);
	csot_dcs_write_seq_static(ctx,0x26,0x08);
	csot_dcs_write_seq_static(ctx,0x27,0x74);
	csot_dcs_write_seq_static(ctx,0x3F,0x00);
	csot_dcs_write_seq_static(ctx,0x40,0x00);
	csot_dcs_write_seq_static(ctx,0x43,0x00);
	csot_dcs_write_seq_static(ctx,0x44,0x02);
	csot_dcs_write_seq_static(ctx,0x45,0x88);
	csot_dcs_write_seq_static(ctx,0x48,0x08);
	csot_dcs_write_seq_static(ctx,0x49,0x74);

	csot_dcs_write_seq_static(ctx,0x5B,0x80);
	csot_dcs_write_seq_static(ctx,0x5C,0x00);
	csot_dcs_write_seq_static(ctx,0x5D,0x02);
	csot_dcs_write_seq_static(ctx,0x5E,0x88);
	csot_dcs_write_seq_static(ctx,0x61,0x08);
	csot_dcs_write_seq_static(ctx,0x62,0x74);
	csot_dcs_write_seq_static(ctx,0x67,0x00);
	csot_dcs_write_seq_static(ctx,0x68,0x04);

	csot_dcs_write_seq_static(ctx,0xC2,0x40);
	csot_dcs_write_seq_static(ctx,0xC5,0x17);

	csot_dcs_write_seq_static(ctx,0xFF,0x26);

	csot_dcs_write_seq_static(ctx,0xFB,0x01);

	csot_dcs_write_seq_static(ctx,0x00,0x81);
	csot_dcs_write_seq_static(ctx,0x06,0x32);

	csot_dcs_write_seq_static(ctx,0x04,0x50);

	csot_dcs_write_seq_static(ctx,0x0A,0xF2);

	csot_dcs_write_seq_static(ctx,0x0C,0x0C);
	csot_dcs_write_seq_static(ctx,0x0D,0x10);
	csot_dcs_write_seq_static(ctx,0x0F,0x00);
	csot_dcs_write_seq_static(ctx,0x11,0x00);
	csot_dcs_write_seq_static(ctx,0x12,0x50);
	csot_dcs_write_seq_static(ctx,0x13,0x5D);
	csot_dcs_write_seq_static(ctx,0x14,0x9B);
	csot_dcs_write_seq_static(ctx,0x16,0x10);
	csot_dcs_write_seq_static(ctx,0x19,0x17);
	csot_dcs_write_seq_static(ctx,0x1A,0xE7);
	csot_dcs_write_seq_static(ctx,0x1B,0x16);
	csot_dcs_write_seq_static(ctx,0x1C,0x7F);
	csot_dcs_write_seq_static(ctx,0x1D,0x00);
	csot_dcs_write_seq_static(ctx,0x1E,0x8E);
	csot_dcs_write_seq_static(ctx,0x1F,0x8E);
	csot_dcs_write_seq_static(ctx,0x20,0x00);
	csot_dcs_write_seq_static(ctx,0x21,0x07);
	csot_dcs_write_seq_static(ctx,0x2A,0x17);
	csot_dcs_write_seq_static(ctx,0x2B,0xE7);
	csot_dcs_write_seq_static(ctx,0x2F,0x03);
	csot_dcs_write_seq_static(ctx,0x30,0x8E);
	csot_dcs_write_seq_static(ctx,0x31,0x00);
	csot_dcs_write_seq_static(ctx,0x32,0x8E);
	csot_dcs_write_seq_static(ctx,0x39,0x04);
	csot_dcs_write_seq_static(ctx,0x3A,0x8E);

	csot_dcs_write_seq_static(ctx,0x33,0x66);
	csot_dcs_write_seq_static(ctx,0x34,0x06);
	csot_dcs_write_seq_static(ctx,0x35,0x00);
	csot_dcs_write_seq_static(ctx,0x36,0x00);
	csot_dcs_write_seq_static(ctx,0x37,0x00);
	csot_dcs_write_seq_static(ctx,0x38,0x00);

	csot_dcs_write_seq_static(ctx,0xC9,0x86);

	csot_dcs_write_seq_static(ctx,0xA9,0x5C);
	csot_dcs_write_seq_static(ctx,0xAA,0x5B);
	csot_dcs_write_seq_static(ctx,0xAB,0x5A);
	csot_dcs_write_seq_static(ctx,0xAC,0x59);
	csot_dcs_write_seq_static(ctx,0xAD,0x58);

	csot_dcs_write_seq_static(ctx,0xC7,0x00);
	csot_dcs_write_seq_static(ctx,0xC8,0x00);

	csot_dcs_write_seq_static(ctx,0xCD,0x2D,0x50);
	csot_dcs_write_seq_static(ctx,0xCE,0x2B,0x50);
	csot_dcs_write_seq_static(ctx,0xCF,0x03,0x00,0x20);
	csot_dcs_write_seq_static(ctx,0xD0,0x00,0x00);
	csot_dcs_write_seq_static(ctx,0xD1,0x00,0x00);
	csot_dcs_write_seq_static(ctx,0xD2,0x3F);

	csot_dcs_write_seq_static(ctx,0xFF,0x27);

	csot_dcs_write_seq_static(ctx,0xFB,0x01);

	csot_dcs_write_seq_static(ctx,0x13,0x06);

	csot_dcs_write_seq_static(ctx,0x58,0x80);
	csot_dcs_write_seq_static(ctx,0x59,0x00);
	csot_dcs_write_seq_static(ctx,0x5A,0x00);
	csot_dcs_write_seq_static(ctx,0x5B,0x7D);
	csot_dcs_write_seq_static(ctx,0x5C,0x00);
	csot_dcs_write_seq_static(ctx,0x5D,0x00);
	csot_dcs_write_seq_static(ctx,0x5E,0x00);
	csot_dcs_write_seq_static(ctx,0x5F,0x00);
	csot_dcs_write_seq_static(ctx,0x60,0x00);
	csot_dcs_write_seq_static(ctx,0x61,0x00);
	csot_dcs_write_seq_static(ctx,0x62,0x00);
	csot_dcs_write_seq_static(ctx,0x63,0x00);
	csot_dcs_write_seq_static(ctx,0x64,0x00);
	csot_dcs_write_seq_static(ctx,0x65,0x00);
	csot_dcs_write_seq_static(ctx,0x66,0x00);
	csot_dcs_write_seq_static(ctx,0x67,0x00);
	csot_dcs_write_seq_static(ctx,0x68,0x00);
	csot_dcs_write_seq_static(ctx,0x75,0xA8);
	csot_dcs_write_seq_static(ctx,0x76,0x00);
	csot_dcs_write_seq_static(ctx,0x77,0xA0);

	csot_dcs_write_seq_static(ctx,0xC0,0x00);

	csot_dcs_write_seq_static(ctx,0xD0,0x13);
	csot_dcs_write_seq_static(ctx,0xD1,0x54);
	csot_dcs_write_seq_static(ctx,0xD2,0x30);
	csot_dcs_write_seq_static(ctx,0xDE,0x43);
	csot_dcs_write_seq_static(ctx,0xDF,0x02);//ESD

	csot_dcs_write_seq_static(ctx,0xFF,0x2A);

	csot_dcs_write_seq_static(ctx,0xFB,0x01);

	csot_dcs_write_seq_static(ctx,0x22,0x0F);
	csot_dcs_write_seq_static(ctx,0x23,0x10);

	csot_dcs_write_seq_static(ctx,0x24,0x00);
	csot_dcs_write_seq_static(ctx,0x25,0x8E);
	csot_dcs_write_seq_static(ctx,0x27,0x00);
	csot_dcs_write_seq_static(ctx,0x28,0x1E);
	csot_dcs_write_seq_static(ctx,0x29,0x00);
	csot_dcs_write_seq_static(ctx,0x2A,0x1E);
	csot_dcs_write_seq_static(ctx,0x2B,0x00);
	csot_dcs_write_seq_static(ctx,0x2D,0x1E);

	csot_dcs_write_seq_static(ctx,0x64,0x43);
	csot_dcs_write_seq_static(ctx,0x67,0x43);
	csot_dcs_write_seq_static(ctx,0x6A,0x43);
	csot_dcs_write_seq_static(ctx,0x70,0x43);
	csot_dcs_write_seq_static(ctx,0x7F,0x43);

	csot_dcs_write_seq_static(ctx,0x97,0x3C);
	csot_dcs_write_seq_static(ctx,0x98,0x02);
	csot_dcs_write_seq_static(ctx,0x99,0x95);
	csot_dcs_write_seq_static(ctx,0x9A,0x03);
	csot_dcs_write_seq_static(ctx,0x9B,0x00);
	csot_dcs_write_seq_static(ctx,0x9C,0x0B);
	csot_dcs_write_seq_static(ctx,0x9D,0x0A);
	csot_dcs_write_seq_static(ctx,0x9E,0x90);

	csot_dcs_write_seq_static(ctx,0xF2,0x3C);
	csot_dcs_write_seq_static(ctx,0xF3,0x02);
	csot_dcs_write_seq_static(ctx,0xF4,0x95);
	csot_dcs_write_seq_static(ctx,0xF5,0x03);
	csot_dcs_write_seq_static(ctx,0xF6,0x00);
	csot_dcs_write_seq_static(ctx,0xF7,0x0B);
	csot_dcs_write_seq_static(ctx,0xF8,0x0A);
	csot_dcs_write_seq_static(ctx,0xF9,0x90);

	csot_dcs_write_seq_static(ctx,0xA2,0x3F);
	csot_dcs_write_seq_static(ctx,0xA4,0xC3);

	csot_dcs_write_seq_static(ctx,0xE8,0x00);

	csot_dcs_write_seq_static(ctx,0xB9,0x14,0x00,0x95,0x00,0xF4,0x86,0xC2,0x70,0x40);

	csot_dcs_write_seq_static(ctx,0xBC,0x03);

	csot_dcs_write_seq_static(ctx,0xFF,0x23);

	csot_dcs_write_seq_static(ctx,0xFB,0x01);

	csot_dcs_write_seq_static(ctx,0x00,0x60);
	csot_dcs_write_seq_static(ctx,0x07,0x20);
	csot_dcs_write_seq_static(ctx,0x08,0x01);
	csot_dcs_write_seq_static(ctx,0x09,0x2C);
	csot_dcs_write_seq_static(ctx,0x11,0x01);
	csot_dcs_write_seq_static(ctx,0x12,0x77);
	csot_dcs_write_seq_static(ctx,0x15,0x07);
	csot_dcs_write_seq_static(ctx,0x16,0x10);

	csot_dcs_write_seq_static(ctx,0xFF,0x20);
	csot_dcs_write_seq_static(ctx,0xFB,0x01);
	csot_dcs_write_seq_static(ctx,0xC6,0x52);
	csot_dcs_write_seq_static(ctx,0xC7,0x20);
	csot_dcs_write_seq_static(ctx,0xC8,0x31);
	csot_dcs_write_seq_static(ctx,0xC9,0x21);
	csot_dcs_write_seq_static(ctx,0xCA,0x10);
	csot_dcs_write_seq_static(ctx,0xCB,0x52);
	csot_dcs_write_seq_static(ctx,0xCC,0x20);
	csot_dcs_write_seq_static(ctx,0xCD,0x31);
	csot_dcs_write_seq_static(ctx,0xCE,0x21);
	csot_dcs_write_seq_static(ctx,0xCF,0x10);
	csot_dcs_write_seq_static(ctx,0xD0,0x52);
	csot_dcs_write_seq_static(ctx,0xD1,0x20);
	csot_dcs_write_seq_static(ctx,0xD2,0x31);
	csot_dcs_write_seq_static(ctx,0xD3,0x21);
	csot_dcs_write_seq_static(ctx,0xD4,0x10);
	csot_dcs_write_seq_static(ctx,0xD5,0x52);
	csot_dcs_write_seq_static(ctx,0xD6,0x20);
	csot_dcs_write_seq_static(ctx,0xD7,0x31);
	csot_dcs_write_seq_static(ctx,0xD8,0x21);
	csot_dcs_write_seq_static(ctx,0xD9,0x10);
	csot_dcs_write_seq_static(ctx,0xDA,0x52);
	csot_dcs_write_seq_static(ctx,0xDB,0x20);
	csot_dcs_write_seq_static(ctx,0xDC,0x31);
	csot_dcs_write_seq_static(ctx,0xDD,0x21);
	csot_dcs_write_seq_static(ctx,0xDE,0x10);
	csot_dcs_write_seq_static(ctx,0xDF,0x52);
	csot_dcs_write_seq_static(ctx,0xE0,0x20);
	csot_dcs_write_seq_static(ctx,0xE1,0x31);
	csot_dcs_write_seq_static(ctx,0xE2,0x21);
	csot_dcs_write_seq_static(ctx,0xE3,0x10);

	csot_dcs_write_seq_static(ctx,0xFF,0xE0);
	csot_dcs_write_seq_static(ctx,0xFB,0x01);

	csot_dcs_write_seq_static(ctx,0x14,0x60);
	csot_dcs_write_seq_static(ctx,0x16,0xC0);

	csot_dcs_write_seq_static(ctx,0xFF,0x10);

	csot_dcs_write_seq_static(ctx,0xFB,0x01);

	csot_dcs_write_seq_static(ctx,0x35,0x00);
	csot_dcs_write_seq_static(ctx,0x51,0x07,0xFF);
	csot_dcs_write_seq_static(ctx,0x53,0x2C);
	csot_dcs_write_seq_static(ctx,0x55,0x00);
	csot_dcs_write_seq_static(ctx,0x90,0x03);
	csot_dcs_write_seq_static(ctx,0x91,0x89, 0xA8, 0x00, 0x14, 0xD2, 0x00, 0x02, 0x45, 0x01, 0xEC, 0x00, 0x08, 0x05, 0x7A, 0x04, 0x94);
	csot_dcs_write_seq_static(ctx,0x92,0x10,0xF0);
	csot_dcs_write_seq_static(ctx,0xB2,0x80);
	csot_dcs_write_seq_static(ctx,0xB3,0x00);
	csot_dcs_write_seq_static(ctx,0xBB,0x13);
	csot_dcs_write_seq_static(ctx,0x3B,0x03,0x8C,0x1E,0x04,0x04,0x00);
	csot_dcs_write_seq_static(ctx,0x11,0x00);
	msleep(70);
	csot_dcs_write_seq_static(ctx,0x29,0x00);
	msleep(40);
}

static int csot_disable(struct drm_panel *panel)
{
    struct csot *ctx = panel_to_lcm(panel);

    if (!ctx->enabled)
        return 0;

    if (ctx->backlight) {
        ctx->backlight->props.power = FB_BLANK_POWERDOWN;
        backlight_update_status(ctx->backlight);
    }

    ctx->enabled = false;

    return 0;
}

static int csot_unprepare(struct drm_panel *panel)
{
    struct csot *ctx = panel_to_lcm(panel);

    pr_info("%s +\n", __func__);

    if (!ctx->prepared)
        return 0;

    gesture_mode = double_click_wakeup_support();
    if(gesture_mode) {
        _lcm_i2c_write_bytes(0x08, 0x1F);
        msleep(100);
        pr_info("%s leda off !\n", __func__);
    }

    csot_dcs_write_seq_static(ctx, 0x28);
    csot_dcs_write_seq_static(ctx, 0x10);
    msleep(120);

    ctx->error = 0;
    ctx->prepared = false;

    if(gesture_mode) {
        pr_info("%s nt36523n gesture on ! Skip Power Control !\n", __func__);
        pr_info("%s -\n", __func__);
        return 0;
    }

#if 1
    ctx->reset_gpio =
        devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->reset_gpio)) {
        dev_info(ctx->dev, "%s: cannot get reset_gpio %ld\n",
            __func__, PTR_ERR(ctx->reset_gpio));
        return PTR_ERR(ctx->reset_gpio);
    }
    gpiod_set_value(ctx->reset_gpio, 0);
    devm_gpiod_put(ctx->dev, ctx->reset_gpio);

    udelay(1000);
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

    msleep(30);
    ctx->bl_en_gpio =
        devm_gpiod_get(ctx->dev, "bl-enable", GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->bl_en_gpio)) {
        dev_info(ctx->dev, "%s: cannot get bl_en_gpio %ld\n",
            __func__, PTR_ERR(ctx->bl_en_gpio));
        return PTR_ERR(ctx->bl_en_gpio);
    }
    gpiod_set_value(ctx->bl_en_gpio, 0);
    devm_gpiod_put(ctx->dev, ctx->bl_en_gpio);

    udelay(1000);
    ctx->vddio_gpio =
        devm_gpiod_get(ctx->dev, "vddi", GPIOD_OUT_HIGH);
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

static int csot_prepare(struct drm_panel *panel)
{
    struct csot *ctx = panel_to_lcm(panel);
    int ret;

    pr_info("%s +\n", __func__);

    if (ctx->prepared)
        return 0;

    gesture_mode = double_click_wakeup_support();
    if(gesture_mode) {
        pr_info("nt36523n gesture on ! Skip Power Control !\n");
        udelay(10 * 1000);
        csot_panel_init(ctx);
        _lcm_i2c_write_bytes(0x08, 0x5F);
        ret = ctx->error;
        if (ret < 0)
        csot_unprepare(panel);
        ctx->prepared = true;
#if defined(CONFIG_MTK_PANEL_EXT)
        mtk_panel_tch_rst(panel);
#endif
#ifdef PANEL_SUPPORT_READBACK
        csot_panel_get_data(ctx);
#endif
        pr_info("%s -\n", __func__);
        return ret;
    }
    ctx->vddio_gpio =
        devm_gpiod_get(ctx->dev, "vddi", GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->vddio_gpio)) {
        dev_info(ctx->dev, "%s: cannot get vddio_gpio %ld\n",
            __func__, PTR_ERR(ctx->vddio_gpio));
        return PTR_ERR(ctx->vddio_gpio);
    }
    gpiod_set_value(ctx->vddio_gpio, 1);
    devm_gpiod_put(ctx->dev, ctx->vddio_gpio);

    udelay(1000);
    ctx->bl_en_gpio =
        devm_gpiod_get(ctx->dev, "bl-enable", GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->bl_en_gpio)) {
        dev_info(ctx->dev, "%s: cannot get bl_en_gpio %ld\n",
            __func__, PTR_ERR(ctx->bl_en_gpio));
        return PTR_ERR(ctx->bl_en_gpio);
    }
    gpiod_set_value(ctx->bl_en_gpio, 1);
    devm_gpiod_put(ctx->dev, ctx->bl_en_gpio);

#ifndef BYPASSI2C
     udelay(2000);
    _lcm_i2c_write_bytes(0x02, 0x22);
    _lcm_i2c_write_bytes(0x03, 0xCD);
    _lcm_i2c_write_bytes(0x0A, 0x11);
    _lcm_i2c_write_bytes(0x0C, 0x2E);
    _lcm_i2c_write_bytes(0x0D, 0x26);//5.9v
    _lcm_i2c_write_bytes(0x0E, 0x26);
    _lcm_i2c_write_bytes(0x10, 0xC0);
    _lcm_i2c_write_bytes(0x11, 0xF6);
    _lcm_i2c_write_bytes(0x15, 0xB1);
    _lcm_i2c_write_bytes(0x04, 0x00);
    _lcm_i2c_write_bytes(0x05, 0x00);
    _lcm_i2c_write_bytes(0x08, 0x5F);
    _lcm_i2c_write_bytes(0x09, 0x99);
#endif

    udelay(2000);
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

    udelay(10 * 1000);
    csot_panel_init(ctx);

    ret = ctx->error;
    if (ret < 0)
        csot_unprepare(panel);

    ctx->prepared = true;

#if defined(CONFIG_MTK_PANEL_EXT)
    mtk_panel_tch_rst(panel);
#endif
#ifdef PANEL_SUPPORT_READBACK
    csot_panel_get_data(ctx);
#endif
    pr_info("%s -\n", __func__);

    return ret;
}

static int csot_enable(struct drm_panel *panel)
{
    struct csot *ctx = panel_to_lcm(panel);

    if (ctx->enabled)
        return 0;

    if (ctx->backlight) {
        ctx->backlight->props.power = FB_BLANK_UNBLANK;
        backlight_update_status(ctx->backlight);
    }

    ctx->enabled = true;

    return 0;
}

#define HFP (80)
#define HSA (4)
#define HBP (80)
#define VFP_60 (1115)
#define VFP_90 (30)
#define VSA (2)
#define VBP (138)
#define VAC (2000)
#define HAC (1200)
#define FPS (90)

static const struct drm_display_mode default_mode = {
    .clock = 266389,
    .hdisplay = HAC,
    .hsync_start = HAC + HFP,
    .hsync_end = HAC + HFP + HSA,
    .htotal = HAC + HFP + HSA + HBP,
    .vdisplay = VAC,
    .vsync_start = VAC + VFP_60,
    .vsync_end = VAC + VFP_60 + VSA,
    .vtotal = VAC + VFP_60 + VSA + VBP,
};

static const struct drm_display_mode performance_mode = {
    .clock = 266389,
    .hdisplay = HAC,
    .hsync_start = HAC + HFP,
    .hsync_end = HAC + HFP + HSA,
    .htotal = HAC + HFP + HSA + HBP,
    .vdisplay = VAC,
    .vsync_start = VAC + VFP_90,
    .vsync_end = VAC + VFP_90 + VSA,
    .vtotal = VAC + VFP_90 + VSA + VBP,
};


#if defined(CONFIG_MTK_PANEL_EXT)
static int panel_ext_reset(struct drm_panel *panel, int on)
{
    struct csot *ctx = panel_to_lcm(panel);

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
    struct csot *ctx = panel_to_lcm(panel);
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

static int csot_setbacklight_cmdq(void *dsi, dcs_write_gce cb,
    void *handle, unsigned int level)
{
	int level_l, level_h, level_8bit, level_1;
	struct i2c_client *client = _lcm_i2c_client;
	char bl_l[2] = { 0x04, 0x00 };
	char bl_h[2] = { 0x05, 0x00 };
	int ret = 0;

	level_8bit = ((level*255)/2047);

	level_1 = backlight_i2c_map[level_8bit];

	pr_info("%s: level=%d level_8bit=%d level_1=%d \n", __func__,level, level_8bit,level_1);

	level_h = (level_1 >> BRIGHTNESS_HIGN_OFFSET) & BRIGHTNESS_HIGN_MASK;
	level_l = (level_1 >> BRIGHTNESS_LOW_OFFSET) & BRIGHTNESS_LOW_MASK;

	bl_l[1] = level_l;
	bl_h[1] = level_h;

	ret = i2c_master_send(client, bl_l, 2);
	ret = i2c_master_send(client, bl_h, 2);

	if (ret < 0)
		pr_info("ERROR %d!! i2c write data fail 0x%0x, 0x%0x, 0x%0x, 0x%0x!!\n",
				ret, bl_l[0], bl_l[1], bl_h[0], bl_h[1]);

    return 0;
}

static struct mtk_panel_params ext_params = {
    .pll_clk = 366,
    .data_rate_khz = 732732,
    .rotate = MTK_PANEL_ROTATE_180,
    .cust_esd_check = 0,
    .esd_check_enable = 1,
    .output_mode = MTK_PANEL_DSC_SINGLE_PORT,
    .physical_width_um = 149616,
    .physical_height_um = 249360,
    .dsc_params = {
        .enable = 1,
        .ver = 18,
        .slice_mode = 1,
        .rgb_swap = 0,
        .dsc_cfg = 34,
        .rct_on = 1,
        .bit_per_channel = 8,
        .dsc_line_buf_depth = 9,
        .bp_enable = 1,
        .bit_per_pixel = 128,
        .pic_height = 2000,
        .pic_width = 1200,
        .slice_height = 20,
        .slice_width = 600,
        .chunk_size = 600,
        .xmit_delay = 512,
        .dec_delay = 581,
        .scale_value = 32,
        .increment_interval = 492,
        .decrement_interval = 8,
        .line_bpg_offset = 13,
        .nfl_bpg_offset = 1402,
        .slice_bpg_offset = 1172,
        .initial_offset = 6144,
        .final_offset = 4336,
        .flatness_minqp = 3,
        .flatness_maxqp = 12,
        .rc_model_size = 8192,
        .rc_edge_factor = 6,
        .rc_quant_incr_limit0 = 11,
        .rc_quant_incr_limit1 = 11,
        .rc_tgt_offset_hi = 3,
        .rc_tgt_offset_lo = 3,
        .rc_buf_thresh[0] = 14,
        .rc_buf_thresh[1] = 28,
        .rc_buf_thresh[2] = 42,
        .rc_buf_thresh[3] = 56,
        .rc_buf_thresh[4] = 70,
        .rc_buf_thresh[5] = 84,
        .rc_buf_thresh[6] = 98,
        .rc_buf_thresh[7] = 105,
        .rc_buf_thresh[8] = 112,
        .rc_buf_thresh[9] = 119,
        .rc_buf_thresh[10] = 121,
        .rc_buf_thresh[11] = 123,
        .rc_buf_thresh[12] = 125,
        .rc_buf_thresh[13] = 126,
        .rc_range_parameters[0].range_min_qp = 0,
        .rc_range_parameters[0].range_max_qp = 4,
        .rc_range_parameters[0].range_bpg_offset = 2,
        .rc_range_parameters[1].range_min_qp = 0,
        .rc_range_parameters[1].range_max_qp = 4,
        .rc_range_parameters[1].range_bpg_offset = 0,
        .rc_range_parameters[2].range_min_qp = 1,
        .rc_range_parameters[2].range_max_qp = 5,
        .rc_range_parameters[2].range_bpg_offset = 0,
        .rc_range_parameters[3].range_min_qp = 1,
        .rc_range_parameters[3].range_max_qp = 6,
        .rc_range_parameters[3].range_bpg_offset = -2,
        .rc_range_parameters[4].range_min_qp = 3,
        .rc_range_parameters[4].range_max_qp = 7,
        .rc_range_parameters[4].range_bpg_offset = -4,
        .rc_range_parameters[5].range_min_qp = 3,
        .rc_range_parameters[5].range_max_qp = 7,
        .rc_range_parameters[5].range_bpg_offset = -6,
        .rc_range_parameters[6].range_min_qp = 3,
        .rc_range_parameters[6].range_max_qp = 7,
        .rc_range_parameters[6].range_bpg_offset = -8,
        .rc_range_parameters[7].range_min_qp = 3,
        .rc_range_parameters[7].range_max_qp = 8,
        .rc_range_parameters[7].range_bpg_offset = -8,
        .rc_range_parameters[8].range_min_qp = 3,
        .rc_range_parameters[8].range_max_qp = 9,
        .rc_range_parameters[8].range_bpg_offset = -8,
        .rc_range_parameters[9].range_min_qp = 3,
        .rc_range_parameters[9].range_max_qp = 10,
        .rc_range_parameters[9].range_bpg_offset = -10,
        .rc_range_parameters[10].range_min_qp = 5,
        .rc_range_parameters[10].range_max_qp = 10,
        .rc_range_parameters[10].range_bpg_offset = -10,
        .rc_range_parameters[11].range_min_qp = 5,
        .rc_range_parameters[11].range_max_qp = 11,
        .rc_range_parameters[11].range_bpg_offset = -12,
        .rc_range_parameters[12].range_min_qp = 5,
        .rc_range_parameters[12].range_max_qp = 11,
        .rc_range_parameters[12].range_bpg_offset = -12,
        .rc_range_parameters[13].range_min_qp = 9,
        .rc_range_parameters[13].range_max_qp = 12,
        .rc_range_parameters[13].range_bpg_offset = -12,
        .rc_range_parameters[14].range_min_qp = 12,
        .rc_range_parameters[14].range_max_qp = 13,
        .rc_range_parameters[14].range_bpg_offset = -12
        },
};

static struct mtk_panel_params ext_params_90hz = {
    .pll_clk = 366,
    .data_rate_khz = 732732,
    .rotate = MTK_PANEL_ROTATE_180,
    .cust_esd_check = 0,
    .esd_check_enable = 1,
    .output_mode = MTK_PANEL_DSC_SINGLE_PORT,
    .physical_width_um = 149616,
    .physical_height_um = 249360,
    .dsc_params = {
        .enable = 1,
        .ver = 18,
        .slice_mode = 1,
        .rgb_swap = 0,
        .dsc_cfg = 34,
        .rct_on = 1,
        .bit_per_channel = 8,
        .dsc_line_buf_depth = 9,
        .bp_enable = 1,
        .bit_per_pixel = 128,
        .pic_height = 2000,
        .pic_width = 1200,
        .slice_height = 20,
        .slice_width = 600,
        .chunk_size = 600,
        .xmit_delay = 512,
        .dec_delay = 581,
        .scale_value = 32,
        .increment_interval = 492,
        .decrement_interval = 8,
        .line_bpg_offset = 13,
        .nfl_bpg_offset = 1402,
        .slice_bpg_offset = 1172,
        .initial_offset = 6144,
        .final_offset = 4336,
        .flatness_minqp = 3,
        .flatness_maxqp = 12,
        .rc_model_size = 8192,
        .rc_edge_factor = 6,
        .rc_quant_incr_limit0 = 11,
        .rc_quant_incr_limit1 = 11,
        .rc_tgt_offset_hi = 3,
        .rc_tgt_offset_lo = 3,
        .rc_buf_thresh[0] = 14,
        .rc_buf_thresh[1] = 28,
        .rc_buf_thresh[2] = 42,
        .rc_buf_thresh[3] = 56,
        .rc_buf_thresh[4] = 70,
        .rc_buf_thresh[5] = 84,
        .rc_buf_thresh[6] = 98,
        .rc_buf_thresh[7] = 105,
        .rc_buf_thresh[8] = 112,
        .rc_buf_thresh[9] = 119,
        .rc_buf_thresh[10] = 121,
        .rc_buf_thresh[11] = 123,
        .rc_buf_thresh[12] = 125,
        .rc_buf_thresh[13] = 126,
        .rc_range_parameters[0].range_min_qp = 0,
        .rc_range_parameters[0].range_max_qp = 4,
        .rc_range_parameters[0].range_bpg_offset = 2,
        .rc_range_parameters[1].range_min_qp = 0,
        .rc_range_parameters[1].range_max_qp = 4,
        .rc_range_parameters[1].range_bpg_offset = 0,
        .rc_range_parameters[2].range_min_qp = 1,
        .rc_range_parameters[2].range_max_qp = 5,
        .rc_range_parameters[2].range_bpg_offset = 0,
        .rc_range_parameters[3].range_min_qp = 1,
        .rc_range_parameters[3].range_max_qp = 6,
        .rc_range_parameters[3].range_bpg_offset = -2,
        .rc_range_parameters[4].range_min_qp = 3,
        .rc_range_parameters[4].range_max_qp = 7,
        .rc_range_parameters[4].range_bpg_offset = -4,
        .rc_range_parameters[5].range_min_qp = 3,
        .rc_range_parameters[5].range_max_qp = 7,
        .rc_range_parameters[5].range_bpg_offset = -6,
        .rc_range_parameters[6].range_min_qp = 3,
        .rc_range_parameters[6].range_max_qp = 7,
        .rc_range_parameters[6].range_bpg_offset = -8,
        .rc_range_parameters[7].range_min_qp = 3,
        .rc_range_parameters[7].range_max_qp = 8,
        .rc_range_parameters[7].range_bpg_offset = -8,
        .rc_range_parameters[8].range_min_qp = 3,
        .rc_range_parameters[8].range_max_qp = 9,
        .rc_range_parameters[8].range_bpg_offset = -8,
        .rc_range_parameters[9].range_min_qp = 3,
        .rc_range_parameters[9].range_max_qp = 10,
        .rc_range_parameters[9].range_bpg_offset = -10,
        .rc_range_parameters[10].range_min_qp = 5,
        .rc_range_parameters[10].range_max_qp = 10,
        .rc_range_parameters[10].range_bpg_offset = -10,
        .rc_range_parameters[11].range_min_qp = 5,
        .rc_range_parameters[11].range_max_qp = 11,
        .rc_range_parameters[11].range_bpg_offset = -12,
        .rc_range_parameters[12].range_min_qp = 5,
        .rc_range_parameters[12].range_max_qp = 11,
        .rc_range_parameters[12].range_bpg_offset = -12,
        .rc_range_parameters[13].range_min_qp = 9,
        .rc_range_parameters[13].range_max_qp = 12,
        .rc_range_parameters[13].range_bpg_offset = -12,
        .rc_range_parameters[14].range_min_qp = 12,
        .rc_range_parameters[14].range_max_qp = 13,
        .rc_range_parameters[14].range_bpg_offset = -12
        },
};

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
    else if (drm_mode_vrefresh(m) == 90)
        ext->params = &ext_params_90hz;
    else
        ret = 1;

    return ret;
}

static struct mtk_panel_funcs ext_funcs = {
    .reset = panel_ext_reset,
    .set_backlight_cmdq = csot_setbacklight_cmdq,
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

static int csot_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
    struct drm_display_mode *mode;
    struct drm_display_mode *mode2;

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

  mode2 = drm_mode_duplicate(connector->dev, &performance_mode);
    if (!mode2) {
        dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
            performance_mode.hdisplay, performance_mode.vdisplay,
            drm_mode_vrefresh(&performance_mode));
        return -ENOMEM;
    }

    drm_mode_set_name(mode2);
    mode2->type = DRM_MODE_TYPE_DRIVER;
    drm_mode_probed_add(connector, mode2);

    connector->display_info.width_mm = 68;
    connector->display_info.height_mm = 152;

    return 1;
}

static const struct drm_panel_funcs csot_drm_funcs = {
    .disable = csot_disable,
    .unprepare = csot_unprepare,
    .prepare = csot_prepare,
    .enable = csot_enable,
    .get_modes = csot_get_modes,
};


static int csot_probe(struct mipi_dsi_device *dsi)
{
    struct device *dev = &dsi->dev;
    struct csot *ctx;
    struct device_node *backlight;
    int ret;
    struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;

    pr_info("kexin ************ %s ****************\n", __func__);
    pr_info("nt36523n %s --- begin\n", __func__);
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

    ctx = devm_kzalloc(dev, sizeof(struct csot), GFP_KERNEL);
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

    ctx->prepared = true;
    ctx->enabled = true;

    drm_panel_init(&ctx->panel, dev, &csot_drm_funcs, DRM_MODE_CONNECTOR_DSI);
    ctx->panel.dev = dev;
    ctx->panel.funcs = &csot_drm_funcs;

    drm_panel_add(&ctx->panel);

    ret = mipi_dsi_attach(dsi);
    if (ret < 0)
        drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
    //mtk_panel_tch_handle_reg(&ctx->panel);
    ret = mtk_panel_ext_create(dev, &ext_params, &ext_funcs, &ctx->panel);
    if (ret < 0)
        return ret;
#endif

#if IS_ENABLED(CONFIG_TINNO_DEVINFO)
    FULL_PRODUCT_DEVICE_INFO(ID_LCD, "CSOT-NT36523N-VDO");
#endif
#ifndef BYPASSI2C
    i2c_add_driver(&_lcm_i2c_driver);
#endif

    pr_info("nt36523n %s --- end\n", __func__);
    return ret;
}

static int csot_remove(struct mipi_dsi_device *dsi)
{
    struct csot *ctx = mipi_dsi_get_drvdata(dsi);

    mipi_dsi_detach(dsi);
    drm_panel_remove(&ctx->panel);
#ifndef BYPASSI2C
    i2c_del_driver(&_lcm_i2c_driver);
#endif
    return 0;
}

static const struct of_device_id csot_of_match[] = {
    { .compatible = "csot,nt36523n,vdo", },
    { }
};

MODULE_DEVICE_TABLE(of, csot_of_match);

static struct mipi_dsi_driver csot_driver = {
    .probe = csot_probe,
    .remove = csot_remove,
    .driver = {
        .name = "nt36523n_dsi_vdo_csot",
        .owner = THIS_MODULE,
        .of_match_table = csot_of_match,
    },
};

module_mipi_dsi_driver(csot_driver);

MODULE_AUTHOR("Ning Feng <Ning.Feng@mediatek.com>");
MODULE_DESCRIPTION("CSOT NT36523N VDO LCD Panel Driver");
MODULE_LICENSE("GPL v2");
