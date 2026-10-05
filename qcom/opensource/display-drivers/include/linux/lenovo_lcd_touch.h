/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LENOVO_LCD_TOUCH_H
#define __LENOVO_LCD_TOUCH_H

#include <linux/types.h>

struct module;
struct completion;

struct lenovo_lcd_touch_ops {
	struct module *owner;
	void (*update_firmware)(struct completion *);
	bool *gesture_flag;
};

int lenovo_lcd_register_touch(const struct lenovo_lcd_touch_ops *ops);
void lenovo_lcd_unregister_touch(const struct lenovo_lcd_touch_ops *ops);
#endif
