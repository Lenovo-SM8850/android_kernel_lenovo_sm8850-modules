// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP PTN3222 / TI eUSB2 repeater I2C driver (Huaqin flavour) for the Lenovo TB323FU.
 *
 * Derived from the Lenovo TB321FU GPL release (kernel 6.1.68,
 * drivers/usb/repeater/repeater-i2c-eusb2_ptn3222.c), itself derived from the Qualcomm
 * repeater-i2c-eusb2.c, ported to Linux 6.12 and brought to the behaviour of the stock
 * TB323FU hq_ptn3222_repeater.ko (reconstructed from its disassembly):
 *  - C1_d_present / C2_d_present are imported from ucsi_qti_glink (not defined here);
 *  - init writes the "qcom,param-override-seq-cX" sequence of the type-C port that has a device
 *    (C1 first, then C2, none if neither) the first time, and afterwards only restores the
 *    register values that power-down saved ("reg save value"), see EUSB2_APPLY_PORT;
 *  - power-down saves registers 0..10 before the regulators go off;
 *  - the Huaqin hqsysfs hardware-info registration (hq_regiser_hw_info(243, "PTN3222")) is left out.
 */
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/usb/lenovo_ucsi.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/qti-regmap-debugfs.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/usb/repeater.h>

#define EUSB2_3P0_VOL_MIN			3075000 /* uV */
#define EUSB2_3P0_VOL_MAX			3300000 /* uV */
#define EUSB2_3P0_HPM_LOAD			3500	/* uA */

#define EUSB2_1P8_VOL_MIN			1800000 /* uV */
#define EUSB2_1P8_VOL_MAX			1800000 /* uV */
#define EUSB2_1P8_HPM_LOAD			32000	/* uA */

/* NXP eUSB2 repeater registers */
#define LINK_CONTROL1			0x02
#define LINK_CONTROL2			0x03
#define REVISION_ID			0x13

/* TI eUSB2 repeater registers */
#define REV_ID				0xB0

/* registers 0..10 are saved at power-down; 2 and 3 (link control) are not written back */
#define EUSB2_NUM_SAVED_REGS		11

/* the two type-C ports (C1, C2) each have their own override sequence and saved registers */
#define EUSB2_PORTS			2

enum eusb2_repeater_type {
	TI_REPEATER,
	NXP_REPEATER,
};

struct i2c_repeater_chip {
	enum eusb2_repeater_type repeater_type;
};

struct eusb2_repeater {
	struct device			*dev;
	struct usb_repeater		ur;
	struct regmap			*regmap;
	const struct i2c_repeater_chip	*chip;
	u16				reg_base;
	struct regulator		*vdd18;
	struct regulator		*vdd3;
	bool				power_enabled;

	/* per port: dtsi sequence already written; registers saved at power-down, not yet restored */
	bool				seq_written[EUSB2_PORTS];
	bool				regs_saved[EUSB2_PORTS];

	struct gpio_desc		*reset_gpiod;
	u32				*param_override_seq_c1;
	u8				param_override_seq_cnt_c1;
	u32				*param_override_seq_c2;
	u8				param_override_seq_cnt_c2;
	u8				saved_regs[EUSB2_PORTS][EUSB2_NUM_SAVED_REGS];
};

/* exported by the stock ucsi_qti_glink.ko: a device is connected at type-C port C1 / C2 */

/* which port the last init wrote for (0 = C1, 1 = C2): power-down saves the registers of this one */
static u8 pre_present;

static const struct regmap_config eusb2_i2c_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static void eusb2_i2c_write_reg(struct eusb2_repeater *er, u8 reg, u8 val)
{
	int ret;

	ret = regmap_write(er->regmap, reg, val);
	if (ret < 0)
		dev_err(er->dev, "failed to write 0x%02x to reg: 0x%02x ret=%d\n", val, reg, ret);
}

/* the sequence is (value, register) pairs */
static void eusb2_repeater_update_seq(struct eusb2_repeater *er, const u32 *seq, u8 cnt)
{
	int i;

	for (i = 0; i < cnt; i += 2)
		eusb2_i2c_write_reg(er, seq[i + 1], seq[i]);
}

