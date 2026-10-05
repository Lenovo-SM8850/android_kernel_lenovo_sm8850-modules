// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb/lenovo_ucsi.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_mux.h>
#include <linux/soc/qcom/fsa4480-i2c.h>
#include <linux/delay.h>
#include <linux/qti-regmap-debugfs.h>

#define FSA4480_SUB_I2C_NAME	"fsa4480_sub-driver"

#define FSA4480_SUB_SWITCH_SETTINGS 0x04
#define FSA4480_SUB_SWITCH_CONTROL  0x05
#define FSA4480_SUB_SWITCH_STATUS1  0x07
#define FSA4480_SUB_SLOW_L          0x08
#define FSA4480_SUB_SLOW_R          0x09
#define FSA4480_SUB_SLOW_MIC        0x0A
#define FSA4480_SUB_SLOW_SENSE      0x0B
#define FSA4480_SUB_SLOW_GND        0x0C
#define FSA4480_SUB_DELAY_L_R       0x0D
#define FSA4480_SUB_DELAY_L_MIC     0x0E
#define FSA4480_SUB_DELAY_L_SENSE   0x0F
#define FSA4480_SUB_DELAY_L_AGND    0x10
#define FSA4480_SUB_RESET           0x1E

bool fsa4480_switch_C2_enable = false;
EXPORT_SYMBOL(fsa4480_switch_C2_enable);

int direction2_value;

struct fsa4480_sub_reg_val {
	u16 reg;
	u8 val;
};

static const struct regmap_config fsa4480_sub_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = FSA4480_SUB_RESET,
};

static const struct fsa4480_sub_reg_val fsa_reg_i2c_defaults[] = {
	{FSA4480_SUB_SLOW_L, 0x00},
	{FSA4480_SUB_SLOW_R, 0x00},
	{FSA4480_SUB_SLOW_MIC, 0x00},
	{FSA4480_SUB_SLOW_SENSE, 0x00},
	{FSA4480_SUB_SLOW_GND, 0x00},
	{FSA4480_SUB_DELAY_L_R, 0x00},
	{FSA4480_SUB_DELAY_L_MIC, 0x00},
	{FSA4480_SUB_DELAY_L_SENSE, 0x00},
	{FSA4480_SUB_DELAY_L_AGND, 0x09},
	{FSA4480_SUB_SWITCH_SETTINGS, 0x98},
};

static void fsa4480_sub_usbc_update_settings(struct fsa4480_sub_priv *fsa_priv,
		u32 switch_control, u32 switch_enable)
{
	u32 prev_control = 0, prev_enable = 0;

	if (!fsa_priv->regmap) {
		dev_err(fsa_priv->dev, "%s: regmap invalid\n", __func__);
		return;
	}

	regmap_read(fsa_priv->regmap, FSA4480_SUB_SWITCH_CONTROL, &prev_control);
	regmap_read(fsa_priv->regmap, FSA4480_SUB_SWITCH_SETTINGS, &prev_enable);
	if (prev_control == switch_control && prev_enable == switch_enable) {
		return;
	}

	regmap_write(fsa_priv->regmap, FSA4480_SUB_SWITCH_SETTINGS, 0x80);
	regmap_write(fsa_priv->regmap, FSA4480_SUB_SWITCH_CONTROL, switch_control);
	/* FSA4480 chip hardware requirement */
	usleep_range(50, 55);
	regmap_write(fsa_priv->regmap, FSA4480_SUB_SWITCH_SETTINGS, switch_enable);
}

static int fsa4480_sub_usbc_analog_setup_switches(
						struct fsa4480_sub_priv *fsa_priv)
{
	int rc = 0;
	int mode;
	struct device *dev;
	u32 reg6 = 0, reg4 = 0;
	u32 reg17 = 0;
	u32 value = 0;
	int i = 0;
    u32 reg0 = 0;

	if (!fsa_priv)
		return -EINVAL;
	dev = fsa_priv->dev;
	if (!dev)
		return -EINVAL;

	mutex_lock(&fsa4480_state_lock);
	if (fsa_priv->removing) {
		mutex_unlock(&fsa4480_state_lock);
		return -ENODEV;
	}
	/* get latest mode again within locked context */
	mode = atomic_read(&(fsa_priv->usbc_mode));

	printk("%s: setting GPIOs active = %d\n",
		__func__, mode != TYPEC_ACCESSORY_NONE);

