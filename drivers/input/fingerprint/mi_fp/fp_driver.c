#include "fp_driver.h"
#define WAKELOCK_HOLD_TIME 2000	/* in ms */
#define FP_UNLOCK_REJECTION_TIMEOUT (WAKELOCK_HOLD_TIME - 500) /*ms*/
/* #define XIAOMI_DRM_INTERFACE_WA */
/*device name after register in charater*/
#define FP_DEV_NAME "xiaomi-fp"
#define FP_CLASS_NAME "xiaomi_fp"
#define FP_INPUT_NAME "uinput-xiaomi"

#ifdef CONFIG_FP_MTK_PLATFORM
#include "teei_fp.h"
#include "tee_client_api.h"

extern void mt_spi_enable_master_clk(struct spi_device *spidev);
extern void mt_spi_disable_master_clk(struct spi_device *spidev);

static atomic_t clk_ref = ATOMIC_INIT(0);
static void fp_spi_clk_enable(struct spi_device *spi)
{
        if (atomic_read(&clk_ref) == 0) {
                mt_spi_enable_master_clk(spi);
                atomic_inc(&clk_ref);
        }
}
static void fp_spi_clk_disable(struct spi_device *spi)
{
        if (atomic_read(&clk_ref) == 1) {
                atomic_dec(&clk_ref);
                mt_spi_disable_master_clk(spi);
        }
}
#endif

/*************************************************************/
static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);
static struct wakeup_source *fp_wakesrc = NULL;
struct work_struct fp_display_work;
static struct fp_device fp;

static struct fp_key_map maps[] = {
        {EV_KEY, FP_KEY_INPUT_HOME},
        {EV_KEY, FP_KEY_INPUT_MENU},
        {EV_KEY, FP_KEY_INPUT_BACK},
        {EV_KEY, FP_KEY_INPUT_POWER},
        {EV_KEY, FP_KEY_DOUBLE_CLICK},
};

#ifndef XIAOMI_DRM_INTERFACE_WA

static struct drm_panel *prim_panel;
static void *cookie = NULL;
#if IS_ENABLED(CONFIG_FP_HAVE_MULTI_SCREEN)
static struct drm_panel *sec_panel;
static void *cookie_sec = NULL;
#endif

static int fp_check_panel(struct device_node *np)
{
        int i;
        int count;
        struct device_node *node;
        struct drm_panel *panel;

        if(!np) {
                return -ENODEV;
        }
        count = of_count_phandle_with_args(np, "panel", NULL);
        if (count <= 0) {
#if IS_ENABLED(CONFIG_FP_HAVE_MULTI_SCREEN)
                goto find_sec_panel;
#endif
                return -ENODEV;
        }
        for (i = 0; i < count; i++) {
                node = of_parse_phandle(np, "panel", i);
                panel = of_drm_find_panel(node);
                of_node_put(node);
                if (!IS_ERR(panel)) {
                        prim_panel = panel;
                        break;
                } else {
                        prim_panel = NULL;
                }
        }
        if (PTR_ERR(prim_panel) == -EPROBE_DEFER) {
        }

#if IS_ENABLED(CONFIG_FP_HAVE_MULTI_SCREEN)
find_sec_panel:
        count = of_count_phandle_with_args(np, "panel1", NULL);
{}
        if (count <= 0)
                return -ENODEV;
        for (i = 0; i < count; i++) {
                node = of_parse_phandle(np, "panel1", i);
                panel = of_drm_find_panel(node);
                of_node_put(node);
                if (!IS_ERR(panel)) {
                        sec_panel = panel;
                        break;
                } else {
                        sec_panel = NULL;
                }
        }
        if (PTR_ERR(sec_panel) == -EPROBE_DEFER) {
        }
#endif
        return PTR_ERR(panel);
}

static void fp_screen_state_for_fingerprint_callback(enum panel_event_notifier_tag notifier_tag,
            struct panel_event_notification *notification, void *client_data)
{
        struct fp_device *fp_dev = client_data;
        if (!fp_dev)
                return;

        if (!notification) {
                return;
        }

