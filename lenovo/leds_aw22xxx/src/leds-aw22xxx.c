// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lenovo TB323FU AW22xxx LED controller.
 * Reconstructed from leds-aw22xxx.ko (clang r536225) in
 * TB323FU_CN_OPEN_USER_Q00020.0_A16_ZUXOS_2.0.12.088_ST_260610.
 * Register protocol also cross-checked against Awinic's GPL v2 reference
 * driver, Copyright (c) 2017 AWINIC Technology CO., LTD,
 * Nick Li <liweilei@awinic.com.cn> (xm8475 public candidate).
 * Stock address references and deliberate differences: port-leds-aw22xxx.md.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/interrupt.h>
#include <linux/leds.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/workqueue.h>

#define AW22XXX_I2C_NAME "aw22xxx_led"
#define AW22XXX_DRIVER_VERSION "v1.7.0.1"
#define FW_NONE 0
#define FW_UPDATE 1
#define FW_OK 2
#define FW_FAIL 3

enum {
	REG_CHIPID = 0x00, REG_SRST = 0x01, REG_GCR = 0x02,
	REG_MCUCTR = 0x04, REG_TASK0 = 0x05, REG_TASK1 = 0x06,
	REG_INTEN = 0x09, REG_INTST = 0x0a, REG_IMAX = 0x0b,
	REG_AUDCTR = 0x0c, REG_DBGCTR = 0x20, REG_ADDR1 = 0x21,
	REG_ADDR2 = 0x22, REG_DATA = 0x23, REG_BSTCTR = 0x24,
	REG_PAGE = 0xff,
};

static char aw22xxx_cfg_name[][64] = {
	"aw22xxx_cfg_led_off.bin",
	"1_rainbow_spiral.bin",
	"2_color_change.bin",
	"3_color_pluse.bin",
	"4_breath.bin",
	"5_circulation.bin",
	"6_on.bin",
	"7_aduio.bin",
	"8_strobe.bin",
	"9_flicker_blue.bin",
	"9_flicker_red.bin",
	"9_flicker_whilte.bin",
	"10_100_green.bin",
	"10_30_red.bin",
	"10_60_blue.bin",
	"11_earthquake_light_synchronization.bin",
	"factory_all_on_blue.bin",
	"factory_all_on_green.bin",
	"factory_all_on_red.bin",
	"factory_all_on_white.bin",
};

static char aw22xxx_imax_name[][32] = {
	"AW22XXX_IMAX_2mA", "AW22XXX_IMAX_3mA", "AW22XXX_IMAX_4mA",
	"AW22XXX_IMAX_6mA", "AW22XXX_IMAX_9mA", "AW22XXX_IMAX_10mA",
	"AW22XXX_IMAX_15mA", "AW22XXX_IMAX_20mA", "AW22XXX_IMAX_30mA",
	"AW22XXX_IMAX_40mA", "AW22XXX_IMAX_45mA", "AW22XXX_IMAX_60mA",
	"AW22XXX_IMAX_75mA",
};
static const u8 aw22xxx_imax_code[] = {
	8, 0, 9, 1, 2, 11, 3, 12, 4, 14, 5, 6, 7,
};

/* Stock .rodata[0..0xff], read permission = bit 0. Not exported. */
const u8 aw22xxx_reg_access[256] = {
	[0x00] = 1, [0x01 ... 0x06] = 3, [0x07] = 1,
	[0x08 ... 0x0d] = 3, [0x0e] = 1, [0x0f ... 0x10] = 3,
	[0x20 ... 0x24] = 3, [0x25 ... 0x26] = 1,
	[0x30 ... 0x39] = 3, [0xff] = 3,
};

struct aw22xxx_container {
	u32 len, version, bist, key;
	u8 data[];
};

struct aw22xxx {
	struct i2c_client *i2c;
	struct device *dev;
	struct led_classdev cdev;
	struct work_struct brightness_work, task_work, fw_work, cfg_work;
	struct mutex cfg_lock;
	struct completion cfg_done;
	struct pinctrl *pinctrl;
	struct pinctrl_state *reset_state;
	int reset_gpio, irq_gpio, irq;
	u32 imax, rgb[9];
	u8 chipid, fw_update, fw_flags, task0, task1, effect, cfg;
	bool stopping;
};

static char chipid_names[10];

static int aw22xxx_i2c_write(struct aw22xxx *aw, u8 reg, u8 val)
{
	int ret, cnt;

	for (cnt = 0; cnt < 2; cnt++) {
		ret = i2c_smbus_write_byte_data(aw->i2c, reg, val);
		if (ret >= 0)
			return ret;
		pr_err("%s: cjf1 i2c_write cnt=%d error=%d\n", __func__, cnt, ret);
		usleep_range(2000, 3000);
	}
	return ret;
}

static int aw22xxx_i2c_read(struct aw22xxx *aw, u8 reg, u8 *val)
{
	int ret, cnt;

	for (cnt = 0; cnt < 2; cnt++) {
		ret = i2c_smbus_read_byte_data(aw->i2c, reg);
		if (ret >= 0) {
			*val = ret;
			return ret;
		}
		pr_err("%s: cjf2 i2c_read cnt=%d error=%d\n", __func__, cnt, ret);
		usleep_range(2000, 3000);
	}
	return ret;
}

