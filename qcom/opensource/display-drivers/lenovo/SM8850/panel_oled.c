// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lenovo Wuji OLED panel power hooks.
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/string.h>

#include "../../msm/dsi/dsi_panel.h"
#include "../../msm/dsi/dsi_defs.h"
#include "panel_power.h"

#ifdef CONFIG_DRM_MSM_LENOVO_DISPLAY
static int lenovo_oled_pin(struct dsi_panel *panel, struct pinctrl_state *state)
{
	if (IS_ERR_OR_NULL(panel->pinctrl.pinctrl) || IS_ERR_OR_NULL(state))
		return 0;
	return pinctrl_select_state(panel->pinctrl.pinctrl, state);
}

static void lenovo_oled_disable_vreg(struct dsi_vreg *vreg, u32 min_voltage)
{
	if (vreg->pre_off_sleep)
		usleep_range(vreg->pre_off_sleep * 1000,
			     vreg->pre_off_sleep * 1000 + 10);
	(void)regulator_disable(vreg->vreg);
	if (vreg->post_off_sleep)
		usleep_range(vreg->post_off_sleep * 1000,
			     vreg->post_off_sleep * 1000 + 10);
	(void)regulator_set_load(vreg->vreg, vreg->disable_load);
	if (regulator_count_voltages(vreg->vreg) > 0)
		(void)regulator_set_voltage(vreg->vreg, min_voltage,
					   vreg->max_voltage);
}

static int lenovo_oled_enable_vreg(struct dsi_vreg *vreg, bool enable)
{
	int rc, num_of_v = 0;

	DSI_INFO("dsi pwr enable vregs count ldo stast name = %s\x20\n",
		 vreg->vreg_name);
	if (!enable) {
		DSI_INFO("dsi pwr disable\x20\n");
		lenovo_oled_disable_vreg(vreg, vreg->off_min_voltage);
		return 0;
	}

	DSI_INFO("dsi pwr enable start...\n");
	if (vreg->pre_on_sleep)
		usleep_range(vreg->pre_on_sleep * 1000,
			     vreg->pre_on_sleep * 1000 + 10);
	rc = regulator_set_load(vreg->vreg, vreg->enable_load);
	if (rc < 0) {
		DSI_ERR("Setting optimum mode failed for %s\n", vreg->vreg_name);
		goto error;
	}
	num_of_v = regulator_count_voltages(vreg->vreg);
	if (num_of_v > 0) {
		rc = regulator_set_voltage(vreg->vreg, vreg->min_voltage,
					   vreg->max_voltage);
		if (rc) {
			DSI_ERR("Set voltage(%s) fail, rc=%d\n", vreg->vreg_name, rc);
			goto error_disable_opt_mode;
		}
	}
	rc = regulator_enable(vreg->vreg);
	if (rc) {
		DSI_ERR("enable failed for %s, rc=%d\n", vreg->vreg_name, rc);
		goto error_disable_voltage;
	}
	if (vreg->post_on_sleep)
		usleep_range(vreg->post_on_sleep * 1000,
			     vreg->post_on_sleep * 1000 + 10);
	return 0;

error_disable_opt_mode:
	(void)regulator_set_load(vreg->vreg, vreg->disable_load);
error_disable_voltage:
	if (num_of_v > 0)
		(void)regulator_set_voltage(vreg->vreg, 0, vreg->max_voltage);
error:
	/* Stock unwinds the failing rail, not just earlier entries. */
	lenovo_oled_disable_vreg(vreg, 0);
	return rc;
}

static int lenovo_oled_supply(struct dsi_panel *panel, const char *name, bool enable)
{
	struct dsi_regulator_info *regs = &panel->power_info;
	int i, rc;

	if (!regs->count) {
		DSI_DEBUG("No valid regulators to enable\n");
		return 0;
	}
	if (!regs->vregs) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}
	DSI_INFO("Get device count ldo start ldo=%s count=%d\n", name, regs->count);
	for (i = 0; i < regs->count; i++) {
		if (strcmp(regs->vregs[i].vreg_name, name)) {
			DSI_ERR("Get name fail count ldo%s=name%s i=%d\n",
				name, regs->vregs[i].vreg_name, i);
			continue;
		}
		DSI_INFO("found regulators: vreg_name=%s index=%d\n",
			 regs->vregs[i].vreg_name, i);
		rc = lenovo_oled_enable_vreg(&regs->vregs[i], enable);
		if (rc) {
			if (enable)
				DSI_ERR("enable regulators failed ldo=%s vreg_name=%s count=%d\n",
					name, regs->vregs[i].vreg_name, i);
			else
				DSI_ERR("disable regulators failed ldo=%s vreg_name=%s count=%d\n",
					name, regs->vregs[i].vreg_name, i);
		}
		return rc;
	}
	return 0;
}

