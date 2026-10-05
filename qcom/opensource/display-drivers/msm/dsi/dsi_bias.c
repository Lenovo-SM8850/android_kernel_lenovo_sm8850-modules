// SPDX-License-Identifier: GPL-2.0-only
/* Lenovo TB323FU bias path reconstructed from the stock msm_drm ELF. */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regmap.h>

#include "dsi_lenovo.h"

#define BIAS_INFO(fmt, ...) pr_info("[drm][bias][%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define BIAS_ERR(fmt, ...) pr_err("[drm][bias][%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__)

static u8 aw3750_init_table[][2] = {
	{ 0x00, 0x15 }, { 0x01, 0x12 }, { 0x03, 0x43 }, { 0x04, 0x09 },
};
static u8 aw3750_match_table[][3] = { { 0x04, 0x01, 0x03 } };
static u8 ocp2140_init_table[][2] = { { 0x00, 0x15 }, { 0x01, 0x12 } };
static u8 ocp2140_match_table[][3] = { { 0x02, 0x00, 0xff } };

static struct bias_chip {
	const char *name;
	u8 (*init_table)[2];
	int init_count;
	u8 (*match_table)[3];
	int match_count;
	bool enable_first;
} data[] = {
	{ "AW37504CSR", aw3750_init_table, 4, aw3750_match_table, 1, false },
	{ "OCP2140WPAD", ocp2140_init_table, 2, ocp2140_match_table, 1, false },
};

static struct {
	const char *name;
	struct pinctrl_state *state;
} bias_gpios[] = {
	{ "enp_active" }, { "enn_active" }, { "enp_sleep" }, { "enn_sleep" },
};

static struct regmap *bias_regmap;
static struct pinctrl *bias_pinctrl;
static int bias_id;
static bool bias_initialized;

static const struct regmap_config bias_regmap_config = {
	.reg_bits = 8, .val_bits = 8, .max_register = 0xff,
};

static int i2c_write_table(struct bias_chip *chip)
{
	int i;

	for (i = 0; i < chip->init_count; i++) {
		u8 *entry = chip->init_table[i];
		int ret = regmap_write(bias_regmap, entry[0], entry[1]);

		if (ret) {
			BIAS_ERR("set reg failed %02x=>%02x\n", entry[0], entry[1]);
			return ret;
		}
	}
	return 0;
}

int bias_enable(bool enable)
{
	struct bias_chip *chip = &data[bias_id];

	if (!bias_initialized) {
		BIAS_ERR("bias not init\n");
		return -ENXIO;
	}
	BIAS_ERR("set bias %d\n", enable);
	if (enable) {
		if (!chip->enable_first && i2c_write_table(chip)) {
			BIAS_ERR("can not set bias without enable, fallback enable first\n");
			chip->enable_first = true;
		}
		if (pinctrl_select_state(bias_pinctrl, bias_gpios[0].state)) {
			BIAS_ERR("set enp error\n");
			return -EIO;
		}
		if (chip->enable_first && i2c_write_table(chip)) {
			BIAS_ERR("can not set bias, maybe device is not online\n");
			return -EIO;
		}
		msleep(5);
		if (pinctrl_select_state(bias_pinctrl, bias_gpios[1].state)) {
			BIAS_ERR("set enn error\n");
			return -EIO;
		}
	} else {
		if (pinctrl_select_state(bias_pinctrl, bias_gpios[3].state)) {
			BIAS_ERR("set enn error\n");
			return -EIO;
		}
		msleep(5);
		if (pinctrl_select_state(bias_pinctrl, bias_gpios[2].state)) {
			BIAS_ERR("set enp error\n");
			return -EIO;
		}
	}
	return 0;
}

static bool check_match_regs(struct bias_chip *chip)
{
	unsigned int value = 0;
	int i;

	for (i = 0; i < chip->match_count; i++) {
		u8 *entry = chip->match_table[i];

		if (regmap_read(bias_regmap, entry[0], &value)) {
			BIAS_ERR("failed to read reg\n");
			return false;
		}
		BIAS_INFO("read reg %02X = %02X\n", entry[0], value);
		if ((value & entry[2]) != entry[1])
			return false;
	}
	return true;
}

static int probe(struct i2c_client *client)
{
	int i;

	bias_regmap = devm_regmap_init_i2c(client, &bias_regmap_config);
	if (IS_ERR_OR_NULL(bias_regmap)) {
		BIAS_ERR("regmap failed, error is %ld\n", PTR_ERR(bias_regmap));
		return -EPROBE_DEFER;
	}
	bias_pinctrl = devm_pinctrl_get(&client->dev);
	if (IS_ERR(bias_pinctrl)) {
		BIAS_ERR("get bias_pinctrl error!\n");
		return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(bias_gpios); i++) {
		bias_gpios[i].state = pinctrl_lookup_state(bias_pinctrl, bias_gpios[i].name);
		if (IS_ERR(bias_gpios[i].state)) {
			BIAS_ERR("pinctrl_lookup_state %s fail, ret %d\n\n",
				bias_gpios[i].name, (int)PTR_ERR(bias_gpios[i].state));
			return -EINVAL;
		}
	}
	for (i = 0; i < ARRAY_SIZE(data); i++) {
		BIAS_INFO("do match %s\n", data[i].name);
		if (check_match_regs(&data[i]))
			break;
	}
	if (i == ARRAY_SIZE(data)) {
		BIAS_ERR("cannot find match bias\n");
		return -ENXIO;
	}
	BIAS_ERR("match successfully, id is %d\n", i);
	bias_id = i;
	if (i2c_write_table(&data[i]))
		BIAS_ERR("failed to init bias\n");
	/* Stock hq_regiser_hw_info(35, data[i].name) is intentionally omitted. */
	bias_initialized = true;
	return 0;
}

static const struct of_device_id bias_match[] = {
	{ .compatible = "bias" }, { }
};

static struct i2c_driver driver = {
	.driver = { .name = "bias", .of_match_table = bias_match },
	.probe = probe,
};

int register_bias(void)
{
	return i2c_add_driver(&driver);
}
