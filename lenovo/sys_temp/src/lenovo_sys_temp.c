// SPDX-License-Identifier: GPL-2.0-only
/* TB323FU (baldur) model recovered from Lenovo's stock 6.12 module.
 * Temperatures are millicelsius. Do not substitute another product's model.
 */
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#define INPUT_COUNT 15
#define OUTPUT_COUNT 5
#define UPDATE_MS 5000
#define INIT_ATTEMPTS 10

static const char * const input_names[INPUT_COUNT] = {
	"ap-therm", "flash-led-ntc", "wlan-therm", "usb2-conn-therm",
	"rear-cam-ntc", "xo-therm", "quiet-therm", "fast-chg-therm",
	"ufs-therm", "fcam-ntc", "top-chg-therm", "batt2-pack-therm",
	"batt-pack-therm", "usb1-conn-therm", "lcm-thermal",
};
static const char * const output_names[OUTPUT_COUNT] = {
	"front_temp", "back_temp", "user_temp", "user_front_temp", "user_back_temp",
};

/* MODEL_BEGIN: also compiled by the offline validation harness. */
/* Rows: front, back, user back left, user back right, user front.
 * Corresponding stock signed multiply addresses: c84,d60,d6c,d80,d88.
 */
static const s32 coefficients[5][INPUT_COUNT] = {
	{ -225, -136, 282, -142, 2359, 222, -1219, 0, 737, -578, -657, -1233, 130, 1293, 0 },
	{ 77, -351, 332, -17, 1944, -1495, 45, 0, 680, -141, -867, -1185, 534, 1266, 0 },
	{ -103, -458, -192, 127, 1767, -1214, 384, 0, 1173, -683, -459, -946, 429, 948, 0 },
	{ -110, -286, -191, 143, 1499, -995, 435, 0, 1136, -718, -417, -1111, 505, 905, 0 },
	{ -278, 250, 404, 684, 484, 2594, -791, 0, 190, -1758, -569, -1685, -785, 2232, 0 },
};
static const s32 intercepts[5] = {
	7074889, 6913913, 7496443, 6847569, 1657885,
};

static void baldur_model(const int input[INPUT_COUNT], int output[OUTPUT_COUNT])
{
	int model[5], i, j;

	for (i = 0; i < 5; i++) {
		/* Stock uses 32-bit MUL/MADD, then signed /1000 (toward zero).
		 * Unsigned arithmetic preserves that wrap without signed C overflow.
		 */
		u32 sum = (u32)intercepts[i];

		for (j = 0; j < INPUT_COUNT; j++)
			sum += (u32)input[j] * (u32)coefficients[i][j];
		model[i] = (s32)sum / 1000;
	}
	output[0] = model[0];
	output[2] = max(model[4], max(model[2], model[3]));
	output[1] = max(max(model[0], model[1]), output[2]);
	output[3] = model[4];
	output[4] = max(model[2], model[3]) + 1000;
}
/* MODEL_END */

struct sys_temp_model {
	const char * const *input_names;
	const char * const *output_names;
	void (*evaluate)(const int input[INPUT_COUNT], int output[OUTPUT_COUNT]);
};

static const struct sys_temp_model baldur_sys_temp_model = {
	.input_names = input_names,
	.output_names = output_names,
	.evaluate = baldur_model,
};

struct sys_temp;
struct sys_temp_sensor {
	struct sys_temp *owner;
	struct thermal_zone_device *zone;
	int temp;
};
struct sys_temp {
	struct device *dev;
	const struct sys_temp_model *model;
	struct sys_temp_sensor input[INPUT_COUNT];
	struct sys_temp_sensor output[OUTPUT_COUNT];
	struct delayed_work work;
	struct mutex lock;
	unsigned int attempts;
	bool ready;
	bool stopping;
};

static int lenovo_sys_temp_get(struct thermal_zone_device *zone, int *temp)
{
	struct sys_temp_sensor *sensor = thermal_zone_device_priv(zone);

	if (!sensor)
		return -EINVAL;
	mutex_lock(&sensor->owner->lock);
	*temp = sensor->temp;
	mutex_unlock(&sensor->owner->lock);
	return 0;
}

static const struct thermal_zone_device_ops lenovo_sys_temp_ops = {
	.get_temp = lenovo_sys_temp_get,
};

