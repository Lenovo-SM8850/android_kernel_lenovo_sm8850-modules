/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LENOVO_PANEL_NOTIFIER_H
#define _LENOVO_PANEL_NOTIFIER_H
#include <linux/of.h>
#include <linux/soc/qcom/panel_event_notifier.h>

static inline struct drm_panel *lenovo_find_panel(struct device_node *node)
{
	struct device_node *panel_node, *display;
	struct drm_panel *panel = ERR_PTR(-EPROBE_DEFER);
	int index;

	for (index = 0; ; index++) {
		panel_node = of_parse_phandle(node, "panel", index);
		if (!panel_node)
			break;
		panel = of_drm_find_panel(panel_node);
		of_node_put(panel_node);
		if (!IS_ERR(panel))
			return panel;
	}
	display = of_find_node_by_name(NULL, "qcom,dsi-display-primary");
	if (!display)
		return panel;
	panel_node = of_parse_phandle(display, "qcom,dsi-panel", 0);
	of_node_put(display);
	if (panel_node) {
		panel = of_drm_find_panel(panel_node);
		of_node_put(panel_node);
	}
	return panel;
}
#endif