        if(notification->notif_data.early_trigger) {
                return;
        }
        if(notifier_tag == PANEL_EVENT_NOTIFICATION_PRIMARY || notifier_tag == PANEL_EVENT_NOTIFICATION_SECONDARY){
		switch (notification->notif_type) {
			case DRM_PANEL_EVENT_UNBLANK:
				if (fp_dev->device_available == 1) {
					fp_dev->fb_black = 0;
					if(fp_dev->fp_netlink_enabled)
						fp_netlink_send(fp_dev, FP_NETLINK_SCREEN_ON);
				}
				break;
			case DRM_PANEL_EVENT_BLANK:
				if (fp_dev->device_available == 1) {
					fp_dev->fb_black = 1;
					fp_dev->wait_finger_down = true;
					if(fp_dev->fp_netlink_enabled)
                                               fp_netlink_send(fp_dev, FP_NETLINK_SCREEN_OFF);
				}
				break;
			default:
				break;
		}
	}
}

static void fp_register_panel_notifier_work(struct work_struct *work)
{
	struct fp_device *fp_dev = container_of(work, struct fp_device, screen_state_dw.work);
	int error = 0;
	static int retry_count = 0;
	struct device_node *node;
	node = of_find_node_by_name(NULL, "fingerprint-screen");
	if (!node) {
		return;
	}

	error = fp_check_panel(node);
	if (prim_panel) {
		if (!cookie) {
			cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
					PANEL_EVENT_NOTIFIER_CLIENT_FINGERPRINT, prim_panel,
					fp_screen_state_for_fingerprint_callback, (void*)fp_dev);
			if (IS_ERR(cookie))
				{}
			else
				{}
		}
	} else {
		if (retry_count++ < 5) {
			queue_delayed_work(fp_dev->screen_state_wq, &fp_dev->screen_state_dw, 5 * HZ);
		} else {
		}
        }

#if IS_ENABLED(CONFIG_FP_HAVE_MULTI_SCREEN)
        if (sec_panel) {
		if (!cookie_sec) {
			cookie_sec = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_SECONDARY,
					PANEL_EVENT_NOTIFIER_CLIENT_FINGERPRINT_SECOND, sec_panel,
					fp_screen_state_for_fingerprint_callback, (void*)fp_dev);
			if (IS_ERR(cookie_sec))
				{}
			else
				{}
		}
        } else {
		if (retry_count++ < 5) {
			queue_delayed_work(fp_dev->screen_state_wq, &fp_dev->screen_state_dw, 5 * HZ);
		} else {
		}
        }
#endif
}
#endif /*XIAOMI_DRM_INTERFACE_WA*/

static ssize_t irq_get(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fp_device *fp_dev = &fp;
	int irq = gpio_get_value(fp_dev->irq_gpio);

	return scnprintf(buf, PAGE_SIZE, "%i\n", irq);
}

static ssize_t irq_ack(struct device *dev,
	       	struct device_attribute *attr,
		const char *buf, size_t count)
{


	return count;
}

static DEVICE_ATTR(irq, S_IRUSR | S_IWUSR, irq_get, irq_ack);

#ifdef CONFIG_SIDE_FINGERPRINT
static ssize_t get_fingerdown_event(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct fp_device *fp_dev = &fp;

	return snprintf(buf, PAGE_SIZE, "%d\n", fp_dev->fingerdown);
}

static ssize_t set_fingerdown_event(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct fp_device *fp_dev = &fp;

	if (!strncmp(buf, "1", strlen("1"))) {
		fp_dev->fingerdown = 1;
		sysfs_notify(&fp_dev->driver_device->dev.kobj, NULL, "fingerdown");
	}
	else if (!strncmp(buf, "0", strlen("0"))) {
		fp_dev->fingerdown = 0;
	}
	else {
		return -EINVAL;
	}
	return count;
}
static DEVICE_ATTR(fingerdown, S_IRUSR | S_IWUSR, get_fingerdown_event, set_fingerdown_event);
#endif