static int aw22xxx_i2c_writes(struct aw22xxx *aw, u8 reg,
			      const u8 *buf, unsigned int len)
{
	u8 *data = kmalloc(len + 1, GFP_KERNEL);
	int ret;

	if (!data)
		return -ENOMEM;
	data[0] = reg;
	memcpy(data + 1, buf, len);
	ret = i2c_master_send(aw->i2c, data, len + 1);
	if (ret < 0)
		pr_err("%s: cjf3 i2c master send error\n", __func__);
	kfree(data);
	return ret;
}

static void aw22xxx_write_bits(struct aw22xxx *aw, u8 reg, u8 mask, u8 val)
{
	u8 old = 0;

	aw22xxx_i2c_read(aw, reg, &old);
	aw22xxx_i2c_write(aw, reg, (old & mask) | val);
}

static void aw22xxx_sw_reset(struct aw22xxx *aw)
{
	aw22xxx_i2c_write(aw, REG_SRST, 0x55);
	usleep_range(2000, 3000);
}

static void aw22xxx_chip_enable(struct aw22xxx *aw, bool enable)
{
	aw22xxx_write_bits(aw, REG_GCR, ~1, enable ? 1 : 0);
	usleep_range(2000, 3000);
}

static void aw22xxx_imax_cfg(struct aw22xxx *aw, u8 imax)
{
	aw22xxx_i2c_write(aw, REG_PAGE, 0);
	aw22xxx_i2c_write(aw, REG_IMAX, min_t(u8, imax, 0x0f));
}

static void aw22xxx_led_display(struct aw22xxx *aw)
{
	aw22xxx_i2c_write(aw, REG_ADDR1, 0xe1);
	aw22xxx_i2c_write(aw, REG_ADDR2, 0);
	aw22xxx_i2c_write(aw, REG_DBGCTR, 2);
	aw22xxx_i2c_write(aw, REG_DATA, 0x3d);
	aw22xxx_i2c_write(aw, REG_DBGCTR, 0);
}

static void aw22xxx_brightness_work(struct work_struct *work)
{
	struct aw22xxx *aw = container_of(work, struct aw22xxx, brightness_work);

	aw22xxx_i2c_write(aw, REG_TASK0, 0xff);
	aw22xxx_write_bits(aw, REG_MCUCTR, ~2, 0);
	aw22xxx_chip_enable(aw, false);
	if (aw->cdev.brightness) {
		aw22xxx_chip_enable(aw, true);
		aw22xxx_write_bits(aw, REG_MCUCTR, ~1, 1);
		/* Stock uses the raw low byte here, unlike imax_store/led_init. */
		aw22xxx_imax_cfg(aw, aw->imax);
		aw22xxx_led_display(aw);
		aw22xxx_i2c_write(aw, REG_TASK0, 0x82);
		aw22xxx_write_bits(aw, REG_MCUCTR, ~2, 2);
	}
}

static void aw22xxx_task_work(struct work_struct *work)
{
	struct aw22xxx *aw = container_of(work, struct aw22xxx, task_work);

	aw22xxx_i2c_write(aw, REG_TASK0, 0xff);
	aw22xxx_write_bits(aw, REG_MCUCTR, ~2, 0);
	aw22xxx_chip_enable(aw, false);
	if (aw->task0) {
		aw22xxx_chip_enable(aw, true);
		aw22xxx_write_bits(aw, REG_MCUCTR, ~1, 1);
		aw22xxx_imax_cfg(aw, aw->imax);
		aw22xxx_led_display(aw);
		aw22xxx_i2c_write(aw, REG_TASK0, aw->task0);
		aw22xxx_i2c_write(aw, REG_TASK1, aw->task1);
		aw22xxx_write_bits(aw, REG_MCUCTR, ~2, 2);
	}
}

static void aw22xxx_set_brightness(struct led_classdev *cdev,
				   enum led_brightness brightness)
{
	struct aw22xxx *aw = container_of(cdev, struct aw22xxx, cdev);

	cdev->brightness = brightness;
	if (!READ_ONCE(aw->stopping))
		schedule_work(&aw->brightness_work);
}

static void aw22xxx_led_init(struct aw22xxx *aw)
{
	aw22xxx_sw_reset(aw);
	aw22xxx_chip_enable(aw, true);
	aw22xxx_imax_cfg(aw, aw22xxx_imax_code[aw->imax]);
	aw22xxx_chip_enable(aw, false);
	/* Lenovo has no reference driver's aw22xxx_sys_init register table. */
}

static void aw22xxx_cfg_loaded(const struct firmware *cont, void *context)
{
	struct aw22xxx *aw = context;
	u8 page = 0, reg, val;
	size_t i;

	mutex_lock(&aw->cfg_lock);
	if (!cont) {
		pr_err("%s: cjf4 failed to read %s\n", __func__,
		       aw22xxx_cfg_name[min_t(u8, aw->effect, 19)]);
		goto out;
	}
	pr_info("%s: cjf5 loaded %s - size: %zu\n", __func__,
		aw22xxx_cfg_name[min_t(u8, aw->effect, 19)], cont->size);
	if (READ_ONCE(aw->stopping) || (cont->size & 1))
		goto out;
	for (i = 0; i < cont->size; i += 2) {
		reg = cont->data[i];
		val = cont->data[i + 1];
		if (reg == REG_PAGE)
			page = val;
		if (aw->cfg != 1 && page == 1 && reg >= 0x10 && reg <= 0x2a) {
			u8 off = reg - 0x10;

			val = aw->rgb[off / 3] >> (8 * (2 - off % 3));
		}
		aw22xxx_i2c_write(aw, reg, val);
		if (!page && reg == REG_GCR && (val & 1))
			usleep_range(2000, 3000);
	}
	pr_info("%s: cjf10 cfg update complete\n", __func__);
out:
	release_firmware(cont);
	mutex_unlock(&aw->cfg_lock);
	complete_all(&aw->cfg_done);
}

