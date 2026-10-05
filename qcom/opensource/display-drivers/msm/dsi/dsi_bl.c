// SPDX-License-Identifier: GPL-2.0-only
/* Lenovo TB323FU backlight path reconstructed from the stock msm_drm ELF. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include "dsi_lenovo.h"

#define BL_INFO(fmt, ...) pr_info("[drm][bl][%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define BL_ERR(fmt, ...) pr_err("[drm][bl][%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__)

static u8 aw99706a_init_table[][3] = {
	{ 0x0d, 0x00, 0xff }, { 0x0c, 0x48, 0xff },
	{ 0x06, 0xd9, 0xff }, { 0x07, 0x05, 0xff },
	{ 0x04, 0x00, 0xff }, { 0x05, 0x00, 0xff },
	{ 0x08, 0x01, 0xff }, { 0x0d, 0xf8, 0xff },
};
static u8 aw99706a_suspend_table[][3] = { { 0x0d, 0x00, 0xff } };
static u8 aw99706a_match_table[][3] = { { 0x11, 0x07, 0xff } };
static int aw99706a_hbm_table[] = { 280 };

static u8 ktz8866_init_table[][3] = {
	{ 0x02, 0xfb, 0xff }, { 0x03, 0xc5, 0xff },
	{ 0x11, 0x37, 0xff }, { 0x14, 0x88, 0xff },
	{ 0x04, 0x00, 0xff }, { 0x05, 0x00, 0xff },
	{ 0x08, 0x4f, 0xff },
};
static u8 ktz8866_suspend_table[][3] = { { 0x08, 0x00, 0xff } };
static u8 ktz8866_match_table[][3] = { { 0x01, 0x20, 0x38 } };
static int ktz8866_hbm_table[] = { 250, 300, 532 };

struct bl_chip {
	const char *name;
	int address;
	u8 (*init_table)[3];
	int init_count;
	u8 (*suspend_table)[3];
	int suspend_count;
	u8 (*match_table)[3];
	int match_count;
	int *hbm_table;
	int hbm_count;
	int current_reg;
	int current_mask;
	int current_step;
	int current_min;
	int brightness_high_reg;
	int brightness_low_reg;
	int brightness_high_mask;
	int brightness_low_mask;
	int high_first;
	int initialized;
	struct device *dev;
	struct regmap *regmap;
	struct i2c_client *client;
};

static struct bl_chip bl_data[] = {
	{
		.name = "aw99706a", .address = 0x76,
		.init_table = aw99706a_init_table, .init_count = 8,
		.suspend_table = aw99706a_suspend_table, .suspend_count = 1,
		.match_table = aw99706a_match_table, .match_count = 1,
		.hbm_table = aw99706a_hbm_table, .hbm_count = 1,
		.current_reg = 0x02, .current_mask = 0x7f,
		.current_step = 5, .current_min = 50,
		.brightness_high_reg = 0x04, .brightness_low_reg = 0x05,
		.brightness_high_mask = 0x0f, .brightness_low_mask = 0xff,
		.high_first = 1,
	}, {
		.name = "ktz8866", .address = 0x11,
		.init_table = ktz8866_init_table, .init_count = 7,
		.suspend_table = ktz8866_suspend_table, .suspend_count = 1,
		.match_table = ktz8866_match_table, .match_count = 1,
		.hbm_table = ktz8866_hbm_table, .hbm_count = 3,
		.current_reg = 0x15, .current_mask = 0xf8,
		.current_step = 8, .current_min = 52,
		.brightness_high_reg = 0x05, .brightness_low_reg = 0x04,
		.brightness_high_mask = 0xff, .brightness_low_mask = 0x07,
	},
};

static struct {
	int initialized;
	int brightness;
	int hbm;
	struct delayed_work check_work;
} bl_context;

static const struct regmap_config bl_regmap_config = {
	.reg_bits = 8, .val_bits = 8, .max_register = 0xff,
};

static int bl_resume(struct device *dev)
{
	struct bl_chip *chip = dev_get_drvdata(dev);
	int ret = 0;

	if (!IS_ERR_OR_NULL(dev->pins) &&
	    !IS_ERR_OR_NULL(dev->pins->default_state)) {
		ret = pinctrl_select_state(dev->pins->p, dev->pins->default_state);
		BL_INFO("%s set default pins\n", chip->name);
	}
	BL_INFO("bl_resume done bltest\n");
	return ret;
}

static int bl_suspend(struct device *dev)
{
	struct bl_chip *chip = dev_get_drvdata(dev);
	int ret = 0;

	if (!IS_ERR_OR_NULL(dev->pins) &&
	    !IS_ERR_OR_NULL(dev->pins->sleep_state)) {
		ret = pinctrl_select_state(dev->pins->p, dev->pins->sleep_state);
		BL_INFO("%s set sleep pin\n", chip->name);
	}
	return ret;
}

static int bl_set_init_table(struct bl_chip *chip)
{
	int i, ret;

	for (i = 0; i < chip->init_count; i++) {
		u8 *entry = chip->init_table[i];

		if (entry[2] == 0xff)
			ret = regmap_write(chip->regmap, entry[0], entry[1]);
		else
			ret = regmap_update_bits(chip->regmap, entry[0], entry[2], entry[1]);
		if (ret) {
			BL_ERR("set reg failed %02x=>%02x\n", entry[0], entry[1]);
			return ret;
		}
	}
	return 0;
}

static int bl_set_suspend_table(struct bl_chip *chip)
{
	int i, ret;

	for (i = 0; i < chip->suspend_count; i++) {
		u8 *entry = chip->suspend_table[i];

		if (entry[0] == chip->brightness_high_reg ||
		    entry[0] == chip->brightness_low_reg)
			continue;
		if (entry[2] == 0xff)
			ret = regmap_write(chip->regmap, entry[0], entry[1]);
		else
			ret = regmap_update_bits(chip->regmap, entry[0], entry[2], entry[1]);
		if (ret) {
			BL_ERR("set reg failed %02x=>%02x\n", entry[0], entry[1]);
			return ret;
		}
	}
	return 0;
}

static int bl_set_current(struct bl_chip *chip, unsigned int level)
{
	int led_current = chip->hbm_table[level];
	int value = (led_current - chip->current_min) / chip->current_step;

	BL_INFO("set current %d.%d mA\n", led_current / 10, led_current % 10);
	if (regmap_update_bits(chip->regmap, chip->current_reg,
			chip->current_mask, value << __ffs(chip->current_mask))) {
		BL_ERR("update led current reg failed\n");
		return -EIO;
	}
	return 0;
}

int bl_set_hbm(unsigned int level)
{
	int i;

	BL_INFO("set hbm level %u\n", level);
	for (i = 0; i < ARRAY_SIZE(bl_data); i++) {
		struct bl_chip *chip = &bl_data[i];

		if (!chip->initialized)
			continue;
		if (level >= chip->hbm_count) {
			BL_ERR("%s: hbm level too high\n", chip->name);
			return -EINVAL;
		}
		if (bl_set_current(chip, level)) {
			BL_ERR("%s: fail to set hbm\n", chip->name);
			return -EIO;
		}
	}
	/* Stock stores the request even when no backlight IC has probed. */
	bl_context.hbm = level;
	return 0;
}

