/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LENOVO_SM8850_PANEL_POWER_H
#define LENOVO_SM8850_PANEL_POWER_H

struct dsi_panel;

struct lenovo_panel_power_ops {
	int (*before_reset)(struct dsi_panel *panel);
	void (*after_reset)(struct dsi_panel *panel);
	bool (*prepare_power_off)(struct dsi_panel *panel);
	void (*after_display_disable)(struct dsi_panel *panel);
	void (*after_reset_low)(struct dsi_panel *panel);
	void (*disable_bias)(struct dsi_panel *panel);
	int (*after_regulators_off)(struct dsi_panel *panel, int rc);
};

const struct lenovo_panel_power_ops *lenovo_panel_lcd_power_ops(void);

#ifdef CONFIG_DRM_MSM_LENOVO_DISPLAY
int lenovo_panel_oled_power_on(struct dsi_panel *panel, bool is_cont_splash);
int lenovo_panel_oled_power_off(struct dsi_panel *panel);
#endif

#endif
