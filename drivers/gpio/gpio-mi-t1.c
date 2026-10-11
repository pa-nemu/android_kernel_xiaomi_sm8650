#include <linux/device.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/pm_wakeirq.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/regulator/consumer.h>
#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define MI_T1_GPIO_COUN_MAX 20
static int MI_T1_GPIO_COUNT = 4;//default for N1
static int FIRST_GPIO_ID = 0;
static bool b_legacy_drv = true;
#define MI_T1_MODE_MSK (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH)	/*664*/
#define MI_T1_GPIO_PROC_NAME "modem_t1_gpio_ctrl"

enum {
	MI_T1_GPIO_SATELLITE_CTL = 0,
	MI_T1_GPIO_WWAN_WLAN_1,
	MI_T1_GPIO_WWAN_WLAN_2,
	MI_ANT_GPIO_QM13111,
};

static const char * const mi_t1_gpio_names_legacy[] = {
	"gpio-satellite-ctl",
	"gpio-wwan-wlan-1",
	"gpio-wwan-wlan-2",
	"gpio-qm13111",
};

static const char * const mi_t1_gpio_names[] = {
	"mi-t1-gpio-1",
	"mi-t1-gpio-2",
	"mi-t1-gpio-3",
	"mi-t1-gpio-4",
	"mi-t1-gpio-5",
	"mi-t1-gpio-6",
	"mi-t1-gpio-7",
	"mi-t1-gpio-8",
	"mi-t1-gpio-9",
	"mi-t1-gpio-10",
};

struct gpio_data {
	int irq;
	int gpio_num;
	int gpio_status;
	int gpio_offset_num;
};

struct mi_t1_gpio_data {
	struct device *dev;

	struct mutex lock;	/* To set/get exported values in sysfs */
	struct gpio_data *data;

	/*char dev related data */
	char *driver_name;
	dev_t mi_t1_major;
	struct class *mi_t1_class;
	struct device *chardev;
	struct cdev cdev;
};


#define MINOR_NUMBER_COUNT 1

#define MI_T1_IOCTL_MAGIC 'y'
#define MI_T1_IOCTL_GPIO_SATELLITE_CTL_SET		_IOWR(MI_T1_IOCTL_MAGIC, 0, int)
#define MI_T1_IOCTL_GPIO_WWAN_WLAN_1_SET		_IOWR(MI_T1_IOCTL_MAGIC, 1, int)
#define MI_T1_IOCTL_GPIO_WWAN_WLAN_2_SET		_IOWR(MI_T1_IOCTL_MAGIC, 2, int)

#define MI_T1_IOCTL_GPIO_SATELLITE_CTL_GET		_IOWR(MI_T1_IOCTL_MAGIC, 3, int)
#define MI_T1_IOCTL_GPIO_WWAN_WLAN_1_GET		_IOWR(MI_T1_IOCTL_MAGIC, 4, int)
#define MI_T1_IOCTL_GPIO_WWAN_WLAN_2_GET		_IOWR(MI_T1_IOCTL_MAGIC, 5, int)

#define MI_T1_IOCTL_GPIO_QM13111_SET		    _IOWR(MI_T1_IOCTL_MAGIC, 6, int)
#define MI_T1_IOCTL_GPIO_QM13111_GET		    _IOWR(MI_T1_IOCTL_MAGIC, 7, int)


static ssize_t gpio_mi_t1_set_general(struct device *device, int gpio_type, const char *buf, size_t count)
{
	int rc = -1;
	int gpio = 0;
	struct mi_t1_gpio_data *mi_t1_data;

	mi_t1_data = dev_get_drvdata(device);
	gpio = mi_t1_data->data[gpio_type].gpio_num;

	mutex_lock(&mi_t1_data->lock);
	if (!strncmp(buf, "1", strlen("1"))) {
		if (gpio_is_valid(gpio)) {
			rc = gpio_direction_output(gpio, 1);
			if (rc) {
				goto unlock_ret;
			}
		} else {
		}
	} 
	else if (!strncmp(buf, "0", strlen("0"))) {
		if (gpio_is_valid(gpio)) {
			rc = gpio_direction_output(gpio, 0);
			if (rc) {
				goto unlock_ret;
			}
		} else {
		}
	} 
	else 
	{
		rc = -EINVAL;
	}

unlock_ret:
	mutex_unlock(&mi_t1_data->lock);
	return count;
}



