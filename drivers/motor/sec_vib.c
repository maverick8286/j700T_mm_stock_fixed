/* drivers/motor/dcmotor.c

 * Copyright (C) 2015 Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */


#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/hrtimer.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>

#include "../staging/android/timed_output.h"
#if defined(CONFIG_IMM_VIB)
#include "imm_vib.h"
#endif

#define SEC_VIB_NAME "sec_vib"

struct sec_vib_pdata {
	const char *regulator;
	int max_timeout;
};

struct sec_vib_drvdata {
	struct regulator *regulator;
	struct timed_output_dev dev;
	struct hrtimer timer;
	struct workqueue_struct *workqueue;
	struct work_struct work;
	spinlock_t lock;
	bool running;
	int max_timeout;
	int timeout;
};

static enum hrtimer_restart sec_vib_timer_func(struct hrtimer *timer)
{
	struct sec_vib_drvdata *ddata =
		container_of(timer, struct sec_vib_drvdata, timer);

	ddata->timeout = 0;
	queue_work(ddata->workqueue, &ddata->work);
	return HRTIMER_NORESTART;
}

static int sec_vib_get_time(struct timed_output_dev *dev)
{
	struct sec_vib_drvdata *ddata =
		container_of(dev, struct sec_vib_drvdata, dev);

	if (hrtimer_active(&ddata->timer)) {
		ktime_t r = hrtimer_get_remaining(&ddata->timer);
		struct timeval t = ktime_to_timeval(r);
		return t.tv_sec * 1000 + t.tv_usec / 1000;
	} else
		return 0;
}

static void sec_vib_enable(struct timed_output_dev *dev, int value)
{
	struct sec_vib_drvdata *ddata =
		container_of(dev, struct sec_vib_drvdata, dev);
	unsigned long	flags;

	hrtimer_cancel(&ddata->timer);

	if (value > 0) {
		if (value > ddata->max_timeout)
			value = ddata->max_timeout;
		
		ddata->timeout = value;
		queue_work(ddata->workqueue, &ddata->work);

		spin_lock_irqsave(&ddata->lock, flags);

		hrtimer_start(&ddata->timer,
			ktime_set(value / 1000, (value % 1000) * 1000000),
			HRTIMER_MODE_REL);

		spin_unlock_irqrestore(&ddata->lock, flags);
	} else {
		ddata->timeout = 0;
		queue_work(ddata->workqueue, &ddata->work);
	}
}

static void sec_vib_work(struct work_struct *work)
{
	struct sec_vib_drvdata *ddata = 
		container_of(work, struct sec_vib_drvdata, work);
	int ret = 0;

	if (ddata->timeout > 0) {
		if (ddata->running)
			return;

		ret = regulator_enable(ddata->regulator);

		ddata->running = true;
	} else {
		if (!ddata->running)
			return;

		regulator_disable(ddata->regulator);

		ddata->running = false;
	}
	return;
}

#if defined(CONFIG_OF)
static struct sec_vib_pdata *sec_vib_get_dt(struct device *dev)
{
	struct device_node *node, *child_node;
	struct sec_vib_pdata *pdata;
	int ret = 0;

	node = dev->of_node;
	if (!node) {
		ret = -ENODEV;
		goto err_out;
	}

	child_node = of_get_next_child(node, child_node);
	if (!child_node) {
		printk("[VIB] failed to get dt node\n");
		ret = -EINVAL;
		goto err_out;
	}

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		printk("[VIB] failed to alloc\n");
		ret = -ENOMEM;
		goto err_out;
	}

	of_property_read_u32(child_node, "sec_vib,max_timeout", &pdata->max_timeout);
	of_property_read_string(child_node, "sec_vib,regulator", &pdata->regulator);

	return pdata;

err_out:
	return ERR_PTR(ret);
}
#endif

static int sec_vib_probe(struct platform_device *pdev)
{
	struct sec_vib_pdata *pdata = pdev->dev.platform_data;
	struct sec_vib_drvdata *ddata;
	int ret = 0;

	if (!pdata) {
#if defined(CONFIG_OF)
		pdata = sec_vib_get_dt(&pdev->dev);
		if (IS_ERR(pdata)) {
			printk(KERN_ERR "[VIB] there is no device tree!\n");
			ret = -ENODEV;
			goto err_out;
		}
#else
		printk(KERN_ERR "[VIB] there is no platform data!\n");
		ret = -ENODEV;
		goto err_out;
#endif
	}

	ddata = kzalloc(sizeof(struct sec_vib_drvdata), GFP_KERNEL);
	if (!ddata) {
		ret = -ENOMEM;
		goto err_out;
	}

	ddata->regulator = regulator_get(NULL, pdata->regulator);
	if (IS_ERR(ddata->regulator)) {
		printk(KERN_ERR "[VIB] failed get %s\n", pdata->regulator);
		ret = PTR_ERR(ddata->regulator);
		goto err_out;
	}

	hrtimer_init(&ddata->timer, CLOCK_MONOTONIC,
			HRTIMER_MODE_REL);

	ddata->timer.function = sec_vib_timer_func;
	spin_lock_init(&ddata->lock);

	ddata->max_timeout = pdata->max_timeout;

	ddata->workqueue = create_singlethread_workqueue("sec_vib_work");
	INIT_WORK(&(ddata->work), sec_vib_work);

	ddata->dev.name = "vibrator";
	ddata->dev.get_time = sec_vib_get_time;
	ddata->dev.enable = sec_vib_enable;

	ret = timed_output_dev_register(&ddata->dev);
	if (ret < 0)
		goto err_out;

	platform_set_drvdata(pdev, ddata);

	return 0;

err_out:
	kfree(ddata);

	return ret;
}

static int sec_vib_remove(struct platform_device *pdev)
{
	struct sec_vib_drvdata *ddata = platform_get_drvdata(pdev);

	timed_output_dev_unregister(&ddata->dev);
	kfree(ddata);
	return 0;
}

#if defined(CONFIG_OF)
static struct of_device_id sec_vib_dt_ids[] = {
	{ .compatible = "sec_vib" },
	{ }
};
MODULE_DEVICE_TABLE(of, sec_vib_dt_ids);
#endif /* CONFIG_OF */

static struct platform_driver sec_vib_driver = {
	.probe		= sec_vib_probe,
	.remove		= sec_vib_remove,
	.driver		= {
		.name		= SEC_VIB_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= of_match_ptr(sec_vib_dt_ids),
	},
};

static int __init sec_vib_init(void)
{
	return platform_driver_register(&sec_vib_driver);
}
module_init(sec_vib_init);

static void __exit sec_vib_exit(void)
{
	platform_driver_unregister(&sec_vib_driver);
}
module_exit(sec_vib_exit);

MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("dc motor driver");
MODULE_LICENSE("GPL");