static struct attribute *attributes[] = {
	&dev_attr_irq.attr,
#ifdef CONFIG_SIDE_FINGERPRINT
	&dev_attr_fingerdown.attr,
#endif
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

static irqreturn_t fp_irq(int irq, void *handle)
{
	struct fp_device *fp_dev = (struct fp_device *)handle;
#ifdef CONFIG_SIDE_FINGERPRINT
	sysfs_notify(&fp_dev->driver_device->dev.kobj, NULL, dev_attr_irq.attr.name);
#endif
	__pm_wakeup_event(fp_wakesrc, WAKELOCK_HOLD_TIME);
	fp_netlink_send(fp_dev, FP_NETLINK_IRQ);
	if ((fp_dev->wait_finger_down == true) && (fp_dev->fb_black == 1)) {
	       	fp_dev->wait_finger_down = false;
	}
	return IRQ_HANDLED;
}

/* -------------------------------------------------------------------- */
/* file operation function                                              */
/* -------------------------------------------------------------------- */

static long fp_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct fp_device *fp_dev = &fp;
	struct fp_key fp_key;
	int retval = 0;
	u8 buf = 0;
	u8 netlink_route = fp_dev->fp_netlink_num;
	struct fp_ioc_chip_info info;
	char vendor_name[10];

	if (_IOC_TYPE(cmd) != FP_IOC_MAGIC)
		return -EINVAL;

	if (_IOC_DIR(cmd) & _IOC_READ)
		retval =!access_ok( (void __user *)arg,_IOC_SIZE(cmd));
	if (retval == 0 && _IOC_DIR(cmd) & _IOC_WRITE)
		retval =!access_ok((void __user *)arg, _IOC_SIZE(cmd));
	if (retval)
		return -EINVAL;

	switch (cmd) {
		case FP_IOC_INIT:
			if(fp_dev->fp_netlink_num <= 0){
				retval = -EFAULT;
				break;
			}
			if(fp_dev->fp_netlink_enabled == 0) {
				retval = fp_netlink_init(fp_dev);
				if(retval != 0) {
					break;
				}
			}
			if(copy_to_user((void __user *)arg, (void *)&netlink_route, sizeof(u8))) {
				retval = -EFAULT;
				break;
			}
			fp_dev->fp_netlink_enabled = 1;
			break;
		case FP_IOC_EXIT:
			fp_disable_irq(fp_dev);
			if(fp_dev->fp_netlink_enabled)
				fp_netlink_destroy(fp_dev);
			fp_dev->fp_netlink_enabled = 0;
			fp_dev->device_available = 0;
			break;

		case FP_IOC_ENABLE_IRQ:
			fp_enable_irq(fp_dev);
			break;

		case FP_IOC_DISABLE_IRQ:
			fp_disable_irq(fp_dev);
			break;

		case FP_IOC_RESET:
			fp_hw_reset(fp_dev, 60);
			break;

		case FP_IOC_ENABLE_POWER:
			if (fp_dev->device_available == 1) {
			} else {
				if(copy_from_user((char *)vendor_name, (char *)arg,10*sizeof(char))) {
					retval = -EFAULT;
					break;
				}
				if (!strcmp(vendor_name, "goodix"))
					fp_dev->vreg = regulator_get(&fp_dev->driver_device->dev, DTS_VOlT_REGULATER_GD);
				else if (!strcmp(vendor_name, "fpc"))
					fp_dev->vreg = regulator_get(&fp_dev->driver_device->dev, DTS_VOlT_REGULATER_FPC);
				else {
					return -EPERM;
				}
				if (IS_ERR(fp_dev->vreg)) {
					return -EPERM;
				}
				fp_power_on(fp_dev);
				fp_dev->device_available = 1;
			}
			break;
		case FP_IOC_DISABLE_POWER:
			if (fp_dev->device_available == 0) {
			} else {
				fp_power_off(fp_dev);
				fp_dev->device_available = 0;
			}
			break;
		case FP_IOC_ENABLE_SPI_CLK:
#ifdef CONFIG_FP_MTK_PLATFORM
			{}
			fp_spi_clk_enable(fp_dev->driver_device);
#endif
			break;
		case FP_IOC_DISABLE_SPI_CLK:
#ifdef CONFIG_FP_MTK_PLATFORM
			{}
			fp_spi_clk_disable(fp_dev->driver_device);
#endif
			break;

		case FP_IOC_INPUT_KEY_EVENT:
			if (copy_from_user(&fp_key, (struct fp_key *)arg, sizeof(struct fp_key))) {
				retval = -EFAULT;
				break;
			}
			fp_kernel_key_input(fp_dev, &fp_key);
			break;

		case FP_IOC_ENTER_SLEEP_MODE:
			break;
		case FP_IOC_GET_FW_INFO:
			if (copy_to_user((void __user *)arg, (void *)&buf, sizeof(u8))) {
				retval = -EFAULT;
			}
			break;

		case FP_IOC_REMOVE:
			break;

		case FP_IOC_CHIP_INFO:
			if (copy_from_user(&info, (struct fp_ioc_chip_info *)arg,sizeof(struct fp_ioc_chip_info))) {
				retval = -EFAULT;
				break;
			}
			break;

		default:
			break;
	}