int bl_get_hbm(void)
{
	int i, level = 0;

	for (i = 0; i < ARRAY_SIZE(bl_data); i++) {
		struct bl_chip *chip = &bl_data[i];
		unsigned int value;
		int led_current;

		if (!chip->initialized)
			continue;
		if (regmap_read(chip->regmap, chip->current_reg, &value))
			return -EIO;
		led_current = ((value & chip->current_mask) >> __ffs(chip->current_mask)) *
			chip->current_step + chip->current_min;
		if (led_current < 0)
			return led_current;
		for (level = 0; level < chip->hbm_count; level++)
			if (chip->hbm_table[level] >= led_current)
				break;
		if (level == chip->hbm_count) {
			BL_ERR("%s: error current config %d\n", chip->name, led_current);
			return -EINVAL;
		}
		BL_INFO("%s: max current is %d, hbm level is %d\n",
			chip->name, led_current, level);
	}
	if (level != bl_context.hbm)
		BL_ERR("hbm level %d is not equal to actual device setting\n", bl_context.hbm);
	return level;
}

static int _bl_set_brightness(struct bl_chip *chip, int level)
{
	int shift = hweight32(chip->brightness_low_mask);
	unsigned int high = ((level >> shift) << __ffs(chip->brightness_high_mask)) &
		chip->brightness_high_mask;
	unsigned int low = (level << __ffs(chip->brightness_low_mask)) &
		chip->brightness_low_mask;
	int ret;

	BL_INFO("%s: set backlight %d\n", chip->name, level);
	if (chip->high_first) {
		ret = regmap_write(chip->regmap, chip->brightness_high_reg, high);
		if (!ret)
			ret = regmap_write(chip->regmap, chip->brightness_low_reg, low);
	} else {
		ret = regmap_write(chip->regmap, chip->brightness_low_reg, low);
		if (!ret)
			ret = regmap_write(chip->regmap, chip->brightness_high_reg, high);
	}
	if (ret)
		BL_ERR("%s: write brightness error\n", chip->name);
	return ret;
}