static ssize_t gpio_mi_t1_get_general(struct device *device, int gpio_type, char *buf)
{
	int value = -1;
	int gpio = 0;
	struct mi_t1_gpio_data *mi_t1_data;
	mi_t1_data = dev_get_drvdata(device);

	mutex_lock(&mi_t1_data->lock);
	gpio = mi_t1_data->data[gpio_type].gpio_num;

	if (gpio_is_valid(gpio)) {
		value = gpio_get_value(gpio);
	} else {
	}
	mutex_unlock(&mi_t1_data->lock);

	return sysfs_emit(buf, "%d\n", value);
}


static int gpio_mi_t1_reuqest_gpio(struct platform_device *pdev, 
	struct mi_t1_gpio_data * mi_t1_data, 
	const char* gpio_name, int idx)
{
	int ret = 0;
	int gpio_num = -1;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
       static int gpio_offset = 0;

        if(idx == -1) {
		return -1;
	}
	gpio_num = of_get_named_gpio(np, gpio_name, 0);
       if(idx == 0) {
	   	gpio_offset = gpio_num - FIRST_GPIO_ID;
       }
       mi_t1_data->data[idx].gpio_offset_num = gpio_num-gpio_offset;

	ret = devm_gpio_request(dev, gpio_num, gpio_name);
	if (ret) {
		return ret;
	}
	mi_t1_data->data[idx].gpio_num = gpio_num;

	return ret;
}

static int mi_t1_gpio_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	struct mi_t1_gpio_data *mi_t1_data = container_of(inode->i_cdev,
						struct mi_t1_gpio_data,
						cdev);
	struct device *dev = mi_t1_data->chardev;

	get_device(dev);
	return ret;
}

static int mi_t1_gpio_release(struct inode *inode, struct file *file)
{
	struct mi_t1_gpio_data *mi_t1_data = container_of(inode->i_cdev,
						struct mi_t1_gpio_data,
						cdev);
	struct device *dev = mi_t1_data->chardev;

	put_device(dev);
	return 0;
}


static ssize_t gpio_mi_t1_get_satellite_ctl(struct device *device,
		struct device_attribute *attr, char *buf)
{
	return gpio_mi_t1_get_general(device, MI_T1_GPIO_SATELLITE_CTL, buf);
}

static ssize_t gpio_mi_t1_set_satellite_ctl(struct device *device,
                              struct device_attribute *attr,
                              const char *buf, size_t count)
{
	return gpio_mi_t1_set_general(device, MI_T1_GPIO_SATELLITE_CTL, buf, count);
}

static DEVICE_ATTR(satellite_ctl, MI_T1_MODE_MSK, gpio_mi_t1_get_satellite_ctl, gpio_mi_t1_set_satellite_ctl);


static ssize_t gpio_mi_t1_get_wwan_wlan_1(struct device *device,
		struct device_attribute *attr, char *buf)
{
	return gpio_mi_t1_get_general(device, MI_T1_GPIO_WWAN_WLAN_1, buf);
}

static ssize_t gpio_mi_t1_set_wwan_wlan_1(struct device *device,
                              struct device_attribute *attr,
                              const char *buf, size_t count)
{
	return gpio_mi_t1_set_general(device, MI_T1_GPIO_WWAN_WLAN_1, buf, count);
}

static DEVICE_ATTR(wwan_wlan_1, MI_T1_MODE_MSK, gpio_mi_t1_get_wwan_wlan_1, gpio_mi_t1_set_wwan_wlan_1);


static ssize_t gpio_mi_t1_get_wwan_wlan_2(struct device *device,
		struct device_attribute *attr, char *buf)
{
	return gpio_mi_t1_get_general(device, MI_T1_GPIO_WWAN_WLAN_2, buf);
}

static ssize_t gpio_mi_t1_set_wwan_wlan_2(struct device *device,
                              struct device_attribute *attr,
                              const char *buf, size_t count)
{
	return gpio_mi_t1_set_general(device, MI_T1_GPIO_WWAN_WLAN_2, buf, count);
}

static DEVICE_ATTR(wwan_wlan_2, MI_T1_MODE_MSK, gpio_mi_t1_get_wwan_wlan_2, gpio_mi_t1_set_wwan_wlan_2);

static ssize_t gpio_mi_t1_get_qm13111(struct device *device,
		struct device_attribute *attr, char *buf)
{
	return gpio_mi_t1_get_general(device, MI_ANT_GPIO_QM13111, buf);
}

static ssize_t gpio_mi_t1_set_qm13111(struct device *device,
                              struct device_attribute *attr,
                              const char *buf, size_t count)
{
	return gpio_mi_t1_set_general(device, MI_ANT_GPIO_QM13111, buf, count);
}