	return retval;
}

#ifdef CONFIG_COMPAT
static long fp_compat_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	int retval = 0;
	retval = filp->f_op->unlocked_ioctl(filp, cmd, arg);
	return retval;
}
#endif

/* -------------------------------------------------------------------- */
/* device function							*/
/* -------------------------------------------------------------------- */
static int fp_open(struct inode *inode, struct file *filp)
{
	struct fp_device *fp_dev = NULL;
	int status = -ENXIO;

	mutex_lock(&device_list_lock);
	list_for_each_entry(fp_dev, &device_list, device_entry) {
		if (fp_dev->devt == inode->i_rdev) {
			status = 0;
			break;
		}
	}
	mutex_unlock(&device_list_lock);

	if (status == 0) {
		filp->private_data = fp_dev;
		nonseekable_open(inode, filp);
		gpio_direction_input(fp_dev->irq_gpio);
		status = request_threaded_irq(fp_dev->irq_num, NULL, fp_irq,
			       	IRQF_TRIGGER_RISING |
				IRQF_ONESHOT, "xiaomi_fp_irq",
				fp_dev);
		if (!status){
			fp_dev->irq_enabled = 1;
			//fp_disable_irq(fp_dev);
		}
		else{
		}
#ifndef XIAOMI_DRM_INTERFACE_WA
		if (fp_dev->screen_state_wq) {
			queue_delayed_work(fp_dev->screen_state_wq, &fp_dev->screen_state_dw, 5 * HZ);
		}
#endif
	} else {
	}
	return status;
}

static int fp_release(struct inode *inode, struct file *filp)
{
	struct fp_device *fp_dev = NULL;
	int status = 0;
	fp_dev = filp->private_data;
	if (fp_dev->irq_num){
		fp_disable_irq(fp_dev);
		free_irq(fp_dev->irq_num, fp_dev);
	}
#ifndef XIAOMI_DRM_INTERFACE_WA
	if (fp_dev->screen_state_wq) {
		cancel_delayed_work_sync(&fp_dev->screen_state_dw);
	}
#endif
	return status;
}
/*
static unsigned int fp_poll(struct file *filp, poll_table *wait)
{
    struct fp_device *fp_dev = &fp;
    unsigned int mask = 0;
	FUNC_ENTRY();

    poll_wait(filp, &fp_dev->fp_wait_queue,  wait);
	if(fp_dev->fp_poll_have_data)
		mask = POLLERR | POLLPRI;
	pr_err("fp_poll end!");
    return mask;
}

static ssize_t fp_read(struct file *filp, char __user *buf, size_t size, loff_t *ppos)
{
	struct fp_device *fp_dev = &fp;
	char value;
	FUNC_ENTRY();
	value = (char)gpio_get_value(fp_dev->irq_gpio)+'0';
	pr_err("value = %c", value);
	if(copy_to_user(buf,&value,1)) {
		pr_err("fp_read copy_to_user error");
		return -EFAULT;
	} else {
		fp_dev->fp_poll_have_data = 1;
		pr_err("fp_read success size =%d, value =%c", size, value);
		return size;
	}
}
*/

static const struct file_operations fp_fops = {
	.owner = THIS_MODULE,
	.open = fp_open,
	.unlocked_ioctl = fp_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = fp_compat_ioctl,
#endif
	.release = fp_release,
//	.poll = fp_poll,
//	.read = fp_read,
};

