/*
 * WL2868C ON Semiconductor LDO PMIC Driver.
 *
 * Copyright (c) 2020 On XiaoMi.
 * liuqinhong@xiaomi.com
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/cdev.h>
#include <linux/fs.h>

#include <linux/module.h>
#include <linux/param.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/of_device.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/pinctrl/consumer.h>


#define wl2868c_err(message, ...) \
((void)0)
#define wl2868c_debug(message, ...) \
((void)0)

#define WL2868C_SLAVE_ADDR      0x5E

#define WL2868C_REG_ENABLE      0x0E
#define WL2868C_REG_DISCHARGE   0x02
#define WL2868C_REG_LDO0        0x03

#define LDO_VSET_REG(offset) ((offset) + WL2868C_REG_LDO0)

#define VSET_DVDD_BASE_UV   496000
#define VSET_DVDD_STEP_UV   8000

#define VSET_AVDD_BASE_UV   1504000
#define VSET_AVDD_STEP_UV   8000

#define MAX_REG_NAME     20
#define WL2868C_MAX_LDO  7

#define WL2868C_CLASS_NAME       "camera_ldo"
#define WL2868C_NAME_FMT         "wl2868c%u"
#define WL2868C_NAME_STR_LEN_MAX 50
#define WL2868C_MAX_NUMBER       5

#define WL2868C_PINCTRL_ENABLE       "wl2868c_enable"
#define WL2868C_PINCTRL_DISABLE      "wl2868c_disable"

struct wl2868c_char_dev {
	dev_t dev_no;
	struct cdev *pcdev;
	struct device *pdevice;
};

static struct class *pwl2868c_class = NULL;
static struct wl2868c_char_dev wl2868c_dev_list[WL2868C_MAX_NUMBER];

struct wl2868c_regulator{
	struct device    *dev;
	struct regmap    *regmap;
	struct regulator_desc rdesc;
	struct regulator_dev  *rdev;
	struct regulator      *parent_supply;
	struct regulator      *en_supply;
	struct device_node    *of_node;
	u16         offset;
	int         min_dropout_uv;
	int         iout_ua;
	int         index;
};

struct regulator_data {
	char *name;
	char *supply_name;
	int   default_uv;
	int   min_dropout_uv;
	int   iout_ua;
};

static struct regulator_data reg_data[] = {
	{ "fan53870-l1", "none", 1200000, 80000, 800000},
	{ "fan53870-l2", "none", 1200000, 80000, 800000},
	{ "fan53870-l3", "none", 2800000, 80000, 800000},
	{ "fan53870-l4", "none", 2800000, 80000, 800000},
	{ "fan53870-l5", "none", 2800000, 80000, 800000},
	{ "fan53870-l6", "none", 2800000, 80000, 800000},
	{ "fan53870-l7", "none", 2800000, 80000, 800000},
};

static const struct regmap_config wl2868c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

/*common functions*/
static int wl2868c_read(struct regmap *regmap, u16 reg, u8 *val, int count)
{
	int rc;

	rc = regmap_bulk_read(regmap, reg, val, count);
	return rc;
}

static int wl2868c_write(struct regmap *regmap, u16 reg, u8*val, int count)
{
	int rc;

	rc = regmap_bulk_write(regmap, reg, val, count);

	return rc;
}

static int wl2868c_masked_write(struct regmap *regmap, u16 reg, u8 mask, u8 val)
{
	int rc;
	rc = regmap_update_bits(regmap, reg, mask, val);
	return rc;
}

static int wl2868c_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct wl2868c_regulator *fan_reg = rdev_get_drvdata(rdev);
	int rc;
	u8 reg;

	rc = wl2868c_read(fan_reg->regmap,
		WL2868C_REG_ENABLE, &reg, 1);
	if (rc < 0) {
		return rc;
	}
	return !!(reg & (1u << fan_reg->offset));
}

