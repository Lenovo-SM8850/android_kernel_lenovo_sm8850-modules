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
#include <linux/hqsysfs.h>
#include <linux/proc_fs.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/string.h>
#define HQ_SYS_FS_VER "2023-08-28 V0.1"

static char pcba_config[64] = "UNKNOW";
static char pcba_stage[64] = "UNKNOW";
static char pcba_protocol[64] = "UNKNOW";
static char pcba_hwid[64] = "UNKNOW";
module_param_string(pcba_config, pcba_config, sizeof(pcba_config), 0);
module_param_string(pcba_stage, pcba_stage, sizeof(pcba_stage), 0);
module_param_string(pcba_protocol, pcba_protocol, sizeof(pcba_protocol), 0);
module_param_string(pcba_hwid, pcba_hwid, sizeof(pcba_hwid), 0);

static HW_INFO(HWID_VER, ver);
static HW_INFO(HWID_SUMMARY, hw_summary);
static HW_INFO(HWID_RAM, ram);
static HW_INFO(HWID_ROM, rom);
static HW_INFO(HWID_LCM, lcm);
static HW_INFO(HWID_BL, bl);
static HW_INFO(HWID_BIAS, bias);
static HW_INFO(HWID_CTP, ctp);
static HW_INFO(HWID_SUB_CAM, sub_cam);
static HW_INFO(HWID_SUB_CAM_2, main0_cam);
static HW_INFO(HWID_MAIN_CAM, main1_cam);
static HW_INFO(HWID_MAIN_CAM_2, main2_cam);
static HW_INFO(HWID_MAIN_CAM_3, main3_cam);
static HW_INFO(HWID_MAIN_LENS, main_cam_len);
static HW_INFO(HWID_FLASHLIGHT, flashlight);
static HW_INFO(HWID_GSENSOR, gsensor);
static HW_INFO(HWID_RGB, rgb);
static HW_INFO(HWID_TOF, tof);
static HW_INFO(HWID_SAR, sar);
static HW_INFO(HWID_HALL, hall);
static HW_INFO(HWID_MSENSOR, msensor);
static HW_INFO(HWID_GYRO, gyro);
static HW_INFO(HWID_ALSPS, alsps);
static HW_INFO(HWID_IRDA, irda);
static HW_INFO(HWID_FUEL_GAUGE_IC, fuel_gauge_ic);
static HW_INFO(HWID_NFC, nfc);
static HW_INFO(HWID_PCBA, pcba_config);
static HW_INFO(HWID_PMIC_VERSION, pmic_version);
static HW_INFO(HWID_MASTER1_CHARGE, master1_charge);
static HW_INFO(HWID_MASTER2_CHARGE, master2_charge);
static HW_INFO(HWID_MASTER_CP_CHARGE, master_cp_charge);
static HW_INFO(HWID_SLAVE_CP_CHARGE, slave_cp_charge);
static HW_INFO(HWID_PD_CHARGE, pd_charge);
static HW_INFO(HWID_FG1_CHARGE, fg1_charge);
static HW_INFO(HWID_FG2_CHARGE, fg2_charge);
static HW_INFO(HWID_QC_CHARGE, qc_charge);
static HW_INFO(HWID_SC7603A_CHARGE, sc7603a_charge);
static HW_INFO(HWID_SC7603B_CHARGE, sc7603b_charge);
static HW_INFO(HWID_VIBRATOR, vibrator);
static HW_INFO(HWID_THERMAL_NTC, thermal_ntc);
static HW_INFO(HWID_SDCARD_STATUS, sdcard_status);
#ifdef CONFIG_LENOVO_CELLULAR_HQSYSFS
static HW_INFO(HWID_SIM_STATUS, sim_status);
#endif
static HW_INFO(HWID_SMARTPA, smartpa);
static HW_INFO(HWID_AUDIO_CODEC, audio_codec);
static HW_INFO(HWID_NXP3222_REPEATER, nxp3222_repeater);