	switch (mode) {
	/* add all modes FSA should notify for in here */
	case TYPEC_ACCESSORY_AUDIO:
		/* activate switches */
		regmap_write(fsa_priv->regmap, FSA4480_SUB_RESET, 0x01);
		regmap_write(fsa_priv->regmap, FSA4480_SUB_SLOW_L, 0x4f);
		regmap_read(fsa_priv->regmap, FSA4480_SUB_SLOW_L, &value);
		dev_info(fsa_priv->dev, "%s: reg[0x08]=0x%x\n",__func__, value);
		regmap_write(fsa_priv->regmap, FSA4480_SUB_SLOW_R, 0x4f);
		regmap_read(fsa_priv->regmap, FSA4480_SUB_SLOW_R, &value);
		dev_info(fsa_priv->dev, "%s: reg[0x09]=0x%x\n",__func__, value);
		regmap_write(fsa_priv->regmap, FSA4480_SUB_SLOW_MIC, 0x4f);
		regmap_read(fsa_priv->regmap, FSA4480_SUB_SLOW_MIC, &value);
		dev_info(fsa_priv->dev, "%s: reg[0x0a]=0x%x\n",__func__, value);
		regmap_write(fsa_priv->regmap, FSA4480_SUB_SLOW_SENSE, 0x4f);
		regmap_read(fsa_priv->regmap, FSA4480_SUB_SLOW_SENSE, &value);
		dev_info(fsa_priv->dev, "%s: reg[0x0b]=0x%x\n",__func__, value);
		regmap_write(fsa_priv->regmap, FSA4480_SUB_SLOW_GND, 0x4f);
		regmap_read(fsa_priv->regmap, FSA4480_SUB_SLOW_GND, &value);
		dev_info(fsa_priv->dev, "%s: reg[0x0c]=0x%x\n",__func__, value);

		regmap_read(fsa_priv->regmap, 0x00, &reg0);
		if (reg0 == 0xf1)
			regmap_write(fsa_priv->regmap, 0x1c, 0x00);

        regmap_write(fsa_priv->regmap, 0x04, 0x9f);
        dev_info(fsa_priv->dev, "%s: set reg[0x04] done.\n",__func__);
        regmap_read(fsa_priv->regmap, 0x04, &value);
        dev_info(fsa_priv->dev, "%s: reg[0x04]=0x%x\n",__func__, value);
 
        regmap_write(fsa_priv->regmap, 0x05, 0x00);
        dev_info(fsa_priv->dev, "%s: set reg[0x05] done.\n",__func__);
        regmap_read(fsa_priv->regmap, 0x05, &value);
        dev_info(fsa_priv->dev, "%s: reg[0x05]=0x%x\n",__func__, value);
 
        regmap_write(fsa_priv->regmap, 0x12, 0x0d);
        dev_info(fsa_priv->dev, "%s: set reg[0x12] done.\n",__func__);
        regmap_read(fsa_priv->regmap, 0x12, &value);
        dev_info(fsa_priv->dev, "%s: reg[0x12]=0x%x\n",__func__, value);
 
        for (i = 0; i < 10; i++) {
            msleep(5);
            if (value == 0x0c)
                break;
            regmap_read(fsa_priv->regmap, 0x12, &value);
            dev_info(fsa_priv->dev, "%s: reg[0x12]=0x%x\n",__func__, value);
        }

		msleep(1000);

		regmap_read(fsa_priv->regmap, 0x17, &reg17);
		dev_info(fsa_priv->dev, "%s: reg[0x17]=0x%x\n",__func__, reg17);
		regmap_read(fsa_priv->regmap, 0x06, &reg6);
		regmap_read(fsa_priv->regmap, 0x04, &reg4);
		dev_info(fsa_priv->dev, "%s: reg4_sub: %d, reg_sub_value: %d\n",
			 __func__, reg4, reg6);
        regmap_read(fsa_priv->regmap, 0x00, &reg0);
		if(reg17 == 0x01 || reg6 == 0x05){
                    regmap_write(fsa_priv->regmap, FSA4480_SUB_RESET, 0x01);
		    fsa4480_sub_usbc_update_settings(fsa_priv, 0x00, 0x9F);
		    msleep(5);
		}
		if(reg17 == 0x02){
		    regmap_write(fsa_priv->regmap, FSA4480_SUB_SWITCH_SETTINGS, 0x87);
			msleep(1);
			regmap_write(fsa_priv->regmap, FSA4480_SUB_SWITCH_CONTROL, 0x07);
			msleep(5);
			regmap_write(fsa_priv->regmap, FSA4480_SUB_SWITCH_SETTINGS, 0x9f);
			msleep(5);
		}

		fsa4480_switch_C2_enable = true;
		direction2_value = reg6 == 0x1a ? 1 : reg6 == 0x2a ? 2 : 0;
		regmap_read(fsa_priv->regmap, 0x06, &reg6);
		regmap_read(fsa_priv->regmap, 0x04, &reg4);
		dev_info(fsa_priv->dev, "%s: reg4_sub: %d, reg_sub_value: %d\n",
			 __func__, reg4, reg6);
		if (reg4 == 0x9f) {
			if (reg6 == 0x05 || reg6 == 0x1a)
				direction2_value = 1;
			else if (reg6 == 0x2a)
				direction2_value = 2;
			else
				direction2_value = 0;
		} else {
			direction2_value = 0;
		}

		/* notify call chain on event */
		mutex_unlock(&fsa4480_state_lock);
		blocking_notifier_call_chain(&fsa_priv->fsa4480_sub_notifier,
					     mode, NULL);
		mutex_lock(&fsa4480_state_lock);
		break;
	case TYPEC_ACCESSORY_NONE:
		fsa4480_switch_C2_enable = false;

		/* notify call chain on event */
		mutex_unlock(&fsa4480_state_lock);
		blocking_notifier_call_chain(&fsa_priv->fsa4480_sub_notifier,
				TYPEC_ACCESSORY_NONE, NULL);
		mutex_lock(&fsa4480_state_lock);

		/* deactivate switches */
	    regmap_write(fsa_priv->regmap, FSA4480_SUB_RESET, 0x01);
		fsa4480_sub_usbc_update_settings(fsa_priv, 0x18, 0x98);
		dev_info(fsa_priv->dev, "%s: TYPEC_ACCESSORY_NONE",__func__);

		break;
	default:
		/* ignore other usb connection modes */
		break;
	}

