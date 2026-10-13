// SPDX-License-Identifier: GPL-2.0-only
/* Lenovo LCD panel power hooks. */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kmod.h>
#include <linux/module.h>
#include <linux/lenovo_lcd_touch.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>

#include "../../msm/dsi/dsi_panel.h"
#include "../../msm/dsi/dsi_defs.h"
#include "../../msm/dsi/dsi_lenovo.h"
#include "panel_power.h"

static DECLARE_COMPLETION(load_fw_completion);
static DEFINE_MUTEX(touch_supplier_lock);
static const struct lenovo_lcd_touch_ops *registered_touch_ops;
static const struct lenovo_lcd_touch_ops *active_touch_ops;

int lenovo_lcd_register_touch(const struct lenovo_lcd_touch_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->owner || !ops->update_firmware || !ops->gesture_flag)
		return -EINVAL;
	mutex_lock(&touch_supplier_lock);
	if (registered_touch_ops)
		ret = -EBUSY;
	else
		registered_touch_ops = ops;
	mutex_unlock(&touch_supplier_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(lenovo_lcd_register_touch);

void lenovo_lcd_unregister_touch(const struct lenovo_lcd_touch_ops *ops)
{
	mutex_lock(&touch_supplier_lock);
	if (registered_touch_ops == ops)
		registered_touch_ops = NULL;
	if (active_touch_ops == ops) {
		active_touch_ops = NULL;
		module_put(ops->owner);
	}
	mutex_unlock(&touch_supplier_lock);
}
EXPORT_SYMBOL_GPL(lenovo_lcd_unregister_touch);

bool lenovo_lcd_gesture_enabled(void)
{
	const struct lenovo_lcd_touch_ops *ops;
	bool gesture;

	mutex_lock(&touch_supplier_lock);
	ops = active_touch_ops ?: registered_touch_ops;
	gesture = ops && READ_ONCE(*ops->gesture_flag);
	mutex_unlock(&touch_supplier_lock);
	return gesture;
}

static bool lenovo_lcd_touch_supplier_get(void)
{
	bool ready;

	mutex_lock(&touch_supplier_lock);
	ready = active_touch_ops != NULL;
	mutex_unlock(&touch_supplier_lock);
	if (!ready)
		request_module_nowait("nvt_36xxx");
	mutex_lock(&touch_supplier_lock);
	if (!active_touch_ops && registered_touch_ops &&
	    try_module_get(registered_touch_ops->owner))
		active_touch_ops = registered_touch_ops;
	ready = active_touch_ops != NULL;
	mutex_unlock(&touch_supplier_lock);
	return ready;
}

void lenovo_lcd_touch_supplier_put(void)
{
	mutex_lock(&touch_supplier_lock);
	if (active_touch_ops) {
		module_put(active_touch_ops->owner);
		active_touch_ops = NULL;
	}
	mutex_unlock(&touch_supplier_lock);
}
#if IS_ENABLED(CONFIG_DRM_MSM_LENOVO_DISPLAY)
bool lenovo_lcd_wait_for_firmware(void)
{
	return wait_for_completion_timeout(&load_fw_completion,
					   msecs_to_jiffies(350));
}
#endif

static int lenovo_lcd_before_reset(struct dsi_panel *panel)
{
	int rc;

	if (!lenovo_lcd_touch_supplier_get())
		return -EPROBE_DEFER;
	usleep_range(2000, 2100);
	rc = bias_enable(true);
	if (rc)
		DSI_ERR("[%s] failed to set bias, rc=%d\n", panel->name, rc);
	usleep_range(5000, 5100);
	return rc;
}

static void lenovo_lcd_after_reset(struct dsi_panel *panel)
{
	mutex_lock(&touch_supplier_lock);
	if (active_touch_ops)
		active_touch_ops->update_firmware(&load_fw_completion);
	mutex_unlock(&touch_supplier_lock);
}

static bool lenovo_lcd_prepare_power_off(struct dsi_panel *panel)
{
	bool gesture;
	init_completion(&load_fw_completion);
	usleep_range(10000, 10100);
	mutex_lock(&touch_supplier_lock);
	gesture = active_touch_ops && READ_ONCE(*active_touch_ops->gesture_flag);
	mutex_unlock(&touch_supplier_lock);
	return gesture;
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