#ifdef CONFIG_FP_MTK_PLATFORM
static int fp_probe(struct spi_device *driver_device)
#else
static int fp_probe(struct platform_device *driver_device)
#endif
{
	struct fp_device *fp_dev = &fp;
	int status = -EINVAL;
#ifdef CONFIG_FP_MTK_PLATFORM
	int ret = 0;
#endif
	int i;

	INIT_LIST_HEAD(&fp_dev->device_entry);

	fp_dev->irq_gpio = -EINVAL;
	fp_dev->device_available = 0;
	fp_dev->fb_black = 0;
	fp_dev->wait_finger_down = false;
	fp_dev->fp_netlink_enabled = 0;
	fp_dev->fingerdown = 0;
	/*setup fp configurations. */
	fp_dev->driver_device = driver_device;
	/* get gpio info from dts or defination */
	status = fp_parse_dts(fp_dev);
	if(status){
		goto err_dts;
	}
#ifdef CONFIG_FP_MTK_PLATFORM
	if (!IS_ERR(fp_dev->pins_eint_default)) {
		pinctrl_select_state(fp_dev->pinctrl, fp_dev->pins_eint_default);
	}

	if (IS_ERR(fp_dev->pins_spiio_spi_mode)) {
		ret = PTR_ERR(fp_dev->pins_spiio_spi_mode);
		return ret;
	} else {
		pinctrl_select_state(fp_dev->pinctrl, fp_dev->pins_spiio_spi_mode);
	}
#endif
	/* create class */
	fp_dev->class = class_create(THIS_MODULE, FP_CLASS_NAME);
	if (IS_ERR(fp_dev->class)) {
		status = -ENODEV;
		goto err_class;
	}
	/* get device no */
	status = alloc_chrdev_region(&fp_dev->devt, 0,1, FP_DEV_NAME);
	if (status < 0) {
		goto err_devno;
	}
	/* create device */
	fp_dev->device = device_create(fp_dev->class, &driver_device->dev, fp_dev->devt,fp_dev,FP_DEV_NAME);
	if (IS_ERR(fp_dev->device)) {
		status = -ENODEV;
		goto err_device;
	} else {
		mutex_lock(&device_list_lock);
		list_add(&fp_dev->device_entry, &device_list);
		mutex_unlock(&device_list_lock);
	}
	/* cdev init and add */
	cdev_init(&fp_dev->cdev, &fp_fops);
	fp_dev->cdev.owner = THIS_MODULE;
	status = cdev_add(&fp_dev->cdev, fp_dev->devt, 1);
	if (status) {
		goto err_cdev;
	}
	/*register device within input system. */
	fp_dev->input = input_allocate_device();
	if (fp_dev->input == NULL) {
		status = -ENOMEM;
		goto err_input;
	}
	for (i = 0; i < ARRAY_SIZE(maps); i++) {
		input_set_capability(fp_dev->input, maps[i].type, maps[i].code);
	}
	fp_dev->input->name = FP_INPUT_NAME;
	fp_dev->input->id.vendor  = 0x0666;
	fp_dev->input->id.product = 0x0888;
	if (input_register_device(fp_dev->input)) {
		status = -ENODEV;
		goto err_input_2;
	}
	init_waitqueue_head(&fp_dev->fp_wait_queue);
#ifndef XIAOMI_DRM_INTERFACE_WA
        fp_dev->screen_state_wq = create_singlethread_workqueue("screen_state_wq");
        if (fp_dev->screen_state_wq){
                INIT_DELAYED_WORK(&fp_dev->screen_state_dw, fp_register_panel_notifier_work);
        }
#endif
	/* netlink interface init */
	status = fp_netlink_init(fp_dev);
	if (status == -1) {
		input_unregister_device(fp_dev->input);
		fp_dev->input = NULL;
		goto err_input;
	}
	fp_dev->fp_netlink_enabled = 1;

	fp_wakesrc = wakeup_source_register(&fp_dev->driver_device->dev,"fp_wakesrc");
#ifndef CONFIG_SIDE_FINGERPRINT
	if (device_may_wakeup(fp_dev->device)) {
		disable_irq_wake(fp_dev->irq_num);
	}
	{}
#else
	status = sysfs_create_group(&fp_dev->driver_device->dev.kobj, &attribute_group);
	{}
	enable_irq_wake(fp_dev->irq_num);
	{}
#endif
	return 0;

err_input_2:

	if (fp_dev->input != NULL) {
		input_free_device(fp_dev->input);
		fp_dev->input = NULL;
	}

err_input:
	cdev_del(&fp_dev->cdev);

err_cdev:
	device_destroy(fp_dev->class, fp_dev->devt);
	list_del(&fp_dev->device_entry);

err_device:
	unregister_chrdev_region(fp_dev->devt, 1);

err_devno:
	class_destroy(fp_dev->class);

err_class:
#ifdef CONFIG_FP_MTK_PLATFORM
	fp_spi_clk_disable(fp_dev->driver_device);
	fp_power_off(fp_dev);
#endif

err_dts:
	fp_dev->driver_device = NULL;
	fp_dev->device_available = 0;
	return status;
}