	mutex_unlock(&fsa4480_state_lock);
	return rc;
}

/*
 * fsa4480_sub_reg_notifier - register notifier block with fsa driver
 *
 * @nb - notifier block of fsa4480
 * @node - phandle node to fsa4480 device
 *
 * Returns 0 on success, or error code
 */
int fsa4480_sub_reg_notifier(struct notifier_block *nb,
			 struct device_node *node)
{
	int rc = 0;
	struct i2c_client *client = of_find_i2c_device_by_node(node);
	struct fsa4480_sub_priv *fsa_priv;

	if (!client)
		return -EPROBE_DEFER;
	down_read(&fsa4480_api_lock);
	mutex_lock(&fsa4480_state_lock);

	fsa_priv = (struct fsa4480_sub_priv *)i2c_get_clientdata(client);
	if (!fsa_priv || fsa_priv->removing) {
		rc = -EPROBE_DEFER;
		goto out;
	}

	mutex_unlock(&fsa4480_state_lock);
	rc = blocking_notifier_chain_register
				(&fsa_priv->fsa4480_sub_notifier, nb);

	if (rc) {
		mutex_lock(&fsa4480_state_lock);
		goto out;
	}

	/*
	 * as part of the init sequence check if there is a connected
	 * USB C analog adapter
	 */
	if (atomic_read(&(fsa_priv->usbc_mode)) == TYPEC_ACCESSORY_AUDIO) {
		rc = fsa4480_sub_usbc_analog_setup_switches(fsa_priv);
	}

	if (rc)
		blocking_notifier_chain_unregister(&fsa_priv->fsa4480_sub_notifier, nb);
	mutex_lock(&fsa4480_state_lock);
out:
	mutex_unlock(&fsa4480_state_lock);
	up_read(&fsa4480_api_lock);
	put_device(&client->dev);
	return rc;
}
EXPORT_SYMBOL_GPL(fsa4480_sub_reg_notifier);

/*
 * fsa4480_sub_unreg_notifier - unregister notifier block with fsa driver
 *
 * @nb - notifier block of fsa4480
 * @node - phandle node to fsa4480 device
 *
 * Returns 0 on pass, or error code
 */
int fsa4480_sub_unreg_notifier(struct notifier_block *nb,
			     struct device_node *node)
{
	int rc = 0;
	struct i2c_client *client = of_find_i2c_device_by_node(node);
	struct fsa4480_sub_priv *fsa_priv;