static int wl2868c_regulator_enable(struct regulator_dev *rdev)
{
	struct wl2868c_regulator *fan_reg = rdev_get_drvdata(rdev);
	int rc;

	if (fan_reg->parent_supply) {
		rc = regulator_enable(fan_reg->parent_supply);
		if (rc < 0) {
			return rc;
		}
	}

	rc = wl2868c_masked_write(fan_reg->regmap,
		WL2868C_REG_ENABLE,
		1u << fan_reg->offset, 1u << fan_reg->offset);
	if (rc < 0) {
		goto remove_vote;
	}
	return 0;

remove_vote:
	if (fan_reg->parent_supply)
		rc = regulator_disable(fan_reg->parent_supply);
	return -ETIME;
}

static int wl2868c_regulator_disable(struct regulator_dev *rdev)
{
	struct wl2868c_regulator *fan_reg = rdev_get_drvdata(rdev);
	int rc;

	rc = wl2868c_masked_write(fan_reg->regmap,
		WL2868C_REG_ENABLE,
		1u << fan_reg->offset, 0);

	if (rc < 0) {
		return rc;
	}

	/*remove voltage vot from parent regulator */
	if (fan_reg->parent_supply) {
		rc = regulator_set_voltage(fan_reg->parent_supply, 0, INT_MAX);
		if (rc < 0) {
			return rc;
		}
		rc = regulator_disable(fan_reg->parent_supply);
		if (rc < 0) {
			return rc;
		}
	}

	return 0;
}

static int wl2868c_regulator_get_voltage(struct regulator_dev *rdev)
{
	struct wl2868c_regulator *fan_reg = rdev_get_drvdata(rdev);
	u8  vset = 0;
	int rc   = 0;
	int uv   = 0;

	rc = wl2868c_read(fan_reg->regmap, LDO_VSET_REG(fan_reg->offset),
	&vset, 1);
	if (rc < 0) {
		return rc;
	}

	if (vset == 0) {
		uv = reg_data[fan_reg->offset].default_uv;
	} else {
		if (fan_reg->offset == 0 || fan_reg->offset == 1)
			uv = VSET_DVDD_BASE_UV + vset * VSET_DVDD_STEP_UV; //DVDD
		else
			uv = VSET_AVDD_BASE_UV + vset * VSET_AVDD_STEP_UV; //AVDD
	}
	return uv;
}