static void aw22xxx_cfg_work_routine(struct work_struct *work)
{
	struct aw22xxx *aw = container_of(work, struct aw22xxx, cfg_work);
	int ret;

	if (aw->effect >= ARRAY_SIZE(aw22xxx_cfg_name)) {
		pr_err("%s: cjf12 effect 0x%02x over max value\n",
		       "aw22xxx_cfg_update", aw->effect);
		aw->effect = ARRAY_SIZE(aw22xxx_cfg_name) - 1;
	} else {
		pr_info("%s: cjf11 cfg name=%s\n", "aw22xxx_cfg_update",
			aw22xxx_cfg_name[aw->effect]);
	}
	if (aw->fw_flags != FW_OK)
		return;
	/* Serialize async loads without unlocking another task's mutex. */
	wait_for_completion(&aw->cfg_done);
	if (READ_ONCE(aw->stopping))
		return;
	reinit_completion(&aw->cfg_done);
	ret = request_firmware_nowait(THIS_MODULE, true,
		aw22xxx_cfg_name[min_t(u8, aw->effect, 19)], aw->dev,
		GFP_KERNEL, aw, aw22xxx_cfg_loaded);
	if (ret)
		complete_all(&aw->cfg_done);
}

/* Returns whether BIST completed; pending BIST is not a stock hard error. */
static bool aw22xxx_bist(struct aw22xxx *aw, unsigned int len,
			 unsigned int *bist, u8 *status)
{
	u8 val = 0;

	aw22xxx_sw_reset(aw);
	aw22xxx_chip_enable(aw, true);
	aw22xxx_write_bits(aw, REG_MCUCTR, ~1, 1);
	aw22xxx_i2c_write(aw, REG_ADDR2, (len - 1) >> 8);
	aw22xxx_i2c_write(aw, REG_ADDR1, len - 1);
	aw22xxx_i2c_write(aw, REG_BSTCTR, 7);
	usleep_range(5000, 6500);
	aw22xxx_i2c_read(aw, REG_BSTCTR, &val);
	*status = val;
	if (val != 5)
		return false;
	aw22xxx_i2c_read(aw, 0x25, &val);
	*bist = val;
	aw22xxx_i2c_read(aw, 0x26, &val);
	*bist |= val << 8;
	return true;
}

static int aw22xxx_container_update(struct aw22xxx *aw,
				    struct aw22xxx_container *fw)
{
	unsigned int i, len, bist = 0;
	u8 status = 0;

	aw22xxx_i2c_write(aw, REG_PAGE, 0);
	aw22xxx_sw_reset(aw);
	aw22xxx_chip_enable(aw, true);
	aw22xxx_write_bits(aw, REG_MCUCTR, ~1, 1);
	aw22xxx_i2c_write(aw, 0x80, 0xec);
	aw22xxx_i2c_write(aw, 0x35, 0x29);
	aw22xxx_i2c_write(aw, 0x38, fw->key);
	aw22xxx_i2c_write(aw, REG_ADDR2, 0);
	aw22xxx_i2c_write(aw, REG_ADDR1, 0);
	aw22xxx_i2c_write(aw, REG_DBGCTR, 3);
	aw22xxx_i2c_write(aw, 0x30, 3);
	aw22xxx_i2c_write(aw, REG_DATA, 0);
	/* Lenovo: 4..4.5ms, NOT the reference driver's 40ms. */
	usleep_range(4000, 4500);
	aw22xxx_i2c_write(aw, 0x30, 0);
	for (i = 0x40; i <= 0x44; i += 2) {
		aw22xxx_i2c_write(aw, REG_ADDR2, i);
		aw22xxx_i2c_write(aw, REG_ADDR1, 0);
		aw22xxx_i2c_write(aw, 0x30, 2);
		aw22xxx_i2c_write(aw, REG_DATA, 0);
		usleep_range(6000, 6500);
		aw22xxx_i2c_write(aw, 0x30, 0);
	}
	aw22xxx_i2c_write(aw, REG_DBGCTR, 0);
	aw22xxx_i2c_write(aw, REG_DBGCTR, 3);
	for (i = 0; i < fw->len; i += len) {
		aw22xxx_i2c_write(aw, REG_ADDR2, i >> 8);
		aw22xxx_i2c_write(aw, REG_ADDR1, i);
		aw22xxx_i2c_write(aw, 0x11, 1);
		aw22xxx_i2c_write(aw, 0x30, 4);
		len = min(fw->len - i, 128U);
		aw22xxx_i2c_writes(aw, REG_DATA, fw->data + i, len);
		aw22xxx_i2c_write(aw, 0x11, 0);
		aw22xxx_i2c_write(aw, 0x30, 0);
	}
	aw22xxx_i2c_write(aw, REG_DBGCTR, 0);
	if (aw22xxx_bist(aw, fw->len, &bist, &status)) {
		if (bist != fw->bist) {
			pr_err("%s: cjf14 bist check fail, bist=0x%04x\n", __func__, fw->bist);
			pr_err("%s: cjf15 fw update failed, please reset phone\n", __func__);
			return -1;
		}
		pr_info("%s: cjf13 bist check pass, bist=0x%04x\n", __func__, bist);
	} else {
		pr_err("%s: cjf16 bist check is running, reg0x24=0x%02x\n", __func__, status);
	}
	aw22xxx_i2c_write(aw, REG_BSTCTR, 0);
	return 0;
}