static int eusb2_repeater_power(struct eusb2_repeater *er, bool on)
{
	int ret = 0;

	if (on) {
		if (er->power_enabled)
			return 0;
	} else {
		/*
		 * Before the supplies go away, keep the register values of the port that was
		 * initialised (only once after the dtsi sequence has been written), so that the
		 * next init can put them back.
		 */
		int port = pre_present;

		if (er->seq_written[port] && !er->regs_saved[port]) {
			unsigned int val;
			int reg;

			for (reg = 0; reg < EUSB2_NUM_SAVED_REGS; reg++) {
				val = 0;
				ret = regmap_read(er->regmap, reg, &val);
				if (ret < 0)
					dev_err(er->dev, "Failed to read reg:0x%02x ret=%d\n", reg, ret);
				else
					er->saved_regs[port][reg] = val;
			}
			er->regs_saved[port] = true;
		}

		if (!er->power_enabled)
			return 0;
		goto disable_vdd3;
	}

	ret = regulator_set_load(er->vdd18, EUSB2_1P8_HPM_LOAD);
	if (ret < 0) {
		dev_err(er->ur.dev, "Unable to set HPM of vdd12:%d\n", ret);
		goto err_vdd18;
	}

	ret = regulator_set_voltage(er->vdd18, EUSB2_1P8_VOL_MIN,
						EUSB2_1P8_VOL_MAX);
	if (ret) {
		dev_err(er->ur.dev,
				"Unable to set voltage for vdd18:%d\n", ret);
		goto put_vdd18_lpm;
	}

	ret = regulator_enable(er->vdd18);
	if (ret) {
		dev_err(er->ur.dev, "Unable to enable vdd18:%d\n", ret);
		goto unset_vdd18;
	}

	ret = regulator_set_load(er->vdd3, EUSB2_3P0_HPM_LOAD);
	if (ret < 0) {
		dev_err(er->ur.dev, "Unable to set HPM of vdd3:%d\n", ret);
		goto disable_vdd18;
	}

	ret = regulator_set_voltage(er->vdd3, EUSB2_3P0_VOL_MIN,
						EUSB2_3P0_VOL_MAX);
	if (ret) {
		dev_err(er->ur.dev,
				"Unable to set voltage for vdd3:%d\n", ret);
		goto put_vdd3_lpm;
	}

	ret = regulator_enable(er->vdd3);
	if (ret) {
		dev_err(er->ur.dev, "Unable to enable vdd3:%d\n", ret);
		goto unset_vdd3;
	}

	er->power_enabled = true;
	return ret;

disable_vdd3:
	ret = regulator_disable(er->vdd3);
	if (ret)
		dev_err(er->ur.dev, "Unable to disable vdd3:%d\n", ret);

unset_vdd3:
	ret = regulator_set_voltage(er->vdd3, 0, EUSB2_3P0_VOL_MAX);
	if (ret)
		dev_err(er->ur.dev,
			"Unable to set (0) voltage for vdd3:%d\n", ret);

put_vdd3_lpm:
	ret = regulator_set_load(er->vdd3, 0);
	if (ret < 0)
		dev_err(er->ur.dev, "Unable to set (0) HPM of vdd3\n");

disable_vdd18:
	ret = regulator_disable(er->vdd18);
	if (ret)
		dev_err(er->ur.dev, "Unable to disable vdd18:%d\n", ret);

unset_vdd18:
	ret = regulator_set_voltage(er->vdd18, 0, EUSB2_1P8_VOL_MAX);
	if (ret)
		dev_err(er->ur.dev,
			"Unable to set (0) voltage for vdd18:%d\n", ret);

put_vdd18_lpm:
	ret = regulator_set_load(er->vdd18, 0);
	if (ret < 0)
		dev_err(er->ur.dev, "Unable to set LPM of vdd18\n");

	/* case handling when regulator turning on failed */
	if (!er->power_enabled)
		return -EINVAL;

err_vdd18:
	er->power_enabled = false;
	return ret;
}

/*
 * Apply the tuning of the type-C port that has a device (@n = 1 for C1, 2 for C2; @cn its name):
 *  - no override sequence in the devicetree: nothing to do;
 *  - first time: write the devicetree sequence;
 *  - later: only after a power-down that saved the registers, write those back (except
 *    registers 2 and 3); otherwise the repeater still has them, nothing is written.
 * The stock module has the C1 and the C2 case as two copies of the same code.
 */