	if (!client)
		return -EPROBE_DEFER;
	down_read(&fsa4480_api_lock);
	mutex_lock(&fsa4480_state_lock);
	fsa_priv = i2c_get_clientdata(client);
	if (!fsa_priv) {
		rc = -EPROBE_DEFER;
		goto out;
	}
	fsa4480_sub_usbc_update_settings(fsa_priv, 0x18, 0x98);
	mutex_unlock(&fsa4480_state_lock);
	rc = blocking_notifier_chain_unregister(&fsa_priv->fsa4480_sub_notifier, nb);

	mutex_lock(&fsa4480_state_lock);
out:
	mutex_unlock(&fsa4480_state_lock);
	up_read(&fsa4480_api_lock);
	put_device(&client->dev);
	return rc;
}
EXPORT_SYMBOL_GPL(fsa4480_sub_unreg_notifier);

static int fsa4480_sub_validate_display_port_settings(struct fsa4480_sub_priv *fsa_priv)
{
	u32 switch_status = 0;

	regmap_read(fsa_priv->regmap, FSA4480_SUB_SWITCH_STATUS1, &switch_status);

	if ((switch_status != 0x23) && (switch_status != 0x1C)) {
		pr_err("AUX SBU1/2 switch status is invalid = %u\n",
				switch_status);
		return -EIO;
	}

	return 0;
}
/*
 * fsa4480_sub_switch_event - configure FSA switch position based on event
 *
 * @node - phandle node to fsa4480 device
 * @event - fsa_function enum
 *
 * Returns int on whether the switch happened or not
 */
int fsa4480_sub_switch_event(struct device_node *node,
			 enum fsa_function event)
{
	int rc = 0;
	int switch_control = 0;
	struct i2c_client *client = of_find_i2c_device_by_node(node);
	struct fsa4480_sub_priv *fsa_priv;

	if (!client)
		return -EPROBE_DEFER;
	mutex_lock(&fsa4480_state_lock);

	fsa_priv = (struct fsa4480_sub_priv *)i2c_get_clientdata(client);
	if (!fsa_priv || fsa_priv->removing) {
		rc = -EPROBE_DEFER;
		goto out;
	}
	if (!fsa_priv->regmap) {
		rc = -EINVAL;
		goto out;
	}

    pr_info("%s: event = %d\n", __func__, event);

	switch (event) {
	case FSA_MIC_GND_SWAP:
		regmap_read(fsa_priv->regmap, FSA4480_SUB_SWITCH_CONTROL,
				&switch_control);
		if ((switch_control & 0x07) == 0x07)
			switch_control = 0x0;
		else
			switch_control = 0x7;
		fsa4480_sub_usbc_update_settings(fsa_priv, switch_control, 0x9F);
		break;
	case FSA_USBC_ORIENTATION_CC1:
		fsa4480_sub_usbc_update_settings(fsa_priv, 0x18, 0xF8);
		rc = fsa4480_sub_validate_display_port_settings(fsa_priv);
		break;
	case FSA_USBC_ORIENTATION_CC2:
		fsa4480_sub_usbc_update_settings(fsa_priv, 0x78, 0xF8);
		rc = fsa4480_sub_validate_display_port_settings(fsa_priv);
		break;
	case FSA_USBC_DISPLAYPORT_DISCONNECTED:
		fsa4480_sub_usbc_update_settings(fsa_priv, 0x18, 0x98);
		break;
	default:
		break;
	}

out:
	mutex_unlock(&fsa4480_state_lock);
	put_device(&client->dev);
	return rc;
}
EXPORT_SYMBOL_GPL(fsa4480_sub_switch_event);

static void fsa4480_sub_usbc_analog_work_fn(struct work_struct *work)
{
	struct fsa4480_sub_priv *fsa_priv =
		container_of(work, struct fsa4480_sub_priv, usbc_analog_work);

	if (!fsa_priv) {
		pr_err("%s: fsa container invalid\n", __func__);
		return;
	}
	down_read(&fsa4480_api_lock);
	fsa4480_sub_usbc_analog_setup_switches(fsa_priv);
	up_read(&fsa4480_api_lock);
	pm_relax(fsa_priv->dev);
}

static void fsa4480_sub_update_reg_defaults(struct regmap *regmap)
{
	u8 i;

	for (i = 0; i < ARRAY_SIZE(fsa_reg_i2c_defaults); i++)
		regmap_write(regmap, fsa_reg_i2c_defaults[i].reg,
				   fsa_reg_i2c_defaults[i].val);
}