static int aw22xxx_fw_loaded(const struct firmware *cont, struct aw22xxx *aw)
{
	struct aw22xxx_container *fw;
	unsigned int i, bist = 0;
	u16 checksum = 0;
	u8 status = 0;
	char tmp_buf[32] = { 0 };
	int ret;

	if (!cont) {
		pr_err("%s: cjf17 failed to read %s\n", __func__, "aw22xxx_fw.bin");
		release_firmware(cont);
		return -EINVAL;
	}
	pr_info("%s: cjf18 loaded %s - size: %zu\n", __func__, "aw22xxx_fw.bin", cont->size);
	if (cont->size < 40)
		goto invalid;
	for (i = 2; i < cont->size; i++)
		checksum += cont->data[i];
	if (checksum != ((cont->data[0] << 8) | cont->data[1])) {
		pr_err("%s: cjf19 check sum err: check_sum=0x%04x\n", __func__, checksum);
		goto invalid;
	}
	/* 40-byte header: big-endian checksum, version, BIST and length. */
	if (!((cont->data[38] << 8) | cont->data[39]) ||
	    ((cont->data[38] << 8) | cont->data[39]) > cont->size - 40)
		goto invalid;
	fw = kzalloc(cont->size + sizeof(*fw), GFP_KERNEL);
	if (!fw) {
		pr_err("%s: cjf20 Error allocating memory\n", __func__);
		release_firmware(cont);
		return -ENOMEM;
	}
	pr_info("%s: cjf21 fw chip_id : 0x%02x\n", __func__, cont->data[2]);
	memcpy(tmp_buf, cont->data + 3, 16);
	pr_info("%s: cjf22 fw customer: %s\n", __func__, tmp_buf);
	/* Preserve stock's reused buffer, including the customer tail. */
	memcpy(tmp_buf, cont->data + 19, 8);
	pr_info("%s: cjf23 fw project: %s\n", __func__, tmp_buf);
	fw->version = (cont->data[27] << 24) | (cont->data[28] << 16) |
		      (cont->data[29] << 8) | cont->data[30];
	fw->bist = (cont->data[34] << 8) | cont->data[35];
	fw->key = cont->data[36];
	fw->len = (cont->data[38] << 8) | cont->data[39];
	pr_info("%s: cjf24 fw version : 0x%04x\n", __func__, fw->version);
	pr_info("%s: cjf25 fw bist : 0x%04x\n", __func__, fw->bist);
	pr_info("%s: cjf26 fw key : 0x%04x\n", __func__, fw->key);
	pr_info("%s: cjf27 fw len : 0x%04x\n", __func__, fw->len);
	memcpy(fw->data, cont->data + 40, fw->len);
	release_firmware(cont);
	if (aw22xxx_bist(aw, fw->len, &bist, &status)) {
		if (bist == fw->bist) {
			pr_info("%s: bist check pass, bist=0x%04x\n", __func__, bist);
			if (!aw->fw_update) {
				kfree(fw);
				aw22xxx_i2c_write(aw, REG_BSTCTR, 0);
				aw22xxx_led_init(aw);
				aw->fw_flags = FW_OK;
				aw22xxx_i2c_write(aw, REG_BSTCTR, 0);
				return 0;
			}
			pr_info("%s: fw version: 0x%04x, force update fw\n", __func__, fw->version);
		} else {
			pr_info("%s: bist check fail, fw bist=0x%04x, flash bist=0x%04x\n", __func__, fw->bist, bist);
			pr_info("%s: find new fw: 0x%04x, need update\n", __func__, fw->version);
		}
	} else {
		pr_err("%s: cjf28 bist check is running, reg0x24=0x%02x\n", __func__, status);
		pr_info("%s: cjf29 fw need update\n", __func__);
	}
	aw22xxx_i2c_write(aw, REG_BSTCTR, 0);
	pr_info("%s cjf30 update firmware start ....\n", __func__);
	ret = aw22xxx_container_update(aw, fw);
	aw->fw_flags = ret ? FW_FAIL : FW_OK;
	kfree(fw);
	aw->fw_update = 0;
	aw22xxx_led_init(aw);
	/* Stock reports a parsed firmware as loaded even if post-flash BIST fails. */
	return 0;
invalid:
	release_firmware(cont);
	return -EINVAL;
}

static void aw22xxx_fw_work_routine(struct work_struct *work)
{
	struct aw22xxx *aw = container_of(work, struct aw22xxx, fw_work);
	const struct firmware *cont = NULL;
	int cnt, ret;

	aw->fw_flags = FW_UPDATE;
	for (cnt = 0; cnt < 11; cnt++) {
		if (READ_ONCE(aw->stopping))
			return;
		ret = request_firmware(&cont, "aw22xxx_fw.bin", aw->dev);
		if (!ret && cont)
			break;
		pr_err("%s load %s file  need retry fw_retry_cnt:%d",
		       "aw22xxx_fw_update", "aw22xxx_fw.bin", cnt);
		msleep(1000);
	}
	if (cnt == 11)
		return;
	pr_info("%s load %s file success fw_retry_cnt:%d",
		"aw22xxx_fw_update", "aw22xxx_fw.bin", cnt);
	if (aw22xxx_fw_loaded(cont, aw))
		pr_err("%s aw22xxx_fw_loaded fail fw_retry_cnt:%d", "aw22xxx_fw_update", cnt);
}