/* Stock sequences the board pins around individual rail transitions. */
int lenovo_panel_oled_power_off(struct dsi_panel *panel)
{
	int rc;

	DSI_INFO("dsi panel power off start ...\n");
	if (panel->skip_panel_off) {
		DSI_DEBUG("skip panel power off\n");
		return 0;
	}
	if (gpio_is_valid(panel->reset_config.disp_en_gpio))
		gpio_set_value(panel->reset_config.disp_en_gpio, 0);
	if (gpio_is_valid(panel->reset_config.reset_gpio) && !panel->reset_gpio_always_on)
		gpio_set_value(panel->reset_config.reset_gpio, 0);
	if (gpio_is_valid(panel->reset_config.lcd_mode_sel_gpio))
		gpio_set_value(panel->reset_config.lcd_mode_sel_gpio, 0);
	if (gpio_is_valid(panel->panel_test_gpio)) {
		rc = gpio_direction_input(panel->panel_test_gpio);
		if (rc)
			DSI_WARN("set dir for panel test gpio failed rc=%d\n", rc);
	}
	usleep_range(2000, 2100);
	rc = lenovo_oled_supply(panel, "vci", false);
	if (rc)
		DSI_ERR("hy [%s] failed to enable vregs, rc=%d ,vci=%s\n",
			panel->name, rc, "vci");
	usleep_range(2000, 2100);
	rc = lenovo_oled_pin(panel, panel->pinctrl.vo4_set[0]);
	if (rc)
		DSI_ERR("[%s] failed to set vo4_set_sleep pinctrl, rc=%d\n", panel->name, rc);
	usleep_range(2000, 2100);
	rc = lenovo_oled_pin(panel, panel->pinctrl.vo4_en[0]);
	if (rc)
		DSI_ERR("[%s] failed to set vo4_en_sleep pinctrl, rc=%d\n", panel->name, rc);
	usleep_range(2000, 2100);
	rc = lenovo_oled_pin(panel, panel->pinctrl.oled_nreset[0]);
	if (rc)
		DSI_ERR("[%s] failed to set oled_nreset_sleep pinctrl, rc=%d\n", panel->name, rc);
	usleep_range(1000, 1100);
	rc = lenovo_oled_supply(panel, "vddio", false);
	if (rc)
		DSI_ERR("hy [%s] failed to enable vregs, rc=%d ,vddio=%s\n",
			panel->name, rc, "vddio");
	usleep_range(2000, 2100);
	rc = dsi_panel_set_pinctrl_state(panel, false, false);
	if (rc)
		DSI_ERR("[%s] failed set pinctrl state, rc=%d\n", panel->name, rc);
	rc = dsi_pwr_enable_regulator(&panel->power_info, false);
	if (rc)
		DSI_ERR("[%s] failed to enable vregs, rc=%d\n", panel->name, rc);
	DSI_INFO("dsi panel power off end ...\n");
	return rc;
}

int lenovo_panel_oled_power_on(struct dsi_panel *panel, bool is_cont_splash)
{
	int rc;

	DSI_INFO("dsi panel power on start ...\n");
	rc = lenovo_oled_supply(panel, "vddio", true);
	if (rc) {
		DSI_ERR("[%s] failed to enable vregs, rc=%d ,vddio=%s\n",
			panel->name, rc, "vddio");
		return rc;
	}
	rc = dsi_panel_set_pinctrl_state(panel, true, is_cont_splash);
	if (rc) {
		DSI_ERR("[%s] failed to set pinctrl, rc=%d\n", panel->name, rc);
		goto error_disable_vregs;
	}
	usleep_range(1000, 1100);
	rc = lenovo_oled_pin(panel, panel->pinctrl.vo4_set[1]);
	if (rc)
		DSI_ERR("[%s] failed to set vo4_set_active pinctrl, rc=%d\n", panel->name, rc);
	rc = lenovo_oled_pin(panel, panel->pinctrl.oled_nreset[1]);
	if (rc)
		DSI_ERR("[%s] failed to set oled_nreset_active pinctrl, rc=%d\n", panel->name, rc);
	usleep_range(1000, 1100);
	rc = lenovo_oled_pin(panel, panel->pinctrl.vo4_en[1]);
	if (rc)
		DSI_ERR("[%s] failed to set vo4_en_active pinctrl, rc=%d\n", panel->name, rc);
	usleep_range(2000, 2100);
	rc = lenovo_oled_supply(panel, "vci", true);
	if (rc) {
		DSI_ERR("[%s] failed to enable vregs, rc=%d ,vci=%s\n", panel->name, rc, "vci");
		return rc;
	}
	usleep_range(2000, 2100);
	if (is_cont_splash && panel->disp_op == MSM_DISP_OP_HFI) {
		DSI_DEBUG("[%s] skipping panel reset in hfi path with cont-splash\n", panel->name);
	} else {
		rc = dsi_panel_reset(panel);
		if (rc) {
			DSI_ERR("[%s] failed to reset panel, rc=%d\n", panel->name, rc);
			goto error_disable_gpio;
		}
	}
	DSI_INFO("dsi panel power on end ...\n");
	return 0;

error_disable_gpio:
	if (gpio_is_valid(panel->reset_config.disp_en_gpio))
		gpio_set_value(panel->reset_config.disp_en_gpio, 0);
	if (gpio_is_valid(panel->bl_config.en_gpio))
		gpio_set_value(panel->bl_config.en_gpio, 0);
	(void)dsi_panel_set_pinctrl_state(panel, false, is_cont_splash);
error_disable_vregs:
	(void)lenovo_oled_supply(panel, "vci", false);
	(void)lenovo_oled_supply(panel, "vddio", false);
	return rc;
}
#endif