static int fsa4480_sub_usbc_event_changed(struct typec_mux_dev *mux,
					    struct typec_mux_state *state)
{
	struct fsa4480_sub_priv *fsa_priv = typec_mux_get_drvdata(mux);
	enum typec_accessory acc;
	unsigned long mode;

	if (!fsa_priv || !fsa_priv->dev)
		return -EINVAL;
	mutex_lock(&fsa4480_state_lock);
	if (fsa_priv->removing)
		goto out;
	mode = state->mode;
	acc = mode == TYPEC_MODE_AUDIO ? TYPEC_ACCESSORY_AUDIO :
	      mode == TYPEC_MODE_DEBUG ? TYPEC_ACCESSORY_DEBUG :
	      TYPEC_ACCESSORY_NONE;
	printk("%s: USB change event received, supply mode %d, usbc mode %d, expected %d cc_port_num %d\n",
	       __func__, acc, atomic_read(&fsa_priv->usbc_mode),
	       TYPEC_ACCESSORY_AUDIO, cc_port_num);
	if (acc > TYPEC_ACCESSORY_AUDIO || cc_port_num != 2)
		goto out;
	if (atomic_read(&fsa_priv->usbc_mode) == acc)
		goto out;
	atomic_set(&fsa_priv->usbc_mode, acc);
	if (mode == TYPEC_MODE_AUDIO)
		audio_4480_headset_ref++;
	else if ((mode < TYPEC_MODE_AUDIO || mode > TYPEC_MODE_DEBUG) &&
		 audio_4480_headset_ref > 0)
		audio_4480_headset_ref--;
	pm_stay_awake(fsa_priv->dev);
	queue_work(system_freezable_wq, &fsa_priv->usbc_analog_work);
out:
	mutex_unlock(&fsa4480_state_lock);
	return 0;
}

static ssize_t direction2_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct fsa4480_sub_priv *fsa_priv = dev_get_drvdata(dev);
	u32 reg6 = 0, reg4 = 0;
	mutex_lock(&fsa4480_state_lock);

	regmap_read(fsa_priv->regmap, 0x06, &reg6);
	regmap_read(fsa_priv->regmap, 0x04, &reg4);
	dev_info(fsa_priv->dev, "%s: reg4_sub: %d, reg_sub_value: %d\n",
		 __func__, reg4, reg6);
	direction2_value = reg6 == 0x1a ? 1 : reg6 == 0x2a ? 2 : 0;
	mutex_unlock(&fsa4480_state_lock);
	return snprintf(buf, PAGE_SIZE, "%d\n", direction2_value);
}

static ssize_t direction2_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct fsa4480_sub_priv *fsa_priv = dev_get_drvdata(dev);
	u32 databuf[2] = { 0, 0 };
	mutex_lock(&fsa4480_state_lock);

	if (sscanf(buf, "%x %x", &databuf[0], &databuf[1]) == 2)
		regmap_write(fsa_priv->regmap, databuf[0], databuf[1]);
	dev_info(fsa_priv->dev, "%s: databuf[0]:%x, databuf[1]:%x\n",
		 __func__, databuf[0], databuf[1]);
	mutex_unlock(&fsa4480_state_lock);
	return count;
}

static ssize_t reg_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct fsa4480_sub_priv *fsa_priv = dev_get_drvdata(dev);
	u32 value = 0;
	ssize_t len = 0;
	unsigned int reg;
	mutex_lock(&fsa4480_state_lock);

	if (!fsa_priv->regmap)
		printk("%s: regmap invalid\n", __func__);
	for (reg = 0x04; reg <= FSA4480_SUB_RESET; reg++) {
		regmap_read(fsa_priv->regmap, reg, &value);
		len += snprintf(buf + len, PAGE_SIZE - len,
				"reg[0x%x]=0x%x.\n", reg, value);
	}
	mutex_unlock(&fsa4480_state_lock);
	return len;
}

static DEVICE_ATTR(direction2, 0644, direction2_show, direction2_store);
static DEVICE_ATTR(reg, 0444, reg_show, NULL);
static struct attribute *fsa4480_sub_attributes[] = {
	&dev_attr_direction2.attr,
	&dev_attr_reg.attr,
	NULL,
};
static const struct attribute_group fsa4480_sub_attribute_group = {
	.attrs = fsa4480_sub_attributes,
};

