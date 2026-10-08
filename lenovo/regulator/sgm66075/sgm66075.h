/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SGM66075_H
#define _SGM66075_H

#include <linux/types.h>

struct device;
struct i2c_client;
struct regmap;

struct sgm66075 {
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	u8 id;
	u8 status;
};

int sgm_boost_enable_set(struct sgm66075 *sgm, bool enable);
int sgm_boost_vout_set(struct sgm66075 *sgm, int uv);
int sgm_boost_ilimit_set(struct sgm66075 *sgm, int ua);
int sgm_boost_status_get(struct sgm66075 *sgm);

#endif
