/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
*/

#include <linux/backlight.h>
#include <drm/drmP.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mtk_panel_ext.h"
#include "../mediatek/mtk_log.h"
#include "../mediatek/mtk_drm_graphics_base.h"
#endif

#ifdef CONFIG_MTK_ROUND_CORNER_SUPPORT
#include "../mediatek/mtk_corner_pattern/mtk_data_hw_roundedpattern.h"
#endif

struct lcm {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *bias_pos, *bias_neg;

	bool prepared;
	bool enabled;

	int error;
};

#define lcm_dcs_write_seq(ctx, seq...) \
({\
	const u8 d[] = { seq };\
	BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64, "DCS sequence too big for stack");\
	lcm_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

#define lcm_dcs_write_seq_static(ctx, seq...) \
({\
	static const u8 d[] = { seq };\
	lcm_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

static inline struct lcm *panel_to_lcm(struct drm_panel *panel)
{
	return container_of(panel, struct lcm, panel);
}

static void lcm_dcs_write(struct lcm *ctx, const void *data, size_t len)
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
		dev_err(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}

#ifdef PANEL_SUPPORT_READBACK
static int lcm_dcs_read(struct lcm *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return 0;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %d reading dcs seq:(%#x)\n", ret, cmd);
		ctx->error = ret;
	}

	return ret;
}

static void lcm_panel_get_data(struct lcm *ctx)
{
	u8 buffer[3] = {0};
	static int ret;

	if (ret == 0) {
		ret = lcm_dcs_read(ctx,  0x0A, buffer, 1);
		dev_info(ctx->dev, "return %d data(0x%08x) to dsi engine\n",
			 ret, buffer[0] | (buffer[1] << 8));
	}
}
#endif

#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
static struct regulator *disp_bias_pos;
static struct regulator *disp_bias_neg;


static int lcm_panel_bias_regulator_init(void)
{
	static int regulator_inited;
	int ret = 0;

	if (regulator_inited)
		return ret;

	/* please only get regulator once in a driver */
	disp_bias_pos = regulator_get(NULL, "dsv_pos");
	if (IS_ERR(disp_bias_pos)) { /* handle return value */
		ret = PTR_ERR(disp_bias_pos);
		pr_err("get dsv_pos fail, error: %d\n", ret);
		return ret;
	}

	disp_bias_neg = regulator_get(NULL, "dsv_neg");
	if (IS_ERR(disp_bias_neg)) { /* handle return value */
		ret = PTR_ERR(disp_bias_neg);
		pr_err("get dsv_neg fail, error: %d\n", ret);
		return ret;
	}

	regulator_inited = 1;
	return ret; /* must be 0 */

}

static int lcm_panel_bias_enable(void)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_bias_regulator_init();

	/* set voltage with min & max*/
	ret = regulator_set_voltage(disp_bias_pos, 5400000, 5400000);
	if (ret < 0)
		pr_err("set voltage disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_set_voltage(disp_bias_neg, 5400000, 5400000);
	if (ret < 0)
		pr_err("set voltage disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	/* enable regulator */
	ret = regulator_enable(disp_bias_pos);
	if (ret < 0)
		pr_err("enable regulator disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_enable(disp_bias_neg);
	if (ret < 0)
		pr_err("enable regulator disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}

static int lcm_panel_bias_disable(void)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_bias_regulator_init();

	ret = regulator_disable(disp_bias_neg);
	if (ret < 0)
		pr_err("disable regulator disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_disable(disp_bias_pos);
	if (ret < 0)
		pr_err("disable regulator disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}
#endif

static void lcm_panel_init(struct lcm *ctx)
{
	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return;
	}
	gpiod_set_value(ctx->reset_gpio, 0);
	udelay(15 * 1000);
	gpiod_set_value(ctx->reset_gpio, 1);
	udelay(1 * 1000);
	gpiod_set_value(ctx->reset_gpio, 0);
	udelay(10 * 1000);
	gpiod_set_value(ctx->reset_gpio, 1);
	udelay(10 * 1000);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	lcm_dcs_write_seq_static(ctx, 0xB0, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xB6, 0x30, 0x6B, 0x00, 0x06, 0x03,
		0x0A, 0x13, 0x1A, 0x6C, 0x18, 0x19, 0x02);
	lcm_dcs_write_seq_static(ctx, 0xB7, 0x71, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xB8, 0x57, 0x3d, 0x19, 0xbe, 0x1e,
		0x0a, 0x0a);
	lcm_dcs_write_seq_static(ctx, 0xB9, 0x6f, 0x3d, 0x28, 0xbe, 0x3c,
		0x14, 0x0a);
	lcm_dcs_write_seq_static(ctx, 0xBA, 0xb5, 0x33, 0x41, 0xbe, 0x64,
		0x23, 0x0a);
	lcm_dcs_write_seq_static(ctx, 0xBB, 0x44, 0x26, 0xC3, 0x1F, 0x19,
		0x06, 0x03, 0xC0, 0x00, 0x00, 0x10);
	lcm_dcs_write_seq_static(ctx, 0xBC, 0x32, 0x4C, 0xC3, 0x52, 0x32,
		0x1F, 0x03, 0xF2, 0x00, 0x00, 0x13);
	lcm_dcs_write_seq_static(ctx, 0xBD, 0x24, 0x68, 0xC3, 0xAA, 0x3F,
		0x32, 0x03, 0xFF, 0x00, 0x00, 0x25);
	lcm_dcs_write_seq_static(ctx, 0xBE, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xC0, 0x00, 0xDC, 0x00, 0xDC, 0x13,
		0x08, 0xE8, 0x00, 0x08, 0x00, 0x03, 0x78);
	lcm_dcs_write_seq_static(ctx, 0xC1, 0x30, 0x00, 0x00, 0x11, 0x11,
		0x00, 0x00, 0x00, 0x22, 0x00, 0x05, 0x20, 0xFA, 0x00, 0x08,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xC2, 0x06, 0xE0, 0x6E, 0x01, 0x03,
		0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00,
		0x00, 0x04, 0xA0, 0xC9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x48, 0xEB, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x11, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDC, 0x00, 0x00, 0x00,
		0x00, 0x04, 0x00, 0x08, 0xEF, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0xaa, 0xaa,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xC4, 0x00, 0x4c, 0x00, 0x3f, 0x00,
		0x83, 0x00, 0x00, 0x87, 0x86, 0x85, 0x84, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x61, 0x5d, 0x5f, 0x00, 0x5e, 0x60, 0x62, 0x00,
		0x00, 0x00, 0x02, 0x00, 0x83, 0x00, 0x00, 0x87, 0x86, 0x85,
		0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0x5d, 0x5f, 0x00,
		0x5e, 0x60, 0x62, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
		0x0f, 0x0e, 0x00, 0x0f, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x0f, 0xee, 0x00, 0x0f, 0xee, 0x00, 0x00, 0xe0,
		0x00, 0x00, 0xe0, 0x0e, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00,
		0x00, 0x00, 0x00, 0xff, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xC5, 0x08, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xC6, 0x02, 0x0a, 0x08, 0xfc, 0xff,
		0xff, 0xff, 0x00, 0x00, 0x13, 0x01, 0xf0, 0x0c, 0x06, 0x01,
		0x43, 0x43, 0x43, 0x00, 0x00, 0x00, 0x01, 0x77, 0x09, 0x28,
		0x28, 0x06, 0x01, 0x43, 0x43, 0x43, 0x00, 0x00, 0x00, 0x01,
		0x61, 0x00, 0x00, 0x00, 0x1c, 0x01, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x20,
		0x00, 0x00);


	lcm_dcs_write_seq_static(ctx, 0xC7, 0x00, 0x00, 0x00, 0xe0, 0x01,
		0xE9, 0x02, 0x7e, 0x02, 0x05, 0x02, 0x90, 0x02, 0xf6, 0x02,
		0x40, 0x02, 0x5C, 0x02, 0x77, 0x02, 0xC8, 0x02, 0x1b, 0x02,
		0x5b, 0x02, 0xBd, 0x02, 0x27, 0x02, 0xc3, 0x03, 0x54, 0x03,
		0xd8, 0x03, 0xff, 0x00, 0x00, 0x00, 0xe0, 0x01, 0xE9, 0x02,
		0x7e, 0x02, 0x05, 0x02, 0x90, 0x02, 0xf6, 0x02, 0x40, 0x02,
		0x5C, 0x02, 0x77, 0x02, 0xC8, 0x02, 0x1b, 0x02, 0x5b, 0x02,
		0xBd, 0x02, 0x27, 0x02, 0xc3, 0x03, 0x54, 0x03, 0xd8, 0x03,
		0xff);
	lcm_dcs_write_seq_static(ctx, 0xC8, 0x41, 0x00, 0xff, 0xfa, 0x00,
		0xff, 0x00, 0x00, 0xfe, 0xf6, 0xfe, 0xe9, 0x00, 0x00, 0xff,
		0xf7, 0xfb, 0xe1, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
		0x00, 0xff, 0xfa, 0x00, 0xff, 0x00, 0xfe, 0xf6, 0xfe, 0xe9,
		0x00, 0xff, 0xf7, 0xfb, 0xe1, 0x00, 0x00, 0x00, 0x00, 0xff,
		0x00, 0xff, 0xfa, 0x00, 0xff, 0x00, 0xfe, 0xf6, 0xfe, 0xe9,
		0x00, 0xff, 0xf7, 0xfb, 0xe1, 0x00, 0x00, 0x00, 0x00, 0xff);
	lcm_dcs_write_seq_static(ctx, 0xC9, 0x00, 0xff, 0xfa, 0x00, 0xff,
		0x00, 0x00, 0xfe, 0xf6, 0xfe, 0xe9, 0x00, 0x00, 0xff, 0xf7,
		0xfb, 0xe1, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xCA, 0x1c, 0xfc, 0xfc, 0xfc, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xCC, 0x00, 0x00, 0x4d, 0x8b, 0x55,
		0x4d, 0x8b, 0xaa, 0x4d, 0x8b);
	lcm_dcs_write_seq_static(ctx, 0xCD, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xCE, 0x5d, 0x40, 0x49, 0x53, 0x59,
		0x5e, 0x63, 0x68, 0x6e, 0x74, 0x7e, 0x8a, 0x98, 0xa8, 0xbb,
		0xd0, 0xe7, 0xff, 0x04, 0x00, 0x04, 0x04, 0x42, 0x04, 0x69,
		0x5a, 0x40, 0x11, 0xf4, 0x00, 0x00, 0x84, 0xfa, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xCF, 0x00, 0x00, 0x80, 0x46, 0x61,
		0x00);
	lcm_dcs_write_seq_static(ctx, 0xD0, 0xf6, 0x95, 0x11, 0xb1, 0x55,
		0xcf, 0x00, 0xf6, 0xd3, 0x11, 0xf0, 0x01, 0x12, 0xcf, 0x02,
		0x20, 0x11);
	lcm_dcs_write_seq_static(ctx, 0xD1, 0xd3, 0xd3, 0x33, 0x33, 0x07,
		0x03, 0x3b, 0x33, 0x77, 0x37, 0x77, 0x37, 0x35, 0x77, 0x07,
		0x77, 0xf7, 0x33, 0x73, 0x07, 0x33, 0x33, 0x03, 0x33, 0x1b,
		0x03, 0x32, 0x3d, 0x0a, 0x30, 0x13, 0x13, 0x20, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xD2, 0x00, 0x00, 0x07);
	lcm_dcs_write_seq_static(ctx, 0xD3, 0x03, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x0f, 0x00, 0x57, 0x00, 0x00, 0x32, 0x00, 0x00,
		0x1a, 0x70, 0x01, 0x19, 0x80, 0x01, 0x01, 0xf0, 0x02, 0x00,
		0xe0, 0x06, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7,
		0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff,
		0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff,
		0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7,
		0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff,
		0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff,
		0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7,
		0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff,
		0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff,
		0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7,
		0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff,
		0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff,
		0xf7, 0xff, 0xff, 0xf7, 0xff, 0xff, 0xf7, 0xff);


	lcm_dcs_write_seq_static(ctx, 0xE5, 0x03, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xD5, 0x02, 0x42, 0x02, 0x42, 0x02,
		0xdc, 0x02, 0xdc);
	lcm_dcs_write_seq_static(ctx, 0xD6, 0xc0);

	lcm_dcs_write_seq_static(ctx, 0xD7, 0x21, 0x10, 0x52, 0x52, 0x00,
		0xB6, 0x04, 0xFD, 0x00, 0xB6, 0x04, 0xFD, 0x00, 0x82, 0x80,
		0x83, 0x84, 0x85, 0x83, 0x80, 0x84, 0x45, 0x85, 0x85, 0x85,
		0x87, 0x04, 0x06, 0x02, 0x04, 0x04, 0x07, 0x10, 0x0C, 0x0B,
		0x0A, 0x0A, 0x07, 0x06, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xDD, 0x30, 0x06, 0x23, 0x65);
	lcm_dcs_write_seq_static(ctx, 0xDE, 0x00, 0x00, 0x00, 0x0f, 0xff,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10);
	lcm_dcs_write_seq_static(ctx, 0xE3, 0xff);
	lcm_dcs_write_seq_static(ctx, 0xE6, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00);
	lcm_dcs_write_seq_static(ctx, 0xE7, 0x50, 0x04, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xE8, 0x00, 0x01, 0x23, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xEA, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xEB, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x11);
	lcm_dcs_write_seq_static(ctx, 0xEC, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xED, 0x01, 0x01, 0x02, 0x02, 0x02,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00,
		0x00, 0x00, 0x10, 0x00, 0x18, 0x00, 0x18, 0x00, 0xB0, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xDA, 0x10, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xEE, 0x03, 0x0F, 0x00, 0x00, 0x00,
		0x00, 0x40, 0x1F, 0x00, 0x00, 0x0F, 0xF2, 0x3F, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x09, 0x01, 0x8C, 0xD8,
		0xEF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50,
		0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00);
	lcm_dcs_write_seq_static(ctx, 0xEF, 0x00, 0x70, 0x4A, 0x08, 0xD0,
		0x00, 0x00, 0x00, 0x00, 0x3C, 0x3C, 0x3C, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x70, 0x4A, 0x08, 0xD0, 0x00, 0x00, 0x00,
		0x00, 0x3C, 0x3C, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x08, 0xEC,
		0x50, 0x10, 0x00, 0x10, 0x00, 0x0A, 0x0A, 0x00, 0x00, 0x00,
		0x00, 0x10, 0x0F, 0x00, 0x03, 0x51, 0x00, 0x50, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x03, 0x08, 0xEC);
	lcm_dcs_write_seq_static(ctx, 0xB0, 0x03);

	lcm_dcs_write_seq_static(ctx, 0xB0, 0x04);
	lcm_dcs_write_seq_static(ctx, 0xD6, 0x00);

	lcm_dcs_write_seq_static(ctx, 0x51, 0xff, 0xf0);
	lcm_dcs_write_seq_static(ctx, 0x53, 0x0c);

	lcm_dcs_write_seq_static(ctx, 0x55, 0x00);

	lcm_dcs_write_seq_static(ctx, 0x35, 0x00);

	lcm_dcs_write_seq_static(ctx, 0x35);

	lcm_dcs_write_seq_static(ctx, 0x29);
	msleep(200);
	lcm_dcs_write_seq_static(ctx, 0x11);

	msleep(100);
}

static int lcm_disable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	return 0;
}

static int lcm_unprepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (!ctx->prepared)
		return 0;

	lcm_dcs_write_seq_static(ctx, 0x28);
	msleep(50);
	lcm_dcs_write_seq_static(ctx, 0x10);
	msleep(150);

	ctx->error = 0;
	ctx->prepared = false;
#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
	lcm_panel_bias_disable();
#else
	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	gpiod_set_value(ctx->reset_gpio, 0);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);


	ctx->bias_neg = devm_gpiod_get_index(ctx->dev,
		"bias", 1, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_neg)) {
		dev_err(ctx->dev, "%s: cannot get bias_neg %ld\n",
			__func__, PTR_ERR(ctx->bias_neg));
		return PTR_ERR(ctx->bias_neg);
	}
	gpiod_set_value(ctx->bias_neg, 0);
	devm_gpiod_put(ctx->dev, ctx->bias_neg);

	udelay(1000);

	ctx->bias_pos = devm_gpiod_get_index(ctx->dev,
		"bias", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_pos)) {
		dev_err(ctx->dev, "%s: cannot get bias_pos %ld\n",
			__func__, PTR_ERR(ctx->bias_pos));
		return PTR_ERR(ctx->bias_pos);
	}
	gpiod_set_value(ctx->bias_pos, 0);
	devm_gpiod_put(ctx->dev, ctx->bias_pos);
#endif

	return 0;
}

static int lcm_prepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret;

	pr_info("%s\n", __func__);
	if (ctx->prepared)
		return 0;

#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
	lcm_panel_bias_enable();
#else
	ctx->bias_pos = devm_gpiod_get_index(ctx->dev,
		"bias", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_pos)) {
		dev_err(ctx->dev, "%s: cannot get bias_pos %ld\n",
			__func__, PTR_ERR(ctx->bias_pos));
		return PTR_ERR(ctx->bias_pos);
	}
	gpiod_set_value(ctx->bias_pos, 1);
	devm_gpiod_put(ctx->dev, ctx->bias_pos);

	udelay(2000);

	ctx->bias_neg = devm_gpiod_get_index(ctx->dev,
		"bias", 1, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_neg)) {
		dev_err(ctx->dev, "%s: cannot get bias_neg %ld\n",
			__func__, PTR_ERR(ctx->bias_neg));
		return PTR_ERR(ctx->bias_neg);
	}
	gpiod_set_value(ctx->bias_neg, 1);
	devm_gpiod_put(ctx->dev, ctx->bias_neg);
#endif

	lcm_panel_init(ctx);

	ret = ctx->error;
	if (ret < 0)
		lcm_unprepare(panel);

	ctx->prepared = true;

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_rst(panel);
#endif
#ifdef PANEL_SUPPORT_READBACK
	lcm_panel_get_data(ctx);
#endif

	return ret;
}