static void aw22xxx_fw_init(struct aw22xxx *aw)
{
	schedule_work(&aw->fw_work);
}

static void aw22xxx_interrupt_setup(struct aw22xxx *aw)
{
	u8 status = 0;

	aw22xxx_i2c_read(aw, REG_INTST, &status);
	pr_info("%s: cjf31 reg INTST=0x%x\n", "aw22xxx_interrupt_clear", status);
	aw22xxx_write_bits(aw, REG_INTEN, ~1, 1);
	aw22xxx_write_bits(aw, REG_INTEN, ~0x10, 0x10);
}

static irqreturn_t aw22xxx_irq(int irq, void *data)
{
	struct aw22xxx *aw = data;
	u8 status = 0;

	if (READ_ONCE(aw->stopping))
		return IRQ_HANDLED;
	aw22xxx_i2c_read(aw, REG_INTST, &status);
	pr_info("%s: cjf32 reg INTST=0x%x\n", __func__, status);
	if (status & 1) {
		pr_info("%s: cjf33_1 functions compelte!\n", __func__);
		pr_info("%s: cjf34_1 enter standby mode!\n", __func__);
		/* These stock messages do not actually put the chip in standby. */
	}
	if (status & 0x10) {
		pr_info("%s: cjf35 get watch dog irq!\n", __func__);
		aw22xxx_i2c_write(aw, REG_PAGE, 0);
		aw22xxx_write_bits(aw, REG_MCUCTR, ~2, 0);
		aw22xxx_write_bits(aw, REG_MCUCTR, ~2, 2);
		aw22xxx_write_bits(aw, REG_MCUCTR, ~1, 1);
		if (aw->effect)
			schedule_work(&aw->cfg_work);
	}
	return IRQ_HANDLED;
}

static struct aw22xxx *aw22xxx_from_dev(struct device *dev)
{
	return container_of(dev_get_drvdata(dev), struct aw22xxx, cdev);
}

static ssize_t reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	u8 page = 0, val = 0;
	ssize_t len = 0;
	unsigned int i;

	aw22xxx_i2c_read(aw, REG_PAGE, &page);
	for (i = 0; i < 256 && len < PAGE_SIZE - 1; i++) {
		if (!page && !(aw22xxx_reg_access[i] & 1))
			continue;
		aw22xxx_i2c_read(aw, i, &val);
		len += scnprintf(buf + len, PAGE_SIZE - len,
				 "cjf44 reg:0x%02x=0x%02x\n", i, val);
	}
	return len;
}

static ssize_t reg_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	unsigned int reg = 0, val = 0;

	if (sscanf(buf, "%x %x", &reg, &val) == 2)
		aw22xxx_i2c_write(aw22xxx_from_dev(dev), reg, val);
	return count;
}

static ssize_t hwen_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);

	return snprintf(buf, PAGE_SIZE, "hwen=%d\n",
		gpiod_get_raw_value(gpio_to_desc(aw->reset_gpio)));
}

static ssize_t hwen_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	unsigned int val;
	int ret = kstrtouint(buf, 0, &val);

	if (ret) {
		dev_err(aw->dev, "%s: cjf45 input data invalid!", __func__);
		return ret;
	}
	if (!gpio_is_valid(aw->reset_gpio)) {
		if (val == 1)
			pr_err("%s: cjf36 failed\n", "aw22xxx_hw_reset");
		else
			pr_err("%s: cjf37 failed\n", "aw22xxx_hw_off");
		return count;
	}
	gpio_set_value_cansleep(aw->reset_gpio, 0);
	usleep_range(1000, 1500);
	if (val == 1) {
		gpio_set_value_cansleep(aw->reset_gpio, 1);
		usleep_range(1000, 1500);
	}
	return count;
}

static ssize_t fw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "cjf47 firmware name = %s\n", "aw22xxx_fw.bin");
}

static ssize_t fw_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	unsigned int val;
	int ret = kstrtouint(buf, 0, &val);

	if (ret) {
		dev_err(aw->dev, "%s: cjf46 input data invalid!", __func__);
		return ret;
	}
	aw->fw_update = val;
	if (val == 1)
		schedule_work(&aw->fw_work);
	return count;
}

static ssize_t cfg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	ssize_t len = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(aw22xxx_cfg_name); i++)
		len += snprintf(buf + len, PAGE_SIZE - len, "cfg[%x] = %s\n",
				i, aw22xxx_cfg_name[i]);
	len += snprintf(buf + len, PAGE_SIZE - len, "current cfg = %s\n",
			aw22xxx_cfg_name[min_t(u8, aw->effect, 19)]);
	return len;
}

static ssize_t cfg_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	unsigned int val;
	int ret = kstrtouint(buf, 0, &val);

	if (ret) {
		dev_err(aw->dev, "%s: cjf48 input data invalid!", __func__);
		return ret;
	}
	aw->cfg = val;
	if (aw->cfg)
		schedule_work(&aw->cfg_work);
	return count;
}

static ssize_t effect_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "cjf51 effect = 0x%02x\n", aw22xxx_from_dev(dev)->effect);
}

static ssize_t effect_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	unsigned int val;
	int ret = kstrtouint(buf, 0, &val);

	if (ret) {
		dev_err(aw->dev, "%s: cjf49 input data invalid!", __func__);
		return ret;
	}
	aw->effect = val;
	pr_info("%s: cjf50 set effect = %d\n", __func__, aw->effect);
	return count;
}

