/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_LENOVO_HAPTIC_H
#define _LINUX_LENOVO_HAPTIC_H
#include <linux/notifier.h>

/* TB323 wire-independent notifier contract: duration in ms, -1 for stop. */
struct haptic_notify_data {
	int duration;
	int side; /* 1: left, 2: right */
	int mode;
};
int register_haptic_notify(struct notifier_block *nb);
int unregister_haptic_notify(struct notifier_block *nb);
int haptic_notify_call_chain(unsigned long val, void *v); /* 1: start, 2: stop */
#endif
