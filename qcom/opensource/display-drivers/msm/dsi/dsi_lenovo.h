/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DSI_LENOVO_H
#define DSI_LENOVO_H

#include <linux/completion.h>

#if IS_ENABLED(CONFIG_DRM_MSM_LENOVO_BALDUR)
int register_bl(void);
int register_bias(void);
int bl_set_level(int level);
int bl_set_hbm(unsigned int level);
int bl_get_hbm(void);
int bias_enable(bool enable);

extern bool nvt_gesture_flag;
extern void nvt_update_firmware_work(struct completion *load_fw_completion);
#else
static inline int register_bl(void) { return 0; }
static inline int register_bias(void) { return 0; }
static inline int bl_set_level(int level) { return 0; }
static inline int bias_enable(bool enable) { return 0; }
static const bool nvt_gesture_flag;
static inline void nvt_update_firmware_work(struct completion *completion) { }
#endif

#endif