static ssize_t imax_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	ssize_t len = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(aw22xxx_imax_name); i++)
		len += snprintf(buf + len, PAGE_SIZE - len, "imax[%x] = %s\n",
				i, aw22xxx_imax_name[i]);
	len += snprintf(buf + len, PAGE_SIZE - len,
		"cjf53 current id = 0x%02x, imax = %s\n", aw->imax,
		aw22xxx_imax_name[aw->imax]);
	return len;
}

static ssize_t imax_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	unsigned int val;
	int ret = kstrtouint(buf, 0, &val);

	if (ret) {
		dev_err(aw->dev, "%s: cjf52 input data invalid!", __func__);
		return ret;
	}
	if (val >= ARRAY_SIZE(aw22xxx_imax_code)) {
		dev_err(aw->dev, "%s: imax access violation！ value=%u", __func__, val);
		return -EINVAL;
	}
	aw->imax = val;
	aw22xxx_imax_cfg(aw, aw22xxx_imax_code[val]);
	return count;
}

static ssize_t rgb_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	ssize_t len = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(aw->rgb); i++)
		len += snprintf(buf + len, PAGE_SIZE - len, "rgb[%x] = 0x%06x\n", i, aw->rgb[i]);
	return len;
}

static ssize_t rgb_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	unsigned int idx = 0, val = 0;

	if (sscanf(buf, "%x %x", &idx, &val) == 2) {
		if (idx >= ARRAY_SIZE(aw->rgb))
			return -EINVAL;
		aw->rgb[idx] = val;
	}
	return count;
}

static ssize_t task0_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "task0 = 0x%02x\n", aw22xxx_from_dev(dev)->task0);
}

static ssize_t task0_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	unsigned int val;
	int ret = kstrtouint(buf, 0, &val);

	if (ret) {
		dev_err(aw->dev, "%s: cjf54 input data invalid!", __func__);
		return ret;
	}
	aw->task0 = val;
	schedule_work(&aw->task_work);
	return count;
}

static ssize_t task1_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "task1 = 0x%02x\n", aw22xxx_from_dev(dev)->task1);
}

static ssize_t task1_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	unsigned int val;
	int ret = kstrtouint(buf, 0, &val);

	if (ret) {
		dev_err(aw->dev, "%s: cjf55 input data invalid!", __func__);
		return ret;
	}
	aw->task1 = val;
	return count;
}

static ssize_t signal_led_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct aw22xxx *aw = aw22xxx_from_dev(dev);
	int idx = 0, channel = 0, color = 0, brightness = 0, ret, i;
	u8 regs[8][2];
	static const char * const errors[] = {
		"cjf60 i2c_write error", "cjf61 i2c_write error",
		"cjf62 i2c_write error", "cjf63 i2c_write error",
		"cjf64 i2c_write error", "cjf65 i2c_write error",
		"cjf66 i2c_write error", "cjf67 i2c_write error",
	};

	if (sscanf(buf, "%d %d %d %d", &idx, &channel, &color, &brightness) != 4)
		return count;
	if ((unsigned int)idx > 8) {
		dev_err(aw->dev, "cjf56 rgb_index error: 0 ~ 8");
		return count;
	}
	if ((unsigned int)channel > 2) {
		dev_err(aw->dev, "cjf57 r_g_b error: 0 ~ 2");
		return count;
	}
	if ((unsigned int)color > 255) {
		dev_err(aw->dev, "cjf58 color error: 0 ~ 0x%02X", 255);
		return count;
	}
	if ((unsigned int)brightness > 255) {
		dev_err(aw->dev, "cjf59 br error: 0 ~ 0x%02X", 255);
		return count;
	}
	regs[0][0] = REG_PAGE; regs[0][1] = 0;
	regs[1][0] = REG_AUDCTR; regs[1][1] = 0;
	regs[2][0] = REG_TASK0; regs[2][1] = 0xa0 + idx * 3 + channel;
	regs[3][0] = REG_TASK1; regs[3][1] = color;
	regs[4][0] = REG_MCUCTR; regs[4][1] = 7;
	regs[5][0] = REG_TASK0; regs[5][1] = 0xc0 + idx * 3 + channel;
	regs[6][0] = REG_TASK1; regs[6][1] = brightness;
	regs[7][0] = REG_MCUCTR; regs[7][1] = 7;
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = aw22xxx_i2c_write(aw, regs[i][0], regs[i][1]);
		if (ret < 0) {
			dev_err(aw->dev, "%s", errors[i]);
			return ret;
		}
	}
	return count;
}

static DEVICE_ATTR(reg, 0644, reg_show, reg_store);
static DEVICE_ATTR(hwen, 0644, hwen_show, hwen_store);
static DEVICE_ATTR(fw, 0644, fw_show, fw_store);
static DEVICE_ATTR(cfg, 0644, cfg_show, cfg_store);
static DEVICE_ATTR(effect, 0644, effect_show, effect_store);
static DEVICE_ATTR(imax, 0644, imax_show, imax_store);
static DEVICE_ATTR(rgb, 0644, rgb_show, rgb_store);
static DEVICE_ATTR(task0, 0644, task0_show, task0_store);
static DEVICE_ATTR(task1, 0644, task1_show, task1_store);
static DEVICE_ATTR(signal_led, 0200, NULL, signal_led_store);

