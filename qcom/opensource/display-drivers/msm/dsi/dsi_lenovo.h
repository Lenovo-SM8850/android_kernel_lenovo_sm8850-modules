/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DSI_LENOVO_H
#define DSI_LENOVO_H

#include <linux/completion.h>

#if IS_ENABLED(CONFIG_DRM_MSM_LENOVO_DISPLAY)
int register_bl(void);
int register_bias(void);
int bl_set_level(int level);
int bl_set_hbm(unsigned int level);
int bl_get_hbm(void);
int bias_enable(bool enable);

bool lenovo_lcd_gesture_enabled(void);
bool lenovo_lcd_wait_for_firmware(void);
void lenovo_lcd_touch_supplier_put(void);
#else
static inline int register_bl(void) { return 0; }
static inline int register_bias(void) { return 0; }
static inline int bl_set_level(int level) { return 0; }
static inline int bias_enable(bool enable) { return 0; }
static inline bool lenovo_lcd_gesture_enabled(void) { return false; }
static inline bool lenovo_lcd_wait_for_firmware(void) { return false; }
static inline void lenovo_lcd_touch_supplier_put(void) { }
#endif

#endif
