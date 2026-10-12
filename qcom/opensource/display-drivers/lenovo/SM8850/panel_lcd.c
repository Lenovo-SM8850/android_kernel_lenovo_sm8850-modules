// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lenovo Baldur LCD panel power hooks.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/pinctrl/consumer.h>

#include "../../msm/dsi/dsi_panel.h"
#include "../../msm/dsi/dsi_defs.h"
#include "../../msm/dsi/dsi_lenovo.h"
#include "panel_power.h"

static DECLARE_COMPLETION(load_fw_completion);

#if IS_ENABLED(CONFIG_DRM_MSM_LENOVO_BALDUR)
bool lenovo_lcd_wait_for_firmware(void)
{
	return wait_for_completion_timeout(&load_fw_completion,
					   msecs_to_jiffies(350));
}
#endif

static int lenovo_lcd_before_reset(struct dsi_panel *panel)
{
	int rc;

	usleep_range(2000, 2100);
	rc = bias_enable(true);
	if (rc)
		DSI_ERR("[%s] failed to set bias, rc=%d\n", panel->name, rc);
	usleep_range(5000, 5100);
	return rc;
}

static void lenovo_lcd_after_reset(struct dsi_panel *panel)
{
	nvt_update_firmware_work(&load_fw_completion);
}

static bool lenovo_lcd_prepare_power_off(struct dsi_panel *panel)
{
	init_completion(&load_fw_completion);
	usleep_range(10000, 10100);
	return nvt_gesture_flag;
}

static void lenovo_lcd_after_display_disable(struct dsi_panel *panel)
{
	usleep_range(15000, 15100);
}

static void lenovo_lcd_after_reset_low(struct dsi_panel *panel)
{
	usleep_range(5000, 5100);
}

static void lenovo_lcd_disable_bias(struct dsi_panel *panel)
{
	int rc = bias_enable(false);

	if (rc)
		DSI_ERR("[%s] failed to set bias, rc=%d\n", panel->name, rc);
}

static int lenovo_lcd_after_regulators_off(struct dsi_panel *panel, int rc)
{
	usleep_range(5000, 5100);
	if (!IS_ERR_OR_NULL(panel->pinctrl.pinctrl) &&
	    !IS_ERR_OR_NULL(panel->pinctrl.iovdd_disable)) {
		rc = pinctrl_select_state(panel->pinctrl.pinctrl,
					  panel->pinctrl.iovdd_disable);
		if (rc)
			DSI_ERR("[%s] failed to set iovdd_disable pinctrl, rc=%d\n",
				panel->name, rc);
	}
	return rc;
}

static const struct lenovo_panel_power_ops lenovo_lcd_power_ops = {
	.before_reset = lenovo_lcd_before_reset,
	.after_reset = lenovo_lcd_after_reset,
	.prepare_power_off = lenovo_lcd_prepare_power_off,
	.after_display_disable = lenovo_lcd_after_display_disable,
	.after_reset_low = lenovo_lcd_after_reset_low,
	.disable_bias = lenovo_lcd_disable_bias,
	.after_regulators_off = lenovo_lcd_after_regulators_off,
};

const struct lenovo_panel_power_ops *lenovo_panel_lcd_power_ops(void)
{
	return &lenovo_lcd_power_ops;
}