static struct attribute *aw22xxx_attributes[] = {
	&dev_attr_reg.attr, &dev_attr_hwen.attr, &dev_attr_fw.attr,
	&dev_attr_cfg.attr, &dev_attr_effect.attr, &dev_attr_imax.attr,
	&dev_attr_rgb.attr, &dev_attr_task0.attr, &dev_attr_task1.attr,
	&dev_attr_signal_led.attr, NULL,
};
static const struct attribute_group aw22xxx_attribute_group = {
	.attrs = aw22xxx_attributes,
};

static int aw22xxx_parse_led_cdev(struct aw22xxx *aw, struct device_node *np)
{
	struct device_node *child;
	int ret;

	for_each_child_of_node(np, child) {
		ret = of_property_read_string(child, "aw22xxx,name", &aw->cdev.name);
		if (ret < 0) {
			dev_err(aw->dev, "cjf68 Failure reading led name, ret = %d\n", ret);
			goto out_node;
		}
		ret = of_property_read_u32(child, "aw22xxx,imax", &aw->imax);
		if (ret < 0) {
			dev_err(aw->dev, "cjf69 Failure reading imax, ret = %d\n", ret);
			goto out_node;
		}
		ret = of_property_read_u32(child, "aw22xxx,brightness", &aw->cdev.brightness);
		if (ret < 0) {
			dev_err(aw->dev, "cjf70 Failure reading brightness, ret = %d\n", ret);
			goto out_node;
		}
		ret = of_property_read_u32(child, "aw22xxx,max_brightness", &aw->cdev.max_brightness);
		if (ret < 0) {
			dev_err(aw->dev, "cjf71 Failure reading max brightness, ret = %d\n", ret);
			goto out_node;
		}
	}
	if (!aw->cdev.name || aw->imax >= ARRAY_SIZE(aw22xxx_imax_code))
		return -EINVAL;
	aw->cdev.brightness_set = aw22xxx_set_brightness;
	ret = led_classdev_register(aw->dev, &aw->cdev);
	if (ret) {
		dev_err(aw->dev, "cjf72 unable to register led ret=%d\n", ret);
		return ret;
	}
	ret = sysfs_create_group(&aw->cdev.dev->kobj, &aw22xxx_attribute_group);
	if (ret) {
		dev_err(aw->dev, "cjf73 led sysfs ret: %d\n", ret);
		led_classdev_unregister(&aw->cdev);
	}
	return ret;
out_node:
	of_node_put(child);
	return ret;
}

static int aw22xxx_read_chipid(struct aw22xxx *aw)
{
	u8 val = 0;
	int cnt, ret;

	aw22xxx_i2c_write(aw, REG_PAGE, 0);
	aw22xxx_sw_reset(aw);
	for (cnt = 0; cnt < 2; cnt++) {
		ret = aw22xxx_i2c_read(aw, REG_SRST, &val);
		if (ret < 0) {
			dev_err(aw->dev, "%s: cjf38 failed to read register AW22XXX_REG_ID: %d\n", __func__, ret);
			return -EIO;
		}
		if (val == 0x76) {
			pr_info("%s cjf39 aw22xxx detected\n", __func__);
			aw22xxx_i2c_read(aw, REG_CHIPID, &val);
			switch (val) {
			case 0x18:
				aw->chipid = 1;
				pr_info("%s: cjf40 chipid: aw22118\n", __func__);
				memcpy(chipid_names, "aw22118", 8);
				break;
			case 0x27:
				aw->chipid = 2;
				pr_info("%s: cjf41 chipid: aw22127\n", __func__);
				memcpy(chipid_names, "aw22127", 8);
				break;
			default:
				pr_err("%s: cjf42 unknown id=0x%02x\n", __func__, val);
				/* Stock still succeeds after a valid 0x76 reset ID. */
				break;
			}
			return 0;
		}
		pr_info("%s cjf43 unsupported device revision (0x%x)\n", __func__, val);
		msleep(1);
	}
	return -EINVAL;
}