static void temp_changed_work_func(struct work_struct *work)
{
	struct sys_temp *dev = container_of(to_delayed_work(work), struct sys_temp, work);
	int input[INPUT_COUNT], output[OUTPUT_COUNT], i, ret;

	/* Stock retries listener discovery ten times, at five-second intervals.
	 * After that it keeps scheduling but leaves the initial zero outputs.
	 */
	if (!dev->ready) {
		if (++dev->attempts > INIT_ATTEMPTS)
			goto reschedule;
		for (i = 0; i < INPUT_COUNT; i++) {
			dev->input[i].temp = 0;
			dev->input[i].zone = thermal_zone_get_zone_by_name(dev->model->input_names[i]);
			if (IS_ERR(dev->input[i].zone)) {
				dev_err(dev->dev, "thermal zone %s not available: %ld\n",
					dev->model->input_names[i], PTR_ERR(dev->input[i].zone));
				goto reschedule;
			}
		}
		dev->ready = true;
	}
	for (i = 0; i < INPUT_COUNT; i++) {
		ret = thermal_zone_get_temp(dev->input[i].zone, &dev->input[i].temp);
		if (ret)
			dev_err_ratelimited(dev->dev, "Failed to get temp for sensor %d: %d\n", i, ret);
		/* As in stock, errors do not inject a replacement temperature.
		 * A provider leaving *temp unchanged preserves the cached value;
		 * a provider writing it before failing keeps that written value.
		 */
		input[i] = dev->input[i].temp;
	}
	dev->model->evaluate(input, output);
	mutex_lock(&dev->lock);
	for (i = 0; i < OUTPUT_COUNT; i++)
		dev->output[i].temp = output[i];
	mutex_unlock(&dev->lock);
	dev_info(dev->dev, "V=2 prod=1 front_temp=%d back_temp=%d user_temp=%d user_front_temp=%d user_back_temp=%d\n",
		 output[0], output[1], output[2], output[3], output[4]);
reschedule:
	if (!READ_ONCE(dev->stopping))
		schedule_delayed_work(&dev->work, msecs_to_jiffies(UPDATE_MS));
}

static int validate_names(struct device_node *node, const char *property,
			  const char * const *expected, unsigned int count)
{
	const char *name;
	unsigned int i;
	int ret;

	if (of_property_count_strings(node, property) != count)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		ret = of_property_read_string_index(node, property, i, &name);
		if (ret)
			return ret;
		if (strcmp(name, expected[i]))
			return -EINVAL;
	}
	return 0;
}

static int lenovo_sys_temp_probe(struct platform_device *pdev)
{
	struct device *parent = &pdev->dev;
	struct sys_temp *dev;
	const struct sys_temp_model *model;
	int i, ret;

	if (!parent->of_node)
		return -ENODEV;
	model = device_get_match_data(parent);
	if (!model)
		return -ENODEV;
	/* Coefficients are positional. Reject a different DT instead of silently
	 * applying baldur's model to a different sensor ordering or product.
	 */
	ret = validate_names(parent->of_node, "lenovo,sensor-listener-names",
			     model->input_names, INPUT_COUNT);
	if (ret)
		return ret;
	ret = validate_names(parent->of_node, "lenovo,temperature-names",
			     model->output_names, OUTPUT_COUNT);
	if (ret)
		return ret;
	dev = devm_kzalloc(parent, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->dev = parent;
	dev->model = model;
	mutex_init(&dev->lock);
	INIT_DELAYED_WORK(&dev->work, temp_changed_work_func);
	for (i = 0; i < OUTPUT_COUNT; i++) {
		dev->output[i].owner = dev;
		dev->output[i].zone = thermal_zone_device_register_with_trips(
			model->output_names[i], NULL, 0, &dev->output[i],
			&lenovo_sys_temp_ops, NULL, 0, 0);
		if (IS_ERR(dev->output[i].zone)) {
			ret = PTR_ERR(dev->output[i].zone);
			goto unregister;
		}
	}
	platform_set_drvdata(pdev, dev);
	schedule_delayed_work(&dev->work, msecs_to_jiffies(UPDATE_MS));
	return 0;
unregister:
	while (i--)
		thermal_zone_device_unregister(dev->output[i].zone);
	return ret;
}

static void lenovo_sys_temp_remove(struct platform_device *pdev)
{
	struct sys_temp *dev = platform_get_drvdata(pdev);
	int i;

	WRITE_ONCE(dev->stopping, true);
	cancel_delayed_work_sync(&dev->work);
	for (i = 0; i < OUTPUT_COUNT; i++)
		thermal_zone_device_unregister(dev->output[i].zone);
	platform_set_drvdata(pdev, NULL);
}

static const struct of_device_id lenovo_sys_temp_match_table[] = {
	{ .compatible = "lenovo,baldur-sys-temp", .data = &baldur_sys_temp_model },
	{ .compatible = "lenovo,sys-temp" },
	{},
};
MODULE_DEVICE_TABLE(of, lenovo_sys_temp_match_table);

static struct platform_driver lenovo_sys_temp_driver = {
	.probe = lenovo_sys_temp_probe,
	.remove = lenovo_sys_temp_remove,
	.driver = {
		.name = "lenovo_sys_temp",
		.of_match_table = lenovo_sys_temp_match_table,
	},
};
module_platform_driver(lenovo_sys_temp_driver);
MODULE_SOFTDEP("pre: qcom_spmi_adc5_gen3");
MODULE_ALIAS("platform:lenovo_sys_temp");
MODULE_AUTHOR("Lenovo");
MODULE_DESCRIPTION("Lenovo Thermal System Driver");
MODULE_LICENSE("GPL");