static DEVICE_ATTR(gpio174, MI_T1_MODE_MSK, gpio_mi_t1_get_qm13111, gpio_mi_t1_set_qm13111);


static struct attribute *attributes[] = {
	&dev_attr_satellite_ctl.attr,
	&dev_attr_wwan_wlan_1.attr,
	&dev_attr_wwan_wlan_2.attr,
	&dev_attr_gpio174.attr,
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};



static long mi_t1_gpio_ioctl(struct file *file, unsigned int ioctl_num,
				unsigned long __user ioctl_param)
{
	int ret = 0;

	void __user *argp = (void __user *)ioctl_param;
	int __user *p = argp;
	int gpio_num;
	int gpio_value = 0;

	struct mi_t1_gpio_data *mi_t1_data =
			container_of(file->f_inode->i_cdev, struct mi_t1_gpio_data, cdev);

	switch (ioctl_num) {

	case MI_T1_IOCTL_GPIO_SATELLITE_CTL_SET:
		get_user(gpio_value, p);
		if(!(mi_t1_data->data))
			break;
		gpio_num = mi_t1_data->data[MI_T1_GPIO_SATELLITE_CTL].gpio_num;
		if (gpio_is_valid(gpio_num)) {
			ret = gpio_direction_output(gpio_num, gpio_value);
		} else {
		}
		break;

	case MI_T1_IOCTL_GPIO_WWAN_WLAN_1_SET:
		get_user(gpio_value, p);
		if(!(mi_t1_data->data))
			break;
		gpio_num = mi_t1_data->data[MI_T1_GPIO_WWAN_WLAN_1].gpio_num;
		if (gpio_is_valid(gpio_num)) {
			ret = gpio_direction_output(gpio_num, gpio_value);
		} else {
		}
		break;

	case MI_T1_IOCTL_GPIO_WWAN_WLAN_2_SET:
		get_user(gpio_value, p);
		if(!(mi_t1_data->data))
			break;
		gpio_num = mi_t1_data->data[MI_T1_GPIO_WWAN_WLAN_2].gpio_num;
		if (gpio_is_valid(gpio_num)) {
			ret = gpio_direction_output(gpio_num, gpio_value);
		} else {
		}
		break;

	case MI_T1_IOCTL_GPIO_SATELLITE_CTL_GET:
		if(!(mi_t1_data->data))
			break;
		gpio_num = mi_t1_data->data[MI_T1_GPIO_SATELLITE_CTL].gpio_num;
		if (gpio_is_valid(gpio_num)) {
			gpio_value = gpio_get_value(gpio_num);
			put_user(gpio_value, p);
		} else {
		}
		break;

	case MI_T1_IOCTL_GPIO_WWAN_WLAN_1_GET:
		if(!(mi_t1_data->data))
			break;
		gpio_num = mi_t1_data->data[MI_T1_GPIO_WWAN_WLAN_1].gpio_num;
		if (gpio_is_valid(gpio_num)) {
			gpio_value = gpio_get_value(gpio_num);
			put_user(gpio_value, p);
		} else {
		}
		break;

	case MI_T1_IOCTL_GPIO_WWAN_WLAN_2_GET:
		if(!(mi_t1_data->data))
			break;
		gpio_num = mi_t1_data->data[MI_T1_GPIO_WWAN_WLAN_2].gpio_num;
		if (gpio_is_valid(gpio_num)) {
			gpio_value = gpio_get_value(gpio_num);
			put_user(gpio_value, p);
		} else {
		}
		break;

    case MI_T1_IOCTL_GPIO_QM13111_SET:
		get_user(gpio_value, p);
		if(!(mi_t1_data->data))
			break;
		gpio_num = mi_t1_data->data[MI_ANT_GPIO_QM13111].gpio_num;
		if (gpio_is_valid(gpio_num)) {
			ret = gpio_direction_output(gpio_num, gpio_value);
		} else {
		}
		break;

    case MI_T1_IOCTL_GPIO_QM13111_GET:
		if(!(mi_t1_data->data))
			break;
		gpio_num = mi_t1_data->data[MI_ANT_GPIO_QM13111].gpio_num;
		if (gpio_is_valid(gpio_num)) {
			gpio_value = gpio_get_value(gpio_num);
			put_user(gpio_value, p);
		} else {
		}
		break;


	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}


static  struct file_operations mi_t1_gpio_fops = {
	.owner = THIS_MODULE,
	.open = mi_t1_gpio_open,
	.release = mi_t1_gpio_release,
};


ssize_t t1_gpio_write(struct file *file,const char __user *buffer,size_t count,loff_t *ops)
{
    char config[MI_T1_GPIO_COUN_MAX+1];
    int i = 0;
    struct mi_t1_gpio_data *mi_t1_data = NULL;
    if(count <= 0) {
        return -1;
    }
    memset(config,0xff,MI_T1_GPIO_COUNT);
    config[MI_T1_GPIO_COUNT] = 0;
    if(copy_from_user((void *)config,(const void __user *)buffer,count > MI_T1_GPIO_COUNT?MI_T1_GPIO_COUNT:count))
		return -EFAULT;
    mi_t1_data = (struct mi_t1_gpio_data *) file_inode(file)->i_private;
    if(mi_t1_data == NULL)
    {
        return 0;
    }
    for(i=0;i<MI_T1_GPIO_COUNT;i++)
    {
        if(config[i] != 0xff)
        {
            gpio_mi_t1_set_general(mi_t1_data->dev, i, config+i, 1);
        }
    }
    return count;
}

static int gpio_t1_get_state(struct device *device, int gpio_type, char *buf)
{
    int value = -1;
    int gpio = 0;
    struct mi_t1_gpio_data *mi_t1_data;
    mi_t1_data = dev_get_drvdata(device);
    mutex_lock(&mi_t1_data->lock);
    gpio = mi_t1_data->data[gpio_type].gpio_num;
    if (gpio_is_valid(gpio)) {
        value = gpio_get_value(gpio);
    } else {
    }
    mutex_unlock(&mi_t1_data->lock);
    *buf = (char)value;
    return mi_t1_data->data[gpio_type].gpio_offset_num;
}


static int t1_gpio_show(struct seq_file *m, void *v)
{
    int i = 0,gpio=0;
    char config[4];
    struct mi_t1_gpio_data *mi_t1_data =(struct mi_t1_gpio_data *) m->private;;
    seq_printf(m, "GPIO cout=%d : \n",MI_T1_GPIO_COUNT);
#if 1
    for(i=0;i<MI_T1_GPIO_COUNT;i++)
    {
        gpio=gpio_t1_get_state(mi_t1_data->dev,i,config);
        seq_printf(m, "GPIO-%d state=%d \n",gpio,config[0]);
    }
#endif
    return 0;
}
static int t1_gpio_open(struct inode *inode, struct file *file)
{
    return single_open(file, t1_gpio_show, file_inode(file)->i_private);
}

static struct proc_ops   t1_gpio_fops = {
    .proc_open	= t1_gpio_open,
    .proc_read	= seq_read,
    .proc_lseek	= seq_lseek,
    .proc_release	= single_release,
    .proc_write	= t1_gpio_write,
};


static int gpio_mi_t1_reg_chrdev(struct mi_t1_gpio_data * mi_t1_data)
{
	int ret = 0;

	mi_t1_data->driver_name = "mi_t1_chip";
	ret = alloc_chrdev_region(&mi_t1_data->mi_t1_major, 0,
				MINOR_NUMBER_COUNT, mi_t1_data->driver_name);
	if (ret < 0) {
		return ret;
	}

	mi_t1_data->mi_t1_class = class_create(THIS_MODULE,
					mi_t1_data->driver_name);
	if (IS_ERR(mi_t1_data->mi_t1_class)) {
		ret = PTR_ERR(mi_t1_data->mi_t1_class);
		goto err_class;
	}

	mi_t1_data->chardev = device_create(mi_t1_data->mi_t1_class, NULL,
				mi_t1_data->mi_t1_major, NULL,
				mi_t1_data->driver_name);
	if (IS_ERR(mi_t1_data->chardev)) {
		ret = PTR_ERR(mi_t1_data->chardev);
		goto err_device;
	}

	cdev_init(&mi_t1_data->cdev, &mi_t1_gpio_fops);
	ret = cdev_add(&mi_t1_data->cdev, mi_t1_data->mi_t1_major, 1);
	if (ret) {
		goto err_cdev;
	}
	return ret;

err_cdev:
	device_destroy(mi_t1_data->mi_t1_class, mi_t1_data->mi_t1_major);
err_device:
	class_destroy(mi_t1_data->mi_t1_class);
err_class:
	unregister_chrdev_region(0, MINOR_NUMBER_COUNT);
	return ret;
}




static int gpio_mi_t1_probe(struct platform_device *pdev)
{
	int i;
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct mi_t1_gpio_data *mi_t1_data;
       u32 config_gpio_count = 0;

	mi_t1_data = devm_kzalloc(dev, sizeof(struct mi_t1_gpio_data), GFP_KERNEL);
	if (!mi_t1_data) {
		return -ENOMEM;
	}
	ret = of_property_read_u32(dev->of_node, "gpio-count", &config_gpio_count);
       if (ret == 0) {
           if(MI_T1_GPIO_COUN_MAX <= config_gpio_count) {
               return -ENOMEM;
           }
           MI_T1_GPIO_COUNT = config_gpio_count;
           b_legacy_drv  = false;
       } else {
           ret = 0;
       }
       config_gpio_count = 0;
       ret = of_property_read_u32(dev->of_node, "first-gpio", &config_gpio_count);
       FIRST_GPIO_ID = config_gpio_count;
	mi_t1_data->data = devm_kcalloc(dev, MI_T1_GPIO_COUNT, sizeof(struct gpio_data), GFP_KERNEL);
	if (!mi_t1_data->data) {
		return -ENOMEM;
	}

	mutex_init(&mi_t1_data->lock);
	mi_t1_data->dev = dev;
	platform_set_drvdata(pdev, mi_t1_data);

	for(i=0; i<MI_T1_GPIO_COUNT; i++) {
           gpio_mi_t1_reuqest_gpio(pdev, mi_t1_data, b_legacy_drv?mi_t1_gpio_names_legacy[i]:mi_t1_gpio_names[i],i);
	}

       if(b_legacy_drv) {
	    ret = sysfs_create_group(&dev->kobj, &attribute_group);
	    if (ret < 0) {
		    return -EINVAL;
	    }
           mi_t1_gpio_fops.unlocked_ioctl = mi_t1_gpio_ioctl;
       }

	device_init_wakeup(dev, true);
	ret = gpio_mi_t1_reg_chrdev(mi_t1_data);
	if (ret) {
		return ret;
	}
       proc_create_data(MI_T1_GPIO_PROC_NAME, 0777, NULL, &t1_gpio_fops,(void*)mi_t1_data);
	return ret;
}

static int gpio_mi_t1_remove(struct platform_device *pdev)
{
	struct mi_t1_gpio_data *mi_t1_data = platform_get_drvdata(pdev);
       remove_proc_entry(MI_T1_GPIO_PROC_NAME,NULL);
	sysfs_remove_group(&pdev->dev.kobj, &attribute_group);
	dev_pm_clear_wake_irq(mi_t1_data->dev);

	mutex_destroy(&mi_t1_data->lock);

	return 0;
}


#if defined(CONFIG_OF)
static const struct of_device_id gpio_mi_t1_of_match[] = {
	{ .compatible = "mdm,mi_t1_chip", },
	{},
};
MODULE_DEVICE_TABLE(of, gpio_mi_t1_of_match);
#endif


#ifdef CONFIG_PM
static int gpio_mi_t1_suspend(struct device *dev)
{
	struct mi_t1_gpio_data *mi_t1_data;

	mi_t1_data = dev_get_drvdata(dev);

	return 0;
}

static int gpio_mi_t1_resume(struct device *dev)
{
	struct mi_t1_gpio_data *mi_t1_data;

	mi_t1_data = dev_get_drvdata(dev);

	return 0;
}

static const struct dev_pm_ops gpio_mi_t1_pm_ops = {
	.suspend = gpio_mi_t1_suspend,
	.resume = gpio_mi_t1_resume,
	.freeze = gpio_mi_t1_suspend,
	.restore = gpio_mi_t1_resume,
};
#endif

static struct platform_driver gpio_mi_t1_driver = {
	.driver = {
		.name = "gpio-mi-t1",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(gpio_mi_t1_of_match),
#ifdef CONFIG_PM
		.pm = &gpio_mi_t1_pm_ops,
#endif
	},
	.probe = gpio_mi_t1_probe,
	.remove = gpio_mi_t1_remove,
};



static int __init gpio_mi_t1_driver_init(void)
{
	int err;

	err = platform_driver_register(&gpio_mi_t1_driver);

	return err;
}


static void __exit gpio_mi_t1_driver_exit(void)
{
	platform_driver_unregister(&gpio_mi_t1_driver);
}


module_init(gpio_mi_t1_driver_init);
module_exit(gpio_mi_t1_driver_exit);


//module_platform_driver(gpio_mi_t1_driver);
MODULE_AUTHOR("huangqianhong@xiaomi.com");
MODULE_DESCRIPTION("Driver for mi_t1 GPIO control");
MODULE_LICENSE("GPL v2");