static int wl2868c_write_voltage(struct wl2868c_regulator* fan_reg, int min_uv,
	int max_uv)
{
	int rc   = 0;
	u8  vset = 0;

	if (min_uv > max_uv) {
		return -EINVAL;
	}

	if (fan_reg->offset == 0 || fan_reg->offset == 1)
		vset = DIV_ROUND_UP(min_uv - VSET_DVDD_BASE_UV, VSET_DVDD_STEP_UV); //DVDD
	else
		vset = DIV_ROUND_UP(min_uv - VSET_AVDD_BASE_UV, VSET_AVDD_STEP_UV); //AVDD

	rc = wl2868c_write(fan_reg->regmap, LDO_VSET_REG(fan_reg->offset),
		&vset, 1);
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static int wl2868c_regulator_set_voltage(struct regulator_dev *rdev,
	int min_uv, int max_uv, unsigned int* selector)
{
	struct wl2868c_regulator *fan_reg = rdev_get_drvdata(rdev);
	int rc;

	if (fan_reg->parent_supply) {
		rc = regulator_set_voltage(fan_reg->parent_supply,
			fan_reg->min_dropout_uv + min_uv,
			INT_MAX);
		if (rc < 0) {
			return rc;
		}
	}

	rc = wl2868c_write_voltage(fan_reg, min_uv, max_uv);
	if (rc < 0) {
		/* remove parentn's voltage vote */
		if (fan_reg->parent_supply)
			regulator_set_voltage(fan_reg->parent_supply, 0, INT_MAX);
	}
	return rc;
}

static struct regulator_ops wl2868c_regulator_ops = {
	.enable      = wl2868c_regulator_enable,
	.disable     = wl2868c_regulator_disable,
	.is_enabled  = wl2868c_regulator_is_enabled,
	.set_voltage = wl2868c_regulator_set_voltage,
	.get_voltage = wl2868c_regulator_get_voltage,
};

static int wl2868c_register_ldo(struct wl2868c_regulator *wl2868c_reg,
	const char *name)
{
	struct regulator_config reg_config = {};
	struct regulator_init_data *init_data;

	struct device_node *reg_node = wl2868c_reg->of_node;
	struct device *dev           = wl2868c_reg->dev;
	int rc, i, init_voltage;
	char buff[MAX_REG_NAME];

	/* try to find ldo pre-defined in the regulator table */
	for (i = 0; i< WL2868C_MAX_LDO; i++) {
		if (!strcmp(reg_data[i].name, name))
			break;
	}

	if (i == WL2868C_MAX_LDO) {
		return -EINVAL;
	}

	rc = of_property_read_u16(reg_node, "offset", &wl2868c_reg->offset);
	if (rc < 0) {
		return rc;
	}

	//assign default value defined in code.
	wl2868c_reg->min_dropout_uv = reg_data[i].min_dropout_uv;
	of_property_read_u32(reg_node, "min-dropout-voltage",
		&wl2868c_reg->min_dropout_uv);

	wl2868c_reg->iout_ua = reg_data[i].iout_ua;
	of_property_read_u32(reg_node, "iout_ua",
		&wl2868c_reg->iout_ua);

	init_voltage = -EINVAL;
	of_property_read_u32(reg_node, "init-voltage", &init_voltage);

	scnprintf(buff, MAX_REG_NAME, "%s-supply", reg_data[i].supply_name);
	if (of_find_property(dev->of_node, buff, NULL)) {
		wl2868c_reg->parent_supply = devm_regulator_get(dev,
			reg_data[i].supply_name);
		if (IS_ERR(wl2868c_reg->parent_supply)) {
			rc = PTR_ERR(wl2868c_reg->parent_supply);
				return rc;
		}
	}

	init_data = of_get_regulator_init_data(dev, reg_node, &wl2868c_reg->rdesc);
	if (init_data == NULL) {
		return -ENODATA;
	}


	if (!init_data->constraints.name) {
		return -EINVAL;
	}

	/* configure the initial voltage for the regulator */
	if (init_voltage > 0) {
		rc = wl2868c_write_voltage(wl2868c_reg, init_voltage,
			init_data->constraints.max_uV);
	}

	init_data->constraints.input_uV = init_data->constraints.max_uV;
	init_data->constraints.valid_ops_mask |= REGULATOR_CHANGE_STATUS
		| REGULATOR_CHANGE_VOLTAGE;

	reg_config.dev         = dev;
	reg_config.init_data   = init_data;
	reg_config.driver_data = wl2868c_reg;
	reg_config.of_node     = reg_node;

	wl2868c_reg->rdesc.owner      = THIS_MODULE;
	wl2868c_reg->rdesc.type       = REGULATOR_VOLTAGE;
	wl2868c_reg->rdesc.ops        = &wl2868c_regulator_ops;
	wl2868c_reg->rdesc.name       = init_data->constraints.name;
	wl2868c_reg->rdesc.n_voltages = 1;

	wl2868c_reg->rdev = devm_regulator_register(dev, &wl2868c_reg->rdesc,
		&reg_config);
	if (IS_ERR(wl2868c_reg->rdev)) {
		rc = PTR_ERR(wl2868c_reg->rdev);
		return rc;
	}

	return 0;
}

static int wl2868c_parse_regulator(struct regmap *regmap, struct device *dev)
{
	int rc           = 0;
	int index        = 0;
	const char *name = NULL;
	struct device_node *child             = NULL;
	struct wl2868c_regulator *wl2868c_reg = NULL;

	of_property_read_u32(dev->of_node, "index", &index);

	/* parse each regulator */
	for_each_available_child_of_node(dev->of_node, child) {
		wl2868c_reg = devm_kzalloc(dev, sizeof(*wl2868c_reg), GFP_KERNEL);
		if (!wl2868c_reg)
			return -ENOMEM;

		wl2868c_reg->regmap  = regmap;
		wl2868c_reg->of_node = child;
		wl2868c_reg->dev     = dev;
		wl2868c_reg->index   = index;
		wl2868c_reg->parent_supply = NULL;

		rc = of_property_read_string(child, "regulator-name", &name);
		if (rc)
			continue;

		rc = wl2868c_register_ldo(wl2868c_reg, name);
		if (rc <0 ) {
			return rc;
		}
	}
	return 0;
}

static int wl2868c_open(struct inode *a_inode, struct file *a_file)
{
	return 0;
}

static int wl2868c_release(struct inode *a_inode, struct file *a_file)
{
	return 0;
}

static long wl2868c_ioctl(struct file *a_file, unsigned int a_cmd,
			 unsigned long a_param)
{
	return 0;
}

static const struct file_operations wl2868c_file_operations = {
	.owner          = THIS_MODULE,
	.open           = wl2868c_open,
	.release        = wl2868c_release,
	.unlocked_ioctl = wl2868c_ioctl,
};

static ssize_t wl2868c_show_status(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct regmap *regmap = (struct regmap *)dev->driver_data;
	int rc;
	unsigned int val      = 0xFF;
	unsigned int len      = 0;

	rc = regmap_read(regmap, 0x00, &val);
	if (rc < 0) {
		len = sprintf(buf, "fail\n");
	}
	else {
		len = sprintf(buf, "success\n");
	}

	return len;
}

static ssize_t wl2868c_show_info(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct regmap *regmap = (struct regmap *)dev->driver_data;
	int rc;
	unsigned int val      = 0xFF;
	unsigned int len      = 0;
	int i = 0;

	for (i = 0; i <= 0x24; i++) {
		rc = regmap_read(regmap, i, &val);
		if (rc < 0) {
			len += sprintf(buf+len, "read 0x%x ==> fail\n", i);
		}
		else {
			len += sprintf(buf+len, "read 0x%x ==> 0x%x\n",i, val);
		}
	}

	return len;
}


static ssize_t wl2868c_show_enable(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct regmap *regmap = (struct regmap *)dev->driver_data;
	int rc;
	unsigned int val      = 0xFF;
	unsigned int len      = 0;

	rc = regmap_read(regmap, WL2868C_REG_ENABLE, &val);
	if (rc < 0) {
		len = sprintf(buf, "read 0x0E ==> fail\n");
	}
	else {
		len = sprintf(buf, "read 0x0E ==> 0x%x\n", val);
	}

	return len;
}

static ssize_t wl2868c_set_enable(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	u8 val = 0;
	struct regmap *regmap = (struct regmap *)dev->driver_data;

	if (buf[0] == '0' && buf[1] == 'x') {
		val = (u8)simple_strtoul(buf, NULL, 16);
	} else {
		val = (u8)simple_strtoul(buf, NULL, 10);
	}
	wl2868c_write(regmap, WL2868C_REG_ENABLE, &val, 1);

	return len;
}


static DEVICE_ATTR(status, S_IWUSR|S_IRUSR, wl2868c_show_status, NULL);
static DEVICE_ATTR(info, S_IWUSR|S_IRUSR, wl2868c_show_info, NULL);
static DEVICE_ATTR(enable, S_IWUSR|S_IRUSR, wl2868c_show_enable, wl2868c_set_enable);




static int wl2868c_driver_register(int index, struct regmap *regmap)
{
	unsigned long ret;
	char device_drv_name[WL2868C_NAME_STR_LEN_MAX] = { 0 };
	struct wl2868c_char_dev wl2868c_dev = wl2868c_dev_list[index];

	snprintf(device_drv_name, WL2868C_NAME_STR_LEN_MAX - 1,
		WL2868C_NAME_FMT, index);

	/* Register char driver */
	if (alloc_chrdev_region(&(wl2868c_dev.dev_no), 0, 1,
			device_drv_name)) {
		return -EAGAIN;
	}

	/* Allocate driver */
	wl2868c_dev.pcdev = cdev_alloc();
	if (wl2868c_dev.pcdev == NULL) {
		unregister_chrdev_region(wl2868c_dev.dev_no, 1);
		return -ENOMEM;
	}

	/* Attatch file operation. */
	cdev_init(wl2868c_dev.pcdev, &wl2868c_file_operations);
	wl2868c_dev.pcdev->owner = THIS_MODULE;

	/* Add to system */
	if (cdev_add(wl2868c_dev.pcdev, wl2868c_dev.dev_no, 1)) {
		unregister_chrdev_region(wl2868c_dev.dev_no, 1);
		return -EAGAIN;
	}

	if (pwl2868c_class == NULL) {
		pwl2868c_class = class_create(THIS_MODULE, WL2868C_CLASS_NAME);
		if (IS_ERR(pwl2868c_class)) {
			int ret = PTR_ERR(pwl2868c_class);
			return ret;
		}
	}

	wl2868c_dev.pdevice = device_create(pwl2868c_class, NULL,
			wl2868c_dev.dev_no, NULL, device_drv_name);
	if (wl2868c_dev.pdevice == NULL) {
		return -ENOMEM;
	}

	wl2868c_dev.pdevice->driver_data = regmap;
	ret = sysfs_create_file(&(wl2868c_dev.pdevice->kobj), &dev_attr_status.attr);
	ret = sysfs_create_file(&(wl2868c_dev.pdevice->kobj), &dev_attr_info.attr);
	ret = sysfs_create_file(&(wl2868c_dev.pdevice->kobj), &dev_attr_enable.attr);

	return 0;
}
static int wl2868c_regulator_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int rc                = 0;
	unsigned int val      = 0xFF;
	struct regmap *regmap = NULL;
	int index = 0;
	int retryCount = 0;

	struct pinctrl *pinctrl = NULL;
	struct pinctrl_state *gpio_state_enable = NULL;
	struct pinctrl_state *gpio_state_disable = NULL;

	pinctrl = devm_pinctrl_get(&client->dev);
	if (!pinctrl) {
		return -ENODEV;
	}

	gpio_state_enable =  pinctrl_lookup_state(pinctrl, WL2868C_PINCTRL_ENABLE);
	if (IS_ERR_OR_NULL(gpio_state_enable)) {
		return -EINVAL;
	}
	gpio_state_disable = pinctrl_lookup_state(pinctrl, WL2868C_PINCTRL_DISABLE);
	if (IS_ERR_OR_NULL(gpio_state_disable)) {
		return -EINVAL;
	}

	rc = pinctrl_select_state(pinctrl, gpio_state_disable);

	msleep(25);

	rc = pinctrl_select_state(pinctrl, gpio_state_enable);

	msleep(25);

	client->addr =  (WL2868C_SLAVE_ADDR >> 1);

	rc = of_property_read_u32(client->dev.of_node, "index", &index);
	if (rc) {
		return rc;
	}

	regmap = devm_regmap_init_i2c(client, &wl2868c_regmap_config);
	if (IS_ERR(regmap)) {
		return PTR_ERR(regmap);
	}

	wl2868c_driver_register(index, regmap);

	rc = regmap_read(regmap, 0x00, &val);
	while (rc < 0) {
		if (retryCount == 3)
		{
			return 0;
		}
		retryCount++;
		rc = regmap_read(regmap, 0x00, &val);
		msleep(1);
	}



	rc = wl2868c_parse_regulator(regmap, &client->dev);
	if (rc < 0) {
		return 0;
	}

	return 0;
}

static const struct of_device_id wl2868c_dt_ids[] = {
	{
		.compatible = "onsemi,fan53870",
	},
	{
		.compatible = "willsemi,wl2868c",
	},
	{ }
};
MODULE_DEVICE_TABLE(of, wl2868c_dt_ids);

static const struct i2c_device_id wl2868c_id[] = {
	{
		.name = "wl2868c-regulator",
		.driver_data = 0,
	},
	{ },
};
MODULE_DEVICE_TABLE(i2c, wl2868c_id);

static struct i2c_driver wl2868c_regulator_driver = {
	.driver = {
		.name = "wl2868c-regulator",
		.of_match_table = of_match_ptr(wl2868c_dt_ids),
	},
	.probe = wl2868c_regulator_probe,
	.id_table = wl2868c_id,
};

module_i2c_driver(wl2868c_regulator_driver);
MODULE_LICENSE("GPL v2");

