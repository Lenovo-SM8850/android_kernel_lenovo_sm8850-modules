# SPDX-License-Identifier: GPL-2.0-only
# Common and device-exclusive Lenovo module providers.
COMMON_MODULES = [
    "//vendor/lenovo/sm8850-modules/lenovo/hqsysfs:canoe_perf_hqsysfs",
    "//vendor/lenovo/sm8850-modules/lenovo/aw882xx:canoe_perf_aw882xx",
]

LCD_MODULES = [
    "//vendor/lenovo/sm8850-modules/lenovo/nvt_36xxx:canoe_perf_nvt_36xxx",
]

PERIPHERAL_MODULES = [
    "//vendor/lenovo/sm8850-modules/lenovo/touch/gt9976n_thp:canoe_perf_gt9976n_thp_core",
]