#define EUSB2_APPLY_PORT(er, n, cn)								\
do {												\
	int reg;										\
												\
	dev_info((er)->ur.dev, "write " cn " sequence\n");					\
	if ((er)->param_override_seq_cnt_c##n) {						\
		if (!(er)->seq_written[(n) - 1]) {						\
			dev_info((er)->ur.dev, "write " cn " sequence from dtsi\n");		\
			eusb2_repeater_update_seq((er), (er)->param_override_seq_c##n,		\
						  (er)->param_override_seq_cnt_c##n);		\
			(er)->seq_written[(n) - 1] = true;					\
		} else if ((er)->regs_saved[(n) - 1]) {						\
			dev_info((er)->ur.dev, "write " cn " sequence from reg save value\n");	\
			for (reg = 0; reg < EUSB2_NUM_SAVED_REGS; reg++) {			\
				if (reg == LINK_CONTROL1 || reg == LINK_CONTROL2)		\
					continue;						\
				eusb2_i2c_write_reg((er), reg,					\
						    (er)->saved_regs[(n) - 1][reg]);		\
			}									\
			(er)->regs_saved[(n) - 1] = false;					\
		} else {									\
			dev_info((er)->ur.dev, "no need to write " cn " sequence\n");		\
		}										\
	}											\
	pre_present = (n) - 1;									\
} while (0)

static int eusb2_repeater_init(struct usb_repeater *ur)
{
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);
	const struct i2c_repeater_chip *chip = er->chip;
	unsigned int reg_val = 0;
	int ret;

	switch (chip->repeater_type) {
	case TI_REPEATER:
		ret = regmap_read(er->regmap, REV_ID, &reg_val);
		if (ret < 0) {
			dev_err(er->dev, "Failed to read reg:0x%02x ret=%d\n", REV_ID, ret);
			reg_val = 0;
		} else if ((u8)reg_val == 0x03) {
			/* If the repeater revision is B1 disable auto-resume WA */
			ur->flags |= UR_AUTO_RESUME_SUPPORTED;
		}
		break;
	case NXP_REPEATER:
		ret = regmap_read(er->regmap, REVISION_ID, &reg_val);
		if (ret < 0) {
			dev_err(er->dev, "Failed to read reg:0x%02x ret=%d\n", REVISION_ID, ret);
			reg_val = 0;
		}
		break;
	default:
		dev_err(er->ur.dev, "Invalid repeater\n");
	}

	dev_info(er->ur.dev,
		 "eUSB2 repeater version = 0x%x ur->flags:0x%x, C1_d_present:%d, C2_d_present:%d\n",
		 (u8)reg_val, ur->flags, C1_d_present, C2_d_present);

	/* C1 wins when both ports have a device; with neither, nothing is written */
	if (C1_d_present)
		EUSB2_APPLY_PORT(er, 1, "C1");
	else if (C2_d_present)
		EUSB2_APPLY_PORT(er, 2, "C2");

	dev_info(er->ur.dev, "eUSB2 repeater init\n");

	return 0;
}

static int eusb2_repeater_reset(struct usb_repeater *ur, bool bring_out_of_reset)
{
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);

	gpiod_set_value_cansleep(er->reset_gpiod, bring_out_of_reset);
	return 0;
}

static int eusb2_repeater_powerup(struct usb_repeater *ur)
{
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);

	return eusb2_repeater_power(er, true);
}

static int eusb2_repeater_powerdown(struct usb_repeater *ur)
{
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);

	return eusb2_repeater_power(er, false);
}

static struct i2c_repeater_chip repeater_chip[] = {
	[NXP_REPEATER] = {
		.repeater_type = NXP_REPEATER,
	},
	[TI_REPEATER] = {
		.repeater_type = TI_REPEATER,
	}
};

static const struct of_device_id eusb2_repeater_id_table[] = {
	{
		.compatible = "nxp,eusb2-repeater",
		.data = &repeater_chip[NXP_REPEATER]
	},
	{
		.compatible = "ti,eusb2-repeater",
		.data = &repeater_chip[TI_REPEATER]
	},
	{ },
};
MODULE_DEVICE_TABLE(of, eusb2_repeater_id_table);