static struct attribute *huaqin_attrs[] = {
	&hw_info_ver.attr,
	&hw_info_hw_summary.attr,
	&hw_info_ram.attr,
	&hw_info_rom.attr,
	&hw_info_lcm.attr,
	&hw_info_bl.attr,
	&hw_info_bias.attr,
	&hw_info_ctp.attr,
	&hw_info_sub_cam.attr,
	&hw_info_main0_cam.attr,
	&hw_info_main1_cam.attr,
	&hw_info_main2_cam.attr,
	&hw_info_main3_cam.attr,
	&hw_info_main_cam_len.attr,
	&hw_info_flashlight.attr,
	&hw_info_gsensor.attr,
	&hw_info_rgb.attr,
	&hw_info_tof.attr,
	&hw_info_sar.attr,
	&hw_info_hall.attr,
	&hw_info_msensor.attr,
	&hw_info_gyro.attr,
	&hw_info_alsps.attr,
	&hw_info_irda.attr,
	&hw_info_fuel_gauge_ic.attr,
	&hw_info_nfc.attr,
	&hw_info_pcba_config.attr,
	&hw_info_pmic_version.attr,
	&hw_info_master1_charge.attr,
	&hw_info_master2_charge.attr,
	&hw_info_master_cp_charge.attr,
	&hw_info_slave_cp_charge.attr,
	&hw_info_pd_charge.attr,
	&hw_info_fg1_charge.attr,
	&hw_info_fg2_charge.attr,
	&hw_info_qc_charge.attr,
	&hw_info_sc7603a_charge.attr,
	&hw_info_sc7603b_charge.attr,
	&hw_info_vibrator.attr,
	&hw_info_thermal_ntc.attr,
	&hw_info_sdcard_status.attr,
#ifdef CONFIG_LENOVO_CELLULAR_HQSYSFS
	&hw_info_sim_status.attr,
#endif
	&hw_info_smartpa.attr,
	&hw_info_audio_codec.attr,
	&hw_info_nxp3222_repeater.attr,
	NULL
};

static ssize_t huaqin_show(struct kobject *kobj, struct attribute *a, char *buf)
{
	ssize_t count = 0;
	//int i = 0;
	struct hw_info *hw = container_of(a, struct hw_info, attr);
	if (NULL == hw) {
		return sprintf(buf, "Data error\n");
	}
	if (HWID_VER == hw->hw_id) {
		count = sprintf(buf, "%s\n", HQ_SYS_FS_VER);
	} else if (HWID_SUMMARY == hw->hw_id) {
		//iterate all device and output the detail
		int iterator = 0;
		struct hw_info *curent_hw = NULL;
		struct attribute *attr = huaqin_attrs[iterator];
		while (attr) {
			curent_hw = container_of(attr, struct hw_info, attr);
			iterator += 1;
			attr = huaqin_attrs[iterator];
			if (curent_hw->hw_exist && (NULL != curent_hw->hw_device_name)) {
				count += sprintf(buf+count, "%s: %s\n", curent_hw->attr.name, curent_hw->hw_device_name);
			}
		}
	} else if (HWID_PCBA == hw->hw_id) {
#ifdef CONFIG_LENOVO_CELLULAR_HQSYSFS
		if (!strcmp(pcba_config, "UNKNOW") ||
		    !strcmp(pcba_stage, "UNKNOW") ||
		    !strcmp(pcba_protocol, "UNKNOW"))
			count = sprintf(buf, "PCBA_WUJI_UNKNOW\n");
		else
			count = sprintf(buf, "PCBA_WUJI_%s_%s_%s\n",
				pcba_config, pcba_stage, pcba_protocol);
#else
		if (!strcmp(pcba_config, "UNKNOW") ||
		    !strcmp(pcba_stage, "UNKNOW"))
			count = sprintf(buf, "PCBA_BALDUR_UNKNOW\n");
		else
			count = sprintf(buf, "PCBA_BALDUR_%s_%s\n",
				pcba_config, pcba_stage);
#endif
	} else {
		if (0 == hw->hw_exist) {
			#ifdef CONFIG_LENOVO_CELLULAR_HQSYSFS
			count = sprintf(buf, "Not support\n");
#else
			count = sprintf(buf, "Not support!\n");
#endif
		} else if (NULL == hw->hw_device_name) {
			count = sprintf(buf, "Installed with no device Name!\n");
		} else {
			count = sprintf(buf, "%s\n", hw->hw_device_name);
		}
	}
	return count;
}