static int lcm_enable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;

	return 0;
}

#define HFP (24)
#define HSA (20)
#define HBP (20)
#define VFP (75)
#define VSA (2)
#define VBP (12)

static const struct drm_display_mode default_mode = {
	.clock = 163406,
	.hdisplay = 1080,
	.hsync_start = 1080 + HFP,
	.hsync_end = 1080 + HFP + HSA,
	.htotal = 1080 + HFP + HSA + HBP,
	.vdisplay = 2280,
	.vsync_start = 2280 + VFP,
	.vsync_end = 2280 + VFP + VSA,
	.vtotal = 2280 + VFP + VSA + VBP,
	.vrefresh = 60,
};

#if defined(CONFIG_MTK_PANEL_EXT)
static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct lcm *ctx = panel_to_lcm(panel);

	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	gpiod_set_value(ctx->reset_gpio, on);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	return 0;
}

static int panel_ata_check(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	unsigned char data[3] = {0x00, 0x00, 0x00};
	unsigned char id[3] = {0x00, 0x00, 0x00};
	ssize_t ret;

	ret = mipi_dsi_dcs_read(dsi, 0x4, data, 3);
	if (ret < 0) {
		pr_err("%s error\n", __func__);
		return 0;
	}

	DDPINFO("ATA read data %x %x %x\n", data[0], data[1], data[2]);

	if (data[0] == id[0] &&
			data[1] == id[1] &&
			data[2] == id[2])
		return 1;

	DDPINFO("ATA expect read data is %x %x %x\n",
			id[0], id[1], id[2]);

	return 0;
}