int bl_set_level(int level)
{
	int i, ret = 0;

	for (i = 0; i < ARRAY_SIZE(bl_data); i++) {
		struct bl_chip *chip = &bl_data[i];

		if (!chip->initialized)
			continue;
		if (level) {
			if (!bl_context.brightness) {
				ret = pm_runtime_get_sync(chip->dev);
				if (ret < 0) {
					pm_runtime_put_noidle(chip->dev);
					BL_ERR("resume failed\n");
					return ret;
				}
				msleep(10);
				BL_INFO("SLEEP_AFTER_POWER_UP done bltest\n");
				ret = bl_set_current(chip, bl_context.hbm);
				if (ret)
					return ret;
				ret = bl_set_init_table(chip);
				if (ret)
					return ret;
				ret = bl_set_current(chip, bl_context.hbm);
				if (ret)
					return ret;
			}
			ret = _bl_set_brightness(chip, level);
			if (!delayed_work_pending(&bl_context.check_work))
				mod_delayed_work(system_wq, &bl_context.check_work, 50);
			pm_runtime_mark_last_busy(chip->dev);
		} else if (bl_context.brightness) {
			ret = _bl_set_brightness(chip, 0);
			if (ret)
				return ret;
			ret = bl_set_suspend_table(chip);
			if (ret)
				return ret;
			pm_runtime_mark_last_busy(chip->dev);
			ret = pm_runtime_put_autosuspend(chip->dev);
			BL_INFO("%s close backlight, autosuspend %d\n", chip->name, ret);
			ret = 0;
		}
	}
	bl_context.brightness = level;
	return ret;
}

static void bl_check_regs(struct work_struct *work)
{
	u8 buffer[32] = { 0 };
	int i;

	if (!bl_context.brightness) {
		BL_INFO("backlight off\n");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(bl_data); i++) {
		if (!bl_data[i].initialized)
			continue;
		if (regmap_bulk_read(bl_data[i].regmap, 0, buffer, sizeof(buffer))) {
			BL_INFO("error read\n");
			return;
		}
		print_hex_dump(KERN_INFO, "[drm] BL_REGS:", DUMP_PREFIX_OFFSET,
			16, 1, buffer, sizeof(buffer), false);
	}
}

static int probe(struct i2c_client *client)
{
	struct bl_chip *chip = NULL;
	unsigned int high = 0, low = 0;
	int i, j, value;

	if (!of_property_read_bool(client->dev.of_node, "lenovo,lcd-backlight"))
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(bl_data); i++) {
		struct bl_chip *candidate = &bl_data[i];

		if (candidate->address != client->addr || candidate->initialized > 0) {
			BL_INFO("skip match %s\n\n", candidate->name);
			continue;
		}
		i2c_set_clientdata(client, candidate);
		bl_resume(&client->dev);
		BL_INFO("try match bl %s\n", candidate->name);
		for (j = 0; j < candidate->match_count; j++) {
			u8 *entry = candidate->match_table[j];

			value = i2c_smbus_read_byte_data(client, entry[0]);
			if (value < 0) {
				BL_ERR("i2c error, check next\n");
				break;
			}
			BL_INFO("reg = %02X, val = %02X, mask = %02X, rv = %02X\n",
				entry[0], entry[1], entry[2], value);
			if ((value & entry[2]) != entry[1])
				break;
		}
		if (j == candidate->match_count) {
			chip = candidate;
			break;
		}
	}
	if (!chip) {
		BL_ERR("no bl ic match\n");
		return -EIO;
	}
	BL_INFO("we will use %s\n", chip->name);
	/* Stock hq_regiser_hw_info(33, chip->name) is intentionally omitted. */
	chip->regmap = devm_regmap_init_i2c(client, &bl_regmap_config);
	if (IS_ERR_OR_NULL(chip->regmap))
		return -ENOMEM;
	chip->initialized = 1;
	chip->dev = &client->dev;
	chip->client = client;
	regmap_read(chip->regmap, chip->brightness_high_reg, &high);
	regmap_read(chip->regmap, chip->brightness_low_reg, &low);
	bl_context.brightness =
		(((high & chip->brightness_high_mask) >> __ffs(chip->brightness_high_mask)) <<
		 hweight32(chip->brightness_low_mask)) |
		((low & chip->brightness_low_mask) >> __ffs(chip->brightness_low_mask));
	BL_INFO("%s: init brightness is %d\n", chip->name, bl_context.brightness);
	if (bl_context.brightness > 0) {
		BL_INFO("bl has been inited\n");
		pm_runtime_get_noresume(chip->dev);
		pm_runtime_set_active(chip->dev);
	}
	pm_runtime_use_autosuspend(chip->dev);
	pm_runtime_set_autosuspend_delay(chip->dev, 2000);
	devm_pm_runtime_enable(chip->dev);
	bl_context.initialized++;
	INIT_DELAYED_WORK(&bl_context.check_work, bl_check_regs);
	return 0;
}

static const struct dev_pm_ops bl_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(bl_suspend, bl_resume, NULL)
};

static const struct of_device_id bl_match[] = {
	{ .compatible = "i2c-backlight" }, { }
};

static struct i2c_driver driver = {
	.driver = { .name = "i2c-bl", .of_match_table = bl_match, .pm = &bl_dev_pm_ops },
	.probe = probe,
};

int register_bl(void)
{
	return i2c_add_driver(&driver);
}
