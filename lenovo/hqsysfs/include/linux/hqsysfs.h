/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#ifndef __HQ_SYSFS_HEAD__
#define __HQ_SYSFS_HEAD__
#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/kfifo.h>
#include <linux/firmware.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#ifdef CONFIG_OF
#include <linux/of_fdt.h>
#endif

#include <linux/atomic.h>
#define MAX_HW_DEVICE_NAME (64)

enum hardware_id {
	HWID_NONE = 0,
	HWID_VER = 0xF1,
	HWID_SUMMARY = 0xF0,
	HWID_RAM = 0x13,
	HWID_ROM = 0x15,
	HWID_LCM = 0x20,
	HWID_BL = 0x21,
	HWID_BIAS = 0x23,
	HWID_CTP = 0x24,
	HWID_SUB_CAM = 0x33,
	HWID_SUB_CAM_2 = 0x34,
	HWID_MAIN_CAM = 0x30,
	HWID_MAIN_CAM_2 = 0x31,
	HWID_MAIN_CAM_3 = 0x32,
	HWID_MAIN_LENS = 0x35,
	HWID_FLASHLIGHT = 0x3D,
	HWID_GSENSOR = 0x70,
	HWID_RGB = 0x72,
	HWID_TOF = 0x73,
	HWID_SAR = 0x74,
	HWID_HALL = 0x75,
	HWID_MSENSOR = 0x77,
	HWID_GYRO = 0x76,
	HWID_ALSPS = 0x71,
	HWID_IRDA = 0x78,
	HWID_FUEL_GAUGE_IC = 0xA1,
	HWID_NFC = 0xC0,
	HWID_PCBA = 0x80,
	HWID_PMIC_VERSION = 0xF2,
	HWID_MASTER1_CHARGE = 0x50,
	HWID_MASTER2_CHARGE = 0x51,
	HWID_MASTER_CP_CHARGE = 0x52,
	HWID_SLAVE_CP_CHARGE = 0x53,
	HWID_PD_CHARGE = 0x54,
	HWID_FG1_CHARGE = 0x55,
	HWID_FG2_CHARGE = 0x56,
	HWID_QC_CHARGE = 0x57,
	HWID_SC7603A_CHARGE = 0x58,
	HWID_SC7603B_CHARGE = 0x59,
	HWID_VIBRATOR = 0xE1,
	HWID_THERMAL_NTC = 0x26,
	HWID_SDCARD_STATUS = 0x83,
	HWID_SIM_STATUS = 0x82,
	HWID_SMARTPA = 0x84,
	HWID_AUDIO_CODEC = 0x85,
	HWID_NXP3222_REPEATER = 0xF3,
	HWID_END
};

struct hw_info{
	enum hardware_id hw_id;
	struct attribute attr;
	unsigned int hw_exist;
	char *hw_device_name;
};

#define __INFO(_id, _hw_type_name) {				\
		.hw_id = _id,				\
		.attr = {.name = __stringify(_hw_type_name),				\
		.mode = VERIFY_OCTAL_PERMISSIONS(S_IWUSR|S_IRUGO) },		\
		.hw_exist	= 0,						\
		.hw_device_name	= NULL,						\
	}
#define HW_INFO(_id, _hw_type_name) \
	struct hw_info hw_info_##_hw_type_name = __INFO(_id, _hw_type_name)
#define HUAQIN_CLASS_NAME       "hardinfo"
#define HUAIN_INTERFACE_NAME	"interface"
#define HUAQIN_HWID_NAME        "hardware_info"
#define HUAQIN_VERSION_FILE		"hw_info_ver"
int hq_regiser_hw_info(enum hardware_id id, char *device_name);
int hq_deregister_hw_info(enum hardware_id id, char *device_name);
int register_kboj_under_hqsysfs(struct kobject *kobj, struct kobj_type *ktype, const char *fmt, ...);

#endif /* __HQ_SYSFS_HEAD__ */