#ifdef CONFIG_FP_MTK_PLATFORM
static int fp_remove(struct spi_device *driver_device)
#else
static int fp_remove(struct platform_device *driver_device)
#endif
{
	struct fp_device *fp_dev = &fp;
	wakeup_source_unregister(fp_wakesrc);
	fp_wakesrc = NULL;
	/* make sure ops on existing fds can abort cleanly */
	if (fp_dev->irq_num) {
		free_irq(fp_dev->irq_num, fp_dev);
		fp_dev->irq_enabled = 0;
		fp_dev->irq_num = 0;
	}

#ifndef XIAOMI_DRM_INTERFACE_WA
	if (fp_dev->screen_state_wq) {
		destroy_workqueue(fp_dev->screen_state_wq);
	}

	if (prim_panel && !IS_ERR(cookie)) {
		panel_event_notifier_unregister(cookie);
	} else {
	}
#if IS_ENABLED(CONFIG_FP_HAVE_MULTI_SCREEN)
	if (sec_panel && !IS_ERR(cookie_sec)) {
		panel_event_notifier_unregister(cookie_sec);
	} else {
	}
#endif
#endif

	if (fp_dev->input != NULL) {
		input_unregister_device(fp_dev->input);
		input_free_device(fp_dev->input);
	}
	fp_netlink_destroy(fp_dev);
	fp_dev->fp_netlink_enabled = 0;
	cdev_del(&fp_dev->cdev);
	device_destroy(fp_dev->class, fp_dev->devt);
	list_del(&fp_dev->device_entry);

	unregister_chrdev_region(fp_dev->devt, 1);
	class_destroy(fp_dev->class);
#ifdef CONFIG_FP_MTK_PLATFORM
	if (!IS_ERR(fp_dev->pins_spiio_spi_mode)) {
		pinctrl_select_state(fp_dev->pinctrl, fp_dev->pins_spiio_gpio_mode);
	}
	if (!IS_ERR(fp_dev->pins_reset_low)) {
		pinctrl_select_state(fp_dev->pinctrl, fp_dev->pins_reset_low);
	}
	if (!IS_ERR(fp_dev->pins_eint_default)) {
		pinctrl_select_state(fp_dev->pinctrl, fp_dev->pins_eint_default);
	}
#endif
	fp_power_off(fp_dev);
	fp_dev->driver_device = NULL;
	return 0;
}


static const struct of_device_id fp_of_match[] = {
	{.compatible = DRIVER_COMPATIBLE,},
	{},
};
MODULE_DEVICE_TABLE(of, fp_of_match);

#ifdef CONFIG_FP_MTK_PLATFORM
static struct spi_driver fp_spi_driver = {
#else
static struct platform_driver fp_platform_driver = {
#endif
	.driver = {
		   .name = FP_DEV_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = fp_of_match,
		   },
	.probe = fp_probe,
	.remove = fp_remove,
};

/*-------------------------------------------------------------------------*/
static int __init fp_init(void)
{
	int status = 0;
#ifdef CONFIG_FP_MTK_PLATFORM
	status = spi_register_driver(&fp_spi_driver);
#else
	status = platform_driver_register(&fp_platform_driver);
#endif
	if (status < 0) {
		return -EINVAL;
	}
	return status;
}

module_init(fp_init);

static void __exit fp_exit(void)
{
#ifdef CONFIG_FP_MTK_PLATFORM
	spi_unregister_driver(&fp_spi_driver);
#else
	platform_driver_unregister(&fp_platform_driver);
#endif
}

module_exit(fp_exit);

MODULE_AUTHOR("xiaomi");
MODULE_DESCRIPTION("Xiaomi Fingerprint driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("xiaomi-fp");