static ssize_t huaqin_store(struct kobject *kobj, struct attribute *a, const char *buf, size_t count)
{
	char name[40];
	char *imu;

	if (count >= sizeof(name))
		return -E2BIG;
	memcpy(name, buf, count);
	name[count] = '\0';
	imu = strstr(name, "TDK-Invensense") ? "icm536xx" : "lsm6dsv";
	if (strstr(name, "Accelerometer"))
		hq_regiser_hw_info(HWID_GSENSOR, imu);
	if (strstr(name, "Gyroscope"))
		hq_regiser_hw_info(HWID_GYRO, imu);
	if (strstr(name, "Magnetometer")) {
#ifdef CONFIG_LENOVO_CELLULAR_HQSYSFS
		hq_regiser_hw_info(HWID_MSENSOR, "qmc630x");
#else
		hq_regiser_hw_info(HWID_MSENSOR,
			strstr(name, "memsic") ? "mmc56x3x" : "qmc6309h");
#endif
	}
	if (strstr(name, "Alsps")) {
#ifdef CONFIG_LENOVO_CELLULAR_HQSYSFS
		hq_regiser_hw_info(HWID_ALSPS, "stk3a5x");
#else
		hq_regiser_hw_info(HWID_ALSPS, "stk_stk3acx");
#endif
	}
	if (strstr(name, "Sar"))
		hq_regiser_hw_info(HWID_SAR, "aw963xx0");
	if (strstr(name, "HALL"))
		hq_regiser_hw_info(HWID_HALL, "bu52053nvx");
	return count;
}

/* huaqin object */
static struct kobject huaqin_kobj;

static const struct sysfs_ops huaqin_sysfs_ops = {
	.show = huaqin_show,
	.store = huaqin_store,
};
static struct attribute_group huaqin_group = {
	.attrs = huaqin_attrs,
 };

static void huaqin_release(struct kobject *kobj)
{
}

/* huaqin type */
static struct kobj_type huaqin_ktype = {
	.release = huaqin_release,
	.sysfs_ops = &huaqin_sysfs_ops,
	.default_groups =(const struct attribute_group *[]) {
		&huaqin_group,
		NULL,
	},
};

/* huaqin device class */
static struct class  *huaqin_class;

static struct device *huaqin_hw_device;
int register_kboj_under_hqsysfs(struct kobject *kobj, struct kobj_type *ktype, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = kobject_init_and_add(kobj, ktype, &huaqin_hw_device->kobj, "%pV",
				 &(struct va_format) { .fmt = fmt, .va = &args });
	va_end(args);
	return ret;
}


static int __init create_sysfs(void)
{
	int ret;
	/* create class (device model) */
	huaqin_class = class_create(HUAQIN_CLASS_NAME);
	if (IS_ERR(huaqin_class)) {
		pr_err("%s fail to create class\n", __func__);
		return PTR_ERR(huaqin_class);
	}
	huaqin_hw_device = device_create(huaqin_class, NULL, MKDEV(0, 0), NULL, HUAIN_INTERFACE_NAME);
	if (IS_ERR(huaqin_hw_device)) {
		pr_warn("fail to create device\n");
		class_destroy(huaqin_class);
		return PTR_ERR(huaqin_hw_device);
	}
	/* add kobject */
	ret = kobject_init_and_add(&huaqin_kobj, &huaqin_ktype, &(huaqin_hw_device->kobj), HUAQIN_HWID_NAME);
	if (ret < 0) {
		pr_err("%s fail to add kobject\n", __func__);
		kobject_put(&huaqin_kobj);
		device_destroy(huaqin_class, MKDEV(0, 0));
		class_destroy(huaqin_class);
		return ret;
	}
	return 0;
}