static int fsa4480_sub_probe(struct i2c_client *i2c)
{
	struct fsa4480_sub_priv *fsa_priv;
	struct typec_mux_desc mux_desc = { };
	int rc = 0;

	fsa_priv = devm_kzalloc(&i2c->dev, sizeof(*fsa_priv), GFP_KERNEL);
	if (!fsa_priv)
		return -ENOMEM;
	fsa_priv->dev = &i2c->dev;
	mutex_init(&fsa_priv->notification_lock);
	INIT_WORK(&fsa_priv->usbc_analog_work, fsa4480_sub_usbc_analog_work_fn);
	BLOCKING_INIT_NOTIFIER_HEAD(&fsa_priv->fsa4480_sub_notifier);

	fsa_priv->regmap = devm_regmap_init_i2c(i2c, &fsa4480_sub_regmap_config);
	if (IS_ERR_OR_NULL(fsa_priv->regmap)) {
		dev_err(fsa_priv->dev, "%s: Failed to initialize regmap: %d\n",
			__func__, rc);
		return fsa_priv->regmap ? PTR_ERR(fsa_priv->regmap) : -EINVAL;
	}
	fsa4480_sub_update_reg_defaults(fsa_priv->regmap);
	devm_regmap_qti_debugfs_register(fsa_priv->dev, fsa_priv->regmap);
	mux_desc.drvdata = fsa_priv;
	mux_desc.fwnode = dev_fwnode(fsa_priv->dev);
	mux_desc.set = fsa4480_sub_usbc_event_changed;
	fsa_priv->mux = typec_mux_register(fsa_priv->dev, &mux_desc);
	if (IS_ERR(fsa_priv->mux))
		return dev_err_probe(fsa_priv->dev, PTR_ERR(fsa_priv->mux),
				     "failed to register typec mux\n");
	mutex_lock(&fsa4480_state_lock);
	i2c_set_clientdata(i2c, fsa_priv);
	mutex_unlock(&fsa4480_state_lock);
	rc = sysfs_create_group(&i2c->dev.kobj, &fsa4480_sub_attribute_group);
	if (rc < 0)
		dev_err(fsa_priv->dev, "error creating sysfs sub attr files");
	mutex_lock(&fsa4480_state_lock);
	audio_4480_priv2 = fsa_priv;
	mutex_unlock(&fsa4480_state_lock);
	return 0;
}

static void fsa4480_sub_remove(struct i2c_client *i2c)
{
	struct fsa4480_sub_priv *fsa_priv = i2c_get_clientdata(i2c);

	if (!fsa_priv)
		return;
	mutex_lock(&fsa4480_state_lock);
	fsa_priv->removing = true;
	audio_4480_priv2 = NULL;
	if (atomic_read(&fsa_priv->usbc_mode) == TYPEC_ACCESSORY_AUDIO &&
	    audio_4480_headset_ref)
		audio_4480_headset_ref--;
	mutex_unlock(&fsa4480_state_lock);
	typec_mux_unregister(fsa_priv->mux);
	cancel_work_sync(&fsa_priv->usbc_analog_work);
	pm_relax(fsa_priv->dev);
	sysfs_remove_group(&i2c->dev.kobj, &fsa4480_sub_attribute_group);
	down_write(&fsa4480_api_lock);
	mutex_lock(&fsa4480_state_lock);
	fsa4480_sub_usbc_update_settings(fsa_priv, 0x18, 0x98);
	i2c_set_clientdata(i2c, NULL);
	mutex_unlock(&fsa4480_state_lock);
	up_write(&fsa4480_api_lock);
	mutex_destroy(&fsa_priv->notification_lock);
}

static const struct of_device_id fsa4480_sub_i2c_dt_match[] = {
	{
		.compatible = "qcom,fsa4480-sub-i2c",
	},
	{}
};

static struct i2c_driver fsa4480_sub_i2c_driver = {
	.driver = {
		.name = FSA4480_SUB_I2C_NAME,
		.of_match_table = fsa4480_sub_i2c_dt_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = fsa4480_sub_probe,
	.remove = fsa4480_sub_remove,
};

static int __init fsa4480_sub_init(void)
{
	int rc;

	rc = i2c_add_driver(&fsa4480_sub_i2c_driver);
	if (rc)
		pr_err("fsa4480_sub: Failed to register I2C driver: %d\n", rc);

	return rc;
}
module_init(fsa4480_sub_init);

static void __exit fsa4480_sub_exit(void)
{
	i2c_del_driver(&fsa4480_sub_i2c_driver);
}
module_exit(fsa4480_sub_exit);

MODULE_DESCRIPTION("FSA4480_SUB I2C driver");
MODULE_LICENSE("GPL");
