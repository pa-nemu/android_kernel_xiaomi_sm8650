#include "fp_driver.h"
/* -------------------------------------------------------------------- */
/* fingerprint chip hardware configuration				*/
/* -------------------------------------------------------------------- */
#define DTS_NETLINK_NUM				"netlink-event"
#define DTS_IRQ_GPIO				"xiaomi,gpio_irq"
#define DTS_PINCTL_RESET_HIGH			"reset_high"
#define DTS_PINCTL_RESET_LOW			"reset_low"

int fp_parse_dts(struct fp_device *fp_dev)
{
	int ret;
	struct device_node *node = fp_dev->driver_device->dev.of_node;
	if (node) {
	/*get irq resourece */
		fp_dev->irq_gpio =of_get_named_gpio(node, DTS_IRQ_GPIO, 0);
		if (!gpio_is_valid(fp_dev->irq_gpio)) {
			return -EPERM;
		}
		fp_dev->irq_num = gpio_to_irq(fp_dev->irq_gpio);
		of_property_read_u32(node, DTS_NETLINK_NUM, &fp_dev->fp_netlink_num);
	} else {
			return -EPERM;
	}
	if (fp_dev->driver_device) {
		fp_dev->pinctrl = pinctrl_get(&fp_dev->driver_device->dev);
		if (IS_ERR(fp_dev->pinctrl)) {
			ret = PTR_ERR(fp_dev->pinctrl);
			return ret;
		}
		fp_dev->pins_reset_high =
			pinctrl_lookup_state(fp_dev->pinctrl, DTS_PINCTL_RESET_HIGH);
		if (IS_ERR(fp_dev->pins_reset_high)) {
			ret = PTR_ERR(fp_dev->pins_reset_high);
			return ret;
		}
		fp_dev->pins_reset_low =
			pinctrl_lookup_state(fp_dev->pinctrl, DTS_PINCTL_RESET_LOW);
		if (IS_ERR(fp_dev->pins_reset_low)) {
			ret = PTR_ERR(fp_dev->pins_reset_low);
			return ret;
		} else {
			pinctrl_select_state(fp_dev->pinctrl, fp_dev->pins_reset_low);
		}
		/*
		fp_dev->pins_spiio_spi_mode =
	    	pinctrl_lookup_state(fp_dev->pinctrl, "spiio_spi_mode");
		if (IS_ERR(fp_dev->pins_spiio_spi_mode)) {
			ret = PTR_ERR(fp_dev->pins_spiio_spi_mode);
			pr_debug("%s can't find fingerprint pinctrl spiio_spi_mode\n",
				__func__);
		} else {
			pinctrl_select_state(fp_dev->pinctrl, fp_dev->pins_spiio_spi_mode);
		}

		fp_dev->pins_spiio_gpio_mode =
	    	pinctrl_lookup_state(fp_dev->pinctrl, "spiio_gpio_mode");
		if (IS_ERR(fp_dev->pins_spiio_gpio_mode)) {
			ret = PTR_ERR(fp_dev->pins_spiio_gpio_mode);
			pr_debug("%s can't find fingerprint spiio_gpio_mode\n",
				__func__);
		}

		fp_dev->pins_eint_default =
			pinctrl_lookup_state(fp_dev->pinctrl, "eint_default");
		if (IS_ERR(fp_dev->pins_eint_default)) {
			ret = PTR_ERR(fp_dev->pins_eint_default);
			pr_debug("%s can't find fingerprint pinctrl pins_eint_default\n",
				__func__);
			return ret;
		}

		fp_dev->pins_eint_pulldown =
	    	pinctrl_lookup_state(fp_dev->pinctrl, "eint_pulldown");
		if (IS_ERR(fp_dev->pins_eint_pulldown)) {
			ret = PTR_ERR(fp_dev->pins_eint_pulldown);
			pr_debug("%s can't find fingerprint pinctrl pins_eint_pulldown\n",
				__func__);
			return ret;

		} else {
			pinctrl_select_state(fp_dev->pinctrl, fp_dev->pins_eint_pulldown);
		}
		*/
	} else {
		return -EPERM;
	}
	return 0;
}

void fp_power_on(struct fp_device *fp_dev)
{
	int status=0;
	if(fp_dev->device_available == 1){
	}else{
#ifdef CONFIG_FP_MTK_PLATFORM
		status = regulator_set_voltage(fp_dev->vreg, 3300000, 3300000);
#endif
		status = regulator_enable(fp_dev->vreg);
		status = regulator_get_voltage(fp_dev->vreg);
		fp_dev->device_available = 1;
	}
}

void fp_power_off(struct fp_device *fp_dev)
{
	int status=0;
	if(fp_dev->device_available == 0){
	}else{
		status = regulator_disable(fp_dev->vreg);
		status = regulator_get_voltage(fp_dev->vreg);
		fp_dev->device_available = 0;
	}
}

/* delay ms after reset */
void fp_hw_reset(struct fp_device *fp_dev, u8 delay_ms)
{
	pinctrl_select_state(fp_dev->pinctrl, fp_dev->pins_reset_low);
	mdelay(10);
	pinctrl_select_state(fp_dev->pinctrl, fp_dev->pins_reset_high);
	mdelay(delay_ms);
}

void fp_enable_irq(struct fp_device *fp_dev)
{
	if (1 == fp_dev->irq_enabled) {
	} else {
		enable_irq(fp_dev->irq_num);
		fp_dev->irq_enabled = 1;
	}
}

void fp_disable_irq(struct fp_device *fp_dev)
{
	if (0 == fp_dev->irq_enabled) {
	} else {
		disable_irq(fp_dev->irq_num);
		fp_dev->irq_enabled = 0;
	}
}

void fp_kernel_key_input(struct fp_device *fp_dev, struct fp_key *fp_key)
{
	uint32_t key_input = 0;

	if (FP_KEY_HOME == fp_key->key) {
		key_input = FP_KEY_INPUT_HOME;
	} else if (FP_KEY_HOME_DOUBLE_CLICK == fp_key->key) {
                key_input = FP_KEY_DOUBLE_CLICK;
        } else if (FP_KEY_POWER == fp_key->key) {
		key_input = FP_KEY_INPUT_POWER;
	} else if (FP_KEY_CAMERA == fp_key->key) {
		key_input = FP_KEY_INPUT_CAMERA;
	} else {
		/* add special key define */
		key_input = fp_key->key;
	}


	if ((FP_KEY_POWER == fp_key->key || FP_KEY_CAMERA == fp_key->key)
		&& (fp_key->value == 1)) {
		input_report_key(fp_dev->input, key_input, 1);
		input_sync(fp_dev->input);
		input_report_key(fp_dev->input, key_input, 0);
		input_sync(fp_dev->input);
	}

	if (FP_KEY_HOME_DOUBLE_CLICK == fp_key->key) {
		input_report_key(fp_dev->input, key_input, fp_key->value);
		input_sync(fp_dev->input);
	}
}
