// SPDX-License-Identifier: GPL-2.0
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#include "sgm66075.h"

#define SGM_ID_REG 0
#define SGM_CONFIG_REG 1
#define SGM_VOUT_REG 2
#define SGM_ILIMIT_REG 3
#define SGM_STATUS_REG 4
#define SGM_ID 0x32
#define SGM_RESET BIT(7)
#define SGM_ENABLE BIT(6)
#define SGM_AUTO_BYPASS GENMASK(5, 4)
#define SGM_VOUT_MASK GENMASK(5, 0)
#define SGM_ILIMIT_MASK GENMASK(3, 0)
#define SGM_MIN_UV 2850000
#define SGM_MAX_UV 5000000
#define SGM_STEP_UV 50000
#define SGM_MIN_UA 4000000
#define SGM_MAX_UA 9000000
#define SGM_STEP_UA 500000

/* The stock exports consume this layout and cache status at byte 25. */
static_assert(offsetof(struct sgm66075, id) == 24);
static_assert(offsetof(struct sgm66075, status) == 25);

static void sgm_update(struct sgm66075 *sgm, unsigned int reg,
		       unsigned int mask, unsigned int value)
{
	int ret = regmap_write_bits(sgm->regmap, reg, mask, value);

	if (ret)
		dev_err(sgm->dev, "register %u update failed: %d\n", reg, ret);
}

int sgm_boost_enable_set(struct sgm66075 *sgm, bool enable)
{
	sgm_update(sgm, SGM_CONFIG_REG, SGM_ENABLE, enable ? SGM_ENABLE : 0);
	/* Stock setters report success even when the bus update fails. */
	return 0;
}
EXPORT_SYMBOL(sgm_boost_enable_set);

int sgm_boost_vout_set(struct sgm66075 *sgm, int uv)
{
	/* Stock compares the upper bound unsigned, then the lower signed. */
	uv = min_t(unsigned int, uv, SGM_MAX_UV);
	uv = max(uv, SGM_MIN_UV);
	sgm_update(sgm, SGM_VOUT_REG, SGM_VOUT_MASK,
		   (uv - SGM_MIN_UV) / SGM_STEP_UV);
	return 0;
}
EXPORT_SYMBOL(sgm_boost_vout_set);

int sgm_boost_ilimit_set(struct sgm66075 *sgm, int ua)
{
	ua = min_t(unsigned int, ua, SGM_MAX_UA);
	ua = max(ua, SGM_MIN_UA);
	sgm_update(sgm, SGM_ILIMIT_REG, SGM_ILIMIT_MASK,
		   (ua - SGM_MIN_UA) / SGM_STEP_UA);
	return 0;
}
EXPORT_SYMBOL(sgm_boost_ilimit_set);

int sgm_boost_status_get(struct sgm66075 *sgm)
{
	unsigned int status;
	int ret = regmap_read(sgm->regmap, SGM_STATUS_REG, &status);

	if (ret)
		return -1;
	sgm->status = status;
	return 0;
}
EXPORT_SYMBOL(sgm_boost_status_get);

static ssize_t registers_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct sgm66075 *sgm = dev_get_drvdata(dev);
	unsigned int reg, value;
	ssize_t count = 0;

	for (reg = 0; reg <= SGM_STATUS_REG; reg++) {
		if (regmap_read(sgm->regmap, reg, &value))
			continue;
		count += sysfs_emit_at(buf, count, "Reg[%.2X] = 0x%.2x\n",
				       reg, value);
	}
	return count;
}

static ssize_t registers_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct sgm66075 *sgm = dev_get_drvdata(dev);
	unsigned int reg, value;
	int ret;

	/* Stock ignores malformed/out-of-range writes and returns count. */
	if (sscanf(buf, "%x %x", &reg, &value) == 2 && reg <= SGM_STATUS_REG) {
		ret = regmap_write(sgm->regmap, reg, (u8)value);
		if (ret)
			dev_err(dev, "register %u write failed: %d\n", reg, ret);
	}
	return count;
}
static DEVICE_ATTR_RW(registers);

static struct attribute *sgm_attrs[] = {
	&dev_attr_registers.attr,
	NULL,
};
static const struct attribute_group sgm_group = {
	.attrs = sgm_attrs,
};

static const struct linear_range sgm_vout_range[] = {
	REGULATOR_LINEAR_RANGE(SGM_MIN_UV, 0, 43, SGM_STEP_UV),
};

static const struct regulator_ops sgm_regulator_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_current_limit = regulator_set_current_limit_regmap,
	.get_current_limit = regulator_get_current_limit_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

static const struct regulator_desc sgm_regulator = {
	.name = "SGM_BOOST_VOUT",
	.supply_name = "SGM_BOOST_VOUT",
	.of_match = "\"SGM_BOOST_VOUT\"",
	.ops = &sgm_regulator_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	/* Preserve the stock descriptor, including its 43-selector count. */
	.n_voltages = 43,
	.linear_ranges = sgm_vout_range,
	.n_linear_ranges = ARRAY_SIZE(sgm_vout_range),
	.vsel_reg = SGM_VOUT_REG,
	.vsel_mask = SGM_VOUT_MASK,
	.enable_reg = SGM_CONFIG_REG,
	.enable_mask = SGM_ENABLE,
	.active_discharge_on = BIT(3),
	.active_discharge_mask = BIT(3),
	.active_discharge_reg = SGM_CONFIG_REG,
	.enable_time = 150,
};

static const struct regmap_config sgm_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = SGM_STATUS_REG,
};

static int sgm66075_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { .dev = dev };
	struct regulator_dev *rdev;
	struct sgm66075 *sgm;
	unsigned int id;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;
	sgm = devm_kzalloc(dev, sizeof(*sgm), GFP_KERNEL);
	if (!sgm)
		return -ENOMEM;
	sgm->dev = dev;
	sgm->client = client;
	sgm->regmap = devm_regmap_init_i2c(client, &sgm_regmap);
	if (IS_ERR(sgm->regmap))
		return PTR_ERR(sgm->regmap);
	ret = regmap_read(sgm->regmap, SGM_ID_REG, &id);
	if (ret || id != SGM_ID)
		return -ENODEV;
	sgm->id = id;
	i2c_set_clientdata(client, sgm);
	ret = devm_device_add_group(dev, &sgm_group);
	if (ret)
		return ret;
	config.driver_data = sgm;
	config.regmap = sgm->regmap;
	rdev = devm_regulator_register(dev, &sgm_regulator, &config);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);

	/* Stock probe: reset, boost on, auto-bypass2, 3.6V and 8.5A. */
	sgm_update(sgm, SGM_CONFIG_REG, SGM_RESET, SGM_RESET);
	sgm_boost_enable_set(sgm, true);
	sgm_update(sgm, SGM_CONFIG_REG, SGM_AUTO_BYPASS, 2 << 4);
	sgm_boost_vout_set(sgm, 3600000);
	sgm_boost_ilimit_set(sgm, 8500000);
	return 0;
}

static const struct of_device_id sgm_of_match[] = {
	{ .compatible = "sgm,sgm66075" },
	{ }
};
MODULE_DEVICE_TABLE(of, sgm_of_match);

static const struct i2c_device_id sgm_ids[] = {
	{ "sgm66075" },
	{ }
};

static struct i2c_driver sgm_driver = {
	.driver = {
		.name = "sgm66075",
		.of_match_table = sgm_of_match,
	},
	.probe = sgm66075_probe,
	.id_table = sgm_ids,
};
module_i2c_driver(sgm_driver);

MODULE_DESCRIPTION("sgm66075 boost Driver");
MODULE_LICENSE("GPL");