static int aw22xxx_i2c_probe(struct i2c_client *i2c)
{
	struct device_node *np = i2c->dev.of_node;
	struct aw22xxx *aw;
	int ret;

	pr_err("cjf73-3 aw22xxx_i2c_probe enter\n");
	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C)) {
		dev_err(&i2c->dev, "cjf74 check_functionality failed\n");
		return -EIO;
	}
	aw = devm_kzalloc(&i2c->dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;
	aw->i2c = i2c;
	aw->dev = &i2c->dev;
	aw->irq = -1;
	i2c_set_clientdata(i2c, aw);
	aw->pinctrl = devm_pinctrl_get(aw->dev);
	if (IS_ERR(aw->pinctrl)) {
		pr_err("cjf74-2 failed to get pinctrl, ret=%d\n", (int)PTR_ERR(aw->pinctrl));
		pr_err("failed to get pinctrl, ret=%d\n", (int)PTR_ERR(aw->pinctrl));
	} else {
		aw->reset_state = pinctrl_lookup_state(aw->pinctrl, "led_reset_ctrl_default");
		if (IS_ERR(aw->reset_state)) {
			pr_err("Set led_en pin state error\n");
			aw->reset_state = NULL;
		} else {
			pr_info("cjf74-1 aw22xxx_i2c_probe enter\n");
			ret = pinctrl_select_state(aw->pinctrl, aw->reset_state);
			if (ret) {
				pr_err("Set led_en pin state error\n");
				if (ret < 0)
					pr_err("failed to get pinctrl, ret=%d\n", ret);
			}
		}
	}
	mutex_init(&aw->cfg_lock);
	init_completion(&aw->cfg_done);
	complete_all(&aw->cfg_done);
	/* Initialize before requesting IRQ; watchdog can schedule cfg_work. */
	INIT_WORK(&aw->brightness_work, aw22xxx_brightness_work);
	INIT_WORK(&aw->task_work, aw22xxx_task_work);
	INIT_WORK(&aw->fw_work, aw22xxx_fw_work_routine);
	INIT_WORK(&aw->cfg_work, aw22xxx_cfg_work_routine);
	aw->reset_gpio = np ? of_get_named_gpio(np, "reset-gpio", 0) : -1;
	aw->irq_gpio = np ? of_get_named_gpio(np, "irq-gpio", 0) : -1;
	if (gpio_is_valid(aw->reset_gpio)) {
		ret = devm_gpio_request_one(aw->dev, aw->reset_gpio, GPIOF_OUT_INIT_LOW, "aw22xxx_rst");
		if (ret) {
			dev_err(aw->dev, "%s: cjf76 rst request failed\n", __func__);
			goto fail;
		}
	}
	if (gpio_is_valid(aw->irq_gpio)) {
		ret = devm_gpio_request_one(aw->dev, aw->irq_gpio, GPIOF_IN, "aw22xxx_int");
		if (ret) {
			dev_err(aw->dev, "%s: cjf77 int request failed\n", __func__);
			goto fail;
		}
	}
	if (gpio_is_valid(aw->reset_gpio)) {
		gpio_set_value_cansleep(aw->reset_gpio, 0);
		usleep_range(1000, 1500);
		gpio_set_value_cansleep(aw->reset_gpio, 1);
		usleep_range(1000, 1500);
	} else {
		pr_err("%s: cjf36 failed\n", "aw22xxx_hw_reset");
	}
	ret = aw22xxx_read_chipid(aw);
	if (ret) {
		dev_err(aw->dev, "%s: cjf78 aw22xxx_read_chipid failed ret=%d\n", __func__, ret);
		goto fail;
	}
	if (gpio_is_valid(aw->irq_gpio)) {
		aw22xxx_interrupt_setup(aw);
		aw->irq = gpio_to_irq(aw->irq_gpio);
		ret = devm_request_threaded_irq(aw->dev, aw->irq, NULL,
			aw22xxx_irq, IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "aw22xxx_led", aw);
		if (ret) {
			dev_err(aw->dev, "%s: cjf79 failed to request IRQ %d: %d\n", __func__, aw->irq, ret);
			aw->irq = -1;
			goto fail;
		}
	} else {
		dev_info(aw->dev, "%s cjf80 skipping IRQ registration\n", __func__);
	}
	ret = aw22xxx_parse_led_cdev(aw, np);
	if (ret) {
		WRITE_ONCE(aw->stopping, true);
		if (aw->irq >= 0)
			devm_free_irq(aw->dev, aw->irq, aw);
		cancel_work_sync(&aw->brightness_work);
		cancel_work_sync(&aw->task_work);
		cancel_work_sync(&aw->cfg_work);
		goto fail;
	}
	aw22xxx_fw_init(aw);
	pr_info("%s cjf82 probe completed successfully!\n", __func__);
	return 0;
fail:
	devm_kfree(aw->dev, aw);
	return ret;
}

static void aw22xxx_i2c_remove(struct i2c_client *i2c)
{
	struct aw22xxx *aw = i2c_get_clientdata(i2c);

	WRITE_ONCE(aw->stopping, true);
	sysfs_remove_group(&aw->cdev.dev->kobj, &aw22xxx_attribute_group);
	led_classdev_unregister(&aw->cdev);
	if (aw->irq >= 0)
		devm_free_irq(aw->dev, aw->irq, aw);
	cancel_work_sync(&aw->fw_work);
	cancel_work_sync(&aw->cfg_work);
	cancel_work_sync(&aw->brightness_work);
	cancel_work_sync(&aw->task_work);
	wait_for_completion(&aw->cfg_done);
	devm_kfree(aw->dev, aw);
}

static const struct i2c_device_id aw22xxx_i2c_id[] = {
	{ AW22XXX_I2C_NAME, 0 }, { }
};
MODULE_DEVICE_TABLE(i2c, aw22xxx_i2c_id);

static const struct of_device_id aw22xxx_dt_match[] = {
	{ .compatible = "awinic,aw22xxx_led" }, { }
};
/* Stock publishes only the i2c alias, not MODULE_DEVICE_TABLE(of, ...). */
static struct i2c_driver aw22xxx_i2c_driver = {
	.driver = {
		.name = AW22XXX_I2C_NAME,
		.of_match_table = aw22xxx_dt_match,
	},
	.probe = aw22xxx_i2c_probe,
	.remove = aw22xxx_i2c_remove,
	.id_table = aw22xxx_i2c_id,
};

static int __init aw22xxx_i2c_init(void)
{
	int ret;

	pr_info("cjf83 aw22xxx driver version %s\n", AW22XXX_DRIVER_VERSION);
	ret = i2c_add_driver(&aw22xxx_i2c_driver);
	if (ret)
		pr_err("cjf84 fail to add aw22xxx device into i2c\n");
	return ret;
}
module_init(aw22xxx_i2c_init);

static void __exit aw22xxx_i2c_exit(void)
{
	i2c_del_driver(&aw22xxx_i2c_driver);
}
module_exit(aw22xxx_i2c_exit);

MODULE_DESCRIPTION("AW22XXX LED Driver");
MODULE_LICENSE("GPL v2");