static int lcm_setbacklight_cmdq(void *dsi, dcs_write_gce cb,
	void *handle, unsigned int level)
{
	char bl_tb0[] = {0x51, 0xFF};

	bl_tb0[1] = level;

	if (!cb)
		return -1;

	cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));

	return 0;
}

static struct mtk_panel_params ext_params = {
	.pll_clk = 522,
	.vfp_low_power = 810,
	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x1c,
	},
	.lcm_color_mode = MTK_DRM_COLOR_MODE_DISPLAY_P3,

};

static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.set_backlight_cmdq = lcm_setbacklight_cmdq,
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

static int lcm_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(panel->drm->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(panel->connector, mode);

	panel->connector->display_info.width_mm = 64;
	panel->connector->display_info.height_mm = 129;

	return 1;
}

static const struct drm_panel_funcs lcm_drm_funcs = {
	.disable = lcm_disable,
	.unprepare = lcm_unprepare,
	.prepare = lcm_prepare,
	.enable = lcm_enable,
	.get_modes = lcm_get_modes,
};

static int lcm_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct lcm *ctx;
	struct device_node *backlight;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(struct lcm), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO
			 | MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET
			 | MIPI_DSI_CLOCK_NON_CONTINUOUS;

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "%s: cannot get reset-gpios %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	devm_gpiod_put(dev, ctx->reset_gpio);

	ctx->bias_pos = devm_gpiod_get_index(dev, "bias", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_pos)) {
		dev_err(dev, "%s: cannot get bias-pos 0 %ld\n",
			__func__, PTR_ERR(ctx->bias_pos));
		return PTR_ERR(ctx->bias_pos);
	}
	devm_gpiod_put(dev, ctx->bias_pos);

	ctx->bias_neg = devm_gpiod_get_index(dev, "bias", 1, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_neg)) {
		dev_err(dev, "%s: cannot get bias-neg 1 %ld\n",
			__func__, PTR_ERR(ctx->bias_neg));
		return PTR_ERR(ctx->bias_neg);
	}
	devm_gpiod_put(dev, ctx->bias_neg);

	ctx->prepared = true;
	ctx->enabled = true;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &lcm_drm_funcs;

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_handle_reg(&ctx->panel);
	ret = mtk_panel_ext_create(dev, &ext_params, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;
#endif

	pr_info("%s-\n", __func__);

	return ret;
}

static int lcm_remove(struct mipi_dsi_device *dsi)
{
	struct lcm *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id lcm_of_match[] = {
	{ .compatible = "truly,td4330,vdo-fake-wcg", },
	{ }
};

MODULE_DEVICE_TABLE(of, lcm_of_match);

static struct mipi_dsi_driver lcm_driver = {
	.probe = lcm_probe,
	.remove = lcm_remove,
	.driver = {
		.name = "panel-truly-td4330-vdo-fake-wcg",
		.owner = THIS_MODULE,
		.of_match_table = lcm_of_match,
	},
};

module_mipi_dsi_driver(lcm_driver);

MODULE_AUTHOR("Yi-Lun Wang <Yi-Lun.Wang@mediatek.com>");
MODULE_DESCRIPTION("truly td4330 VDO LCD FAKE WCG Panel Driver");
MODULE_LICENSE("GPL v2");

