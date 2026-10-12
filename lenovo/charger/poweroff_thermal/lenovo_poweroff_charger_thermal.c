// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/of.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

extern int qti_battery_charger_set_charge_current_ua(int current_ua);
static struct {
	int threshold_mc;
	int current_ua;
} offmode_limits[10];
static const char *sensor_name;
static u32 default_current_ua;
static u32 initial_ms;
static u32 interval_ms;
static int last_current_ua = -1;
static void offmode_thermal_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(offmode_work, offmode_thermal_work);

static void offmode_thermal_work(struct work_struct *work)
{
	struct thermal_zone_device *zone;
	int temperature, current_ua = default_current_ua;
	int i, ret;

	zone = thermal_zone_get_zone_by_name(sensor_name);
	if (IS_ERR(zone))
		goto reschedule;
	ret = thermal_zone_get_temp(zone, &temperature);
	if (ret || temperature < 0)
		goto reschedule;
	for (i = 0; i < ARRAY_SIZE(offmode_limits); i++) {
		if (temperature >= offmode_limits[i].threshold_mc) {
			current_ua = offmode_limits[i].current_ua;
			break;
		}
	}
	if (current_ua != last_current_ua) {
		ret = qti_battery_charger_set_charge_current_ua(current_ua);
		if (!ret)
			last_current_ua = current_ua;
		else
			pr_err_ratelimited("Wuji off-mode FCC update failed: %d\n", ret);
	}
reschedule:
	schedule_delayed_work(&offmode_work, msecs_to_jiffies(interval_ms));
}
static int __init offmode_thermal_init(void)
{
	struct device_node *charger;
	struct device_node *chosen;
	const char *args;
	bool offmode = true;
	u32 limits[ARRAY_SIZE(offmode_limits) * 2];
	int i, rc;

	charger = of_find_compatible_node(NULL, NULL, "lenovo,offmode-charger-v1");
	if (!charger)
		return 0;
	rc = of_property_read_string(charger, "lenovo,thermal-sensor", &sensor_name);
	if (!rc)
		rc = of_property_read_u32_array(charger, "lenovo,current-limits",
					limits, ARRAY_SIZE(limits));
	if (!rc)
		rc = of_property_read_u32(charger, "lenovo,default-current-ua", &default_current_ua);
	if (!rc)
		rc = of_property_read_u32(charger, "lenovo,initial-delay-ms", &initial_ms);
	if (!rc)
		rc = of_property_read_u32(charger, "lenovo,poll-interval-ms", &interval_ms);
	of_node_put(charger);
	if (rc || !interval_ms)
		return rc ?: -EINVAL;
	for (i = 0; i < ARRAY_SIZE(offmode_limits); i++) {
		offmode_limits[i].threshold_mc = limits[i * 2];
		offmode_limits[i].current_ua = limits[i * 2 + 1];
	}
	chosen = of_find_node_by_path("/chosen");
	/* Match the default when chosen/bootargs cannot be read. */
	if (chosen && !of_property_read_string(chosen, "bootargs", &args))
		offmode = strstr(args, "offmode.charge=1") ||
			  strstr(args, "offmode.charge = 1");
	of_node_put(chosen);
	if (offmode)
		schedule_delayed_work(&offmode_work, msecs_to_jiffies(initial_ms));
	return 0;
}
static void __exit offmode_thermal_exit(void)
{
	cancel_delayed_work_sync(&offmode_work);
}
module_init(offmode_thermal_init);
module_exit(offmode_thermal_exit);
MODULE_DESCRIPTION("Lenovo Wuji off-mode charging thermal current limit");
MODULE_LICENSE("GPL");