int hq_deregister_hw_info(enum hardware_id id, char *device_name)
{
	int ret = 0;
	int find_hw_id = 0;
	int iterator = 0;
	struct hw_info *hw = NULL;
	struct attribute *attr = huaqin_attrs[iterator];
	if (NULL == device_name) {
		pr_err("[%s]: device_name does not allow empty\n", __func__);
		ret = -2;
		goto err;
	}
	while (attr) {
		hw = container_of(attr, struct hw_info, attr);
		iterator += 1;
		attr = huaqin_attrs[iterator];
		if (NULL == hw) {
			continue;
		}
		if (id == hw->hw_id) {
			find_hw_id = 1;
			if (0 == hw->hw_exist) {
				pr_err("[%s]: device has not registed hw->id:0x%x . Cant be deregistered\n"
					, __func__
					, hw->hw_id);
				ret = -4;
				goto err;
			} else if (NULL == hw->hw_device_name) {
				pr_err("[%s]:hw_id is 0x%x Device name cant be NULL\n"
					, __func__
					, hw->hw_id);
				ret = -5;
				goto err;
			} else {
				if (0 == strncmp(hw->hw_device_name, device_name, strlen(hw->hw_device_name))) {
					hw->hw_device_name = NULL;
					hw->hw_exist = 0;
				} else {
					pr_err("[%s]: hw_id is 0x%x Registered device name %s , want to deregister: %s\n"
						, __func__
						, hw->hw_id
						, hw->hw_device_name
						, device_name);
					ret = -6;
					goto err;
				}
			}
			goto err;
		} else
			continue;
	}
	if (0 == find_hw_id) {
		pr_err("[%s]: Cant find correct hardware_id: 0x%x\n", __func__, id);
		ret = -3;
	}
err:
	return ret;
}

EXPORT_SYMBOL_GPL(hq_deregister_hw_info);

int hq_regiser_hw_info(enum hardware_id id, char *device_name)
{
	int ret = 0;
	int find_hw_id = 0;
	int iterator = 0;
	struct hw_info *hw = NULL;
	struct attribute *attr = huaqin_attrs[iterator];
	if (NULL == device_name) {
		pr_err("[%s]: device_name does not allow empty\n", __func__);
		ret = -2;
		goto err;
	}
	while (attr) {
		hw = container_of(attr, struct hw_info, attr);
		iterator += 1;
		attr = huaqin_attrs[iterator];
		if (NULL == hw) {
			continue;
		}
		if (id == hw->hw_id) {
			find_hw_id = 1;
			if (hw->hw_exist) {
				pr_err("[%s]: device has already registed hw->id:0x%x hw_device_name:%s\n"
					, __func__
					, hw->hw_id
					, hw->hw_device_name);
				ret = -4;
				goto err;
			}
			switch (hw->hw_id) {
				/*
					if (map_cam_drv_to_vendor(device_name))
						hw->hw_device_name = map_cam_drv_to_vendor(device_name);
					else
						hw->hw_device_name = "Can't find Camera Vendor";
					break;
				*/
			default:
					hw->hw_device_name = device_name;
					break;
			}
			hw->hw_exist = 1;
			goto err;
		} else
			continue;
	}
	if (0 == find_hw_id) {
		pr_err("[%s]: Cant find correct hardware_id: 0x%x\n", __func__, id);
		ret = -3;
	}
err:
	return ret;
}
EXPORT_SYMBOL_GPL(hq_regiser_hw_info);
static int __init hq_harware_init(void)
{
	return create_sysfs();
}

static void __exit hq_harware_exit(void)
{
	kobject_put(&huaqin_kobj);
	device_destroy(huaqin_class, MKDEV(0, 0));
	class_destroy(huaqin_class);
}

module_init(hq_harware_init);
module_exit(hq_harware_exit);
MODULE_AUTHOR("gaobowei@huaqin.com");
MODULE_DESCRIPTION("Huaqin Hardware Info Driver");
MODULE_LICENSE("GPL");