static int eusb2_repeater_i2c_probe(struct i2c_client *client)
{
	struct eusb2_repeater *er;
	struct device *dev = &client->dev;
	const struct of_device_id *match;
	int ret = 0, num_elem;

	er = devm_kzalloc(dev, sizeof(*er), GFP_KERNEL);
	if (!er)
		return -ENOMEM;

	er->dev = dev;
	match = of_match_node(eusb2_repeater_id_table, dev->of_node);
	er->chip = match->data;

	er->regmap = devm_regmap_init_i2c(client, &eusb2_i2c_regmap);
	if (IS_ERR(er->regmap)) {
		dev_err(dev, "failed to allocate register map\n");
		return PTR_ERR(er->regmap);
	}

	devm_regmap_qti_debugfs_register(er->dev, er->regmap);
	i2c_set_clientdata(client, er);

	ret = of_property_read_u16(dev->of_node, "reg", &er->reg_base);
	if (ret < 0) {
		dev_err(dev, "failed to get reg base address:%d\n", ret);
		return ret;
	}

	er->vdd3 = devm_regulator_get(dev, "vdd3");
	if (IS_ERR(er->vdd3)) {
		dev_err(dev, "unable to get vdd3 supply\n");
		return PTR_ERR(er->vdd3);
	}

	er->vdd18 = devm_regulator_get(dev, "vdd18");
	if (IS_ERR(er->vdd18)) {
		dev_err(dev, "unable to get vdd18 supply\n");
		return PTR_ERR(er->vdd18);
	}

	er->reset_gpiod = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(er->reset_gpiod))
		return PTR_ERR(er->reset_gpiod);

	num_elem = of_property_count_elems_of_size(dev->of_node, "qcom,param-override-seq-c1",
				sizeof(*er->param_override_seq_c1));
	dev_err(dev, "eusb2_repeater_i2c:num_elem:%d\n", num_elem);	/* leftover debug print, error level */
	if (num_elem > 0) {
		if (num_elem % 2) {
			dev_err(dev, "invalid param_override_seq_len_c1\n");
			return -EINVAL;
		}
		dev_info(dev, "get c1 param override seq from dtsi\n");
		er->param_override_seq_cnt_c1 = num_elem;
		er->param_override_seq_c1 = devm_kcalloc(dev,
				er->param_override_seq_cnt_c1,
				sizeof(*er->param_override_seq_c1), GFP_KERNEL);
		if (!er->param_override_seq_c1)
			return -ENOMEM;

		ret = of_property_read_u32_array(dev->of_node,
				"qcom,param-override-seq-c1",
				er->param_override_seq_c1,
				er->param_override_seq_cnt_c1);
		if (ret) {
			dev_err(dev, "qcom,param-override-seq-c1 read failed %d\n", ret);
			return ret;
		}
	}

	num_elem = of_property_count_elems_of_size(dev->of_node, "qcom,param-override-seq-c2",
				sizeof(*er->param_override_seq_c2));
	if (num_elem > 0) {
		if (num_elem % 2) {
			dev_err(dev, "invalid param_override_seq_len_c2\n");
			return -EINVAL;
		}
		dev_info(dev, "get c2 param override seq from dtsi\n");
		er->param_override_seq_cnt_c2 = num_elem;
		er->param_override_seq_c2 = devm_kcalloc(dev,
				er->param_override_seq_cnt_c2,
				sizeof(*er->param_override_seq_c2), GFP_KERNEL);
		if (!er->param_override_seq_c2)
			return -ENOMEM;

		ret = of_property_read_u32_array(dev->of_node,
				"qcom,param-override-seq-c2",
				er->param_override_seq_c2,
				er->param_override_seq_cnt_c2);
		if (ret) {
			dev_err(dev, "qcom,param-override-seq-c2 read failed %d\n", ret);
			return ret;
		}
	}

	er->ur.dev = dev;

	er->ur.init		= eusb2_repeater_init;
	er->ur.reset		= eusb2_repeater_reset;
	er->ur.powerup		= eusb2_repeater_powerup;
	er->ur.powerdown	= eusb2_repeater_powerdown;

	ret = usb_add_repeater_dev(&er->ur);
	if (ret)
		return ret;

	dev_err(dev, "nxp probe success\n");	/* error level in the stock module too */
	return 0;
}

static void eusb2_repeater_i2c_remove(struct i2c_client *client)
{
	struct eusb2_repeater *er = i2c_get_clientdata(client);

	if (!er)
		return;

	usb_remove_repeater_dev(&er->ur);
	eusb2_repeater_power(er, false);
}

static struct i2c_driver eusb2_i2c_repeater_driver = {
	.probe	= eusb2_repeater_i2c_probe,
	.remove	= eusb2_repeater_i2c_remove,
	.driver = {
		.name	= "eusb2-repeater-nxp",
		.of_match_table = of_match_ptr(eusb2_repeater_id_table),
	},
};

module_i2c_driver(eusb2_i2c_repeater_driver);

MODULE_DESCRIPTION("eUSB2 i2c repeater driver");
MODULE_LICENSE("GPL v2");
