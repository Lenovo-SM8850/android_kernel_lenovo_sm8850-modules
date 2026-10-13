# SPDX-License-Identifier: GPL-2.0-only
# Common and device-exclusive Lenovo module providers.
COMMON_MODULES = [
    "//vendor/lenovo/sm8850-modules/lenovo/hqsysfs:canoe_perf_hqsysfs",
    "//vendor/lenovo/sm8850-modules/lenovo/aw882xx:canoe_perf_aw882xx",
    "//vendor/lenovo/sm8850-modules/lenovo/haptic:canoe_perf_haptic",
    "//vendor/lenovo/sm8850-modules/lenovo/hq_ptn3222_repeater:canoe_perf_hq_ptn3222_repeater",
    "//vendor/lenovo/sm8850-modules/lenovo/leds_aw22xxx:canoe_perf_leds_aw22xxx",
    "//vendor/lenovo/sm8850-modules/lenovo/sys_temp:canoe_perf_lenovo_sys_temp",
    "//vendor/lenovo/sm8850-modules/lenovo/thermal_control:canoe_perf_lenovo_thermal_control",
]

LCD_MODULES = [
    "//vendor/lenovo/sm8850-modules/lenovo/spk_id:canoe_perf_spk_id",
    "//vendor/lenovo/sm8850-modules/lenovo/fsa4480_sub:canoe_perf_fsa4480_sub",
    "//vendor/lenovo/sm8850-modules/lenovo/nvt_36xxx:canoe_perf_nvt_36xxx",
]
