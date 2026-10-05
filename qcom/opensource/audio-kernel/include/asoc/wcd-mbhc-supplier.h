/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __WCD_MBHC_SUPPLIER_H
#define __WCD_MBHC_SUPPLIER_H

struct module;
struct notifier_block;
struct device_node;

struct wcd_mbhc_sub_fsa_ops {
	struct module *owner;
	int (*reg_notifier)(struct notifier_block *, struct device_node *);
	int (*unreg_notifier)(struct notifier_block *, struct device_node *);
};

int wcd_mbhc_register_sub_fsa(const struct wcd_mbhc_sub_fsa_ops *ops);
void wcd_mbhc_unregister_sub_fsa(const struct wcd_mbhc_sub_fsa_ops *ops);
#endif
