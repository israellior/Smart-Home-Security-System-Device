// SPDX-License-Identifier: GPL-2.0
/*
 * doorbell.c - the doorbell button as a kernel driver, behind /dev/doorbell.
 *
 * A learning exercise, not part of the shipped device: porchlightd still reads
 * the button through libgpiod, and this driver and porchlightd cannot both
 * hold GPIO24. Stop porchlightd before loading this, or probe fails -EBUSY.
 *
 * The path of one press, top to bottom:
 *
 *   button -> GPIO24 edge -> doorbell_irq()        hard IRQ: timestamp, arm a timer
 *          -> 30 ms of quiet -> doorbell_debounced() process context: read the
 *                                                  line, count the press, wake readers
 *          -> doorbell_read() / doorbell_poll()    copy one line to user space
 *
 * Each press reads as one text line, "<seq> <monotonic ns>\n", so `cat` is a
 * test client. A reader that falls behind gets the newest press, and sees how
 * many it missed from the gap in seq: a doorbell cares that someone is at the
 * door now, not about a queue of old presses.
 *
 * ---------------------------------------------------------------------------
 * Building and loading, on the Pi
 * ---------------------------------------------------------------------------
 *
 * Kernel headers for the running kernel, and dtc:
 *
 *   sudo apt install linux-headers-rpi-v8 device-tree-compiler
 *
 * Kbuild insists on a Kbuild or Makefile beside the source, so make the
 * one-line one and build against the running kernel:
 *
 *   echo 'obj-m := doorbell.o' > Kbuild
 *   make -C /lib/modules/$(uname -r)/build M=$PWD modules
 *
 * The driver binds to a Device Tree node, so the button has to be described
 * there. The overlay is below; pull it out of this file and compile it:
 *
 *   sed -n '/^ \* BEGIN doorbell.dts$/,/^ \* END doorbell.dts$/{//!p}' doorbell.c > doorbell.dts
 *   dtc -@ -I dts -O dtb -o doorbell.dtbo doorbell.dts
 *
 * Then, with the line free:
 *
 *   sudo systemctl stop porchlightd
 *   sudo insmod doorbell.ko
 *   sudo dtoverlay -d . doorbell        # the node appears, probe runs
 *   dmesg | tail                        # "button on irq N, /dev/doorbell"
 *   cat /dev/doorbell                   # press the button
 *   grep doorbell /proc/interrupts      # the count behind each press
 *
 * And back:
 *
 *   sudo dtoverlay -r doorbell          # close every reader first, see remove
 *   sudo rmmod doorbell
 *   sudo systemctl start porchlightd
 *
 * BEGIN doorbell.dts
/dts-v1/;
/plugin/;

/ {
	compatible = "brcm,bcm2711";

	fragment@0 {
		target-path = "/";
		__overlay__ {
			doorbell-button {
				compatible = "porchlight,doorbell-button";
				// 17 = GPIO_ACTIVE_LOW (1) | GPIO_PULL_UP (16). The
				// button is a bare switch to ground with no external
				// resistor, so without the pull-up the line floats.
				button-gpios = <&gpio 24 17>;
			};
		};
	};
};
 * END doorbell.dts
 */

#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

/* Same as porchlightd's line request. */
#define DOORBELL_DEBOUNCE_MS 30

struct doorbell {
	struct device *dev;
	struct gpio_desc *gpio;
	int irq;

	/* Re-armed by every edge, so it runs once the contact stops bouncing. */
	struct delayed_work debounce;
	bool pressed;		/* only touched by the work item */

	/* Written by the hard IRQ handler without a lock; a u64 store is
	 * single-copy atomic on arm64, which READ_ONCE/WRITE_ONCE rely on. */
	u64 edge_ns;

	spinlock_t lock;	/* keeps seq and press_ns consistent as a pair */
	u64 seq;		/* presses since probe */
	u64 press_ns;		/* when the newest one happened */

	wait_queue_head_t wq;	/* readers sleeping in read() or poll() */
	struct miscdevice misc;
};

/* One per open(), so two readers each see every press. */
struct doorbell_reader {
	struct doorbell *db;
	u64 seen;
};

/*
 * Hard IRQ context: interrupts are off on this CPU and nothing here may
 * sleep. So it only takes the timestamp, which is most accurate here, and
 * pushes everything else to the work item.
 *
 * A contact bounces for a few milliseconds, so one press is a burst of edges.
 * Each edge pushes the timer back; the work runs only after 30 ms of quiet.
 * This is how gpio-keys debounces in software too: the BCM2711 GPIO block
 * has no hardware debounce.
 */
static irqreturn_t doorbell_irq(int irq, void *data)
{
	struct doorbell *db = data;

	WRITE_ONCE(db->edge_ns, ktime_get_ns());
	mod_delayed_work(system_wq, &db->debounce,
			 msecs_to_jiffies(DOORBELL_DEBOUNCE_MS));
	return IRQ_HANDLED;
}

/*
 * Process context, so the _cansleep accessor is allowed. The IRQ fires on
 * both edges, because the trigger flags are *physical*; this reads the
 * *logical* value, so active-low is handled by the DT flag and nowhere else.
 */
static void doorbell_debounced(struct work_struct *work)
{
	struct doorbell *db = container_of(to_delayed_work(work),
					   struct doorbell, debounce);
	int val = gpiod_get_value_cansleep(db->gpio);

	if (val < 0) {
		dev_warn_ratelimited(db->dev, "reading the line failed: %d\n", val);
		return;
	}

	/* Only the release-to-press transition counts; a release is silent. */
	if (val && !db->pressed) {
		spin_lock(&db->lock);
		db->press_ns = READ_ONCE(db->edge_ns);
		WRITE_ONCE(db->seq, db->seq + 1);
		spin_unlock(&db->lock);

		wake_up_interruptible(&db->wq);
	}
	db->pressed = val;
}

static bool doorbell_ready(struct doorbell_reader *r)
{
	return READ_ONCE(r->db->seq) != r->seen;
}

static int doorbell_open(struct inode *inode, struct file *file)
{
	/* misc_open() left our miscdevice in private_data before calling us. */
	struct doorbell *db = container_of(file->private_data,
					   struct doorbell, misc);
	struct doorbell_reader *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;

	/* Like gpiomon: only presses from now on, not the history. */
	r->db = db;
	r->seen = READ_ONCE(db->seq);
	file->private_data = r;

	return stream_open(inode, file);
}

static int doorbell_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static ssize_t doorbell_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct doorbell_reader *r = file->private_data;
	struct doorbell *db = r->db;
	char line[48];		/* two u64s in decimal, a space, a newline */
	u64 seq, ns;
	int len, ret;

	if (file->f_flags & O_NONBLOCK) {
		if (!doorbell_ready(r))
			return -EAGAIN;
	} else {
		/* Sleeps on db->wq until a press, or a signal (-ERESTARTSYS). */
		ret = wait_event_interruptible(db->wq, doorbell_ready(r));
		if (ret)
			return ret;
	}

	spin_lock(&db->lock);
	seq = db->seq;
	ns = db->press_ns;
	spin_unlock(&db->lock);

	len = scnprintf(line, sizeof(line), "%llu %llu\n", seq, ns);

	/* A press is one line or nothing: half a line would be a torn event. */
	if (count < len)
		return -EINVAL;

	/*
	 * buf is a user-space address. It may be unmapped, or point into the
	 * kernel, so it is never dereferenced directly; copy_to_user checks it
	 * and returns how many bytes it could *not* copy.
	 */
	if (copy_to_user(buf, line, len))
		return -EFAULT;

	r->seen = seq;
	return len;
}

/*
 * poll() does not sleep here. poll_wait() registers this file's interest in
 * db->wq, and the core sleeps for us; wake_up_interruptible() in the work
 * item then makes it call this again, and the answer is what it returns.
 */
static __poll_t doorbell_poll(struct file *file, poll_table *wait)
{
	struct doorbell_reader *r = file->private_data;

	poll_wait(file, &r->db->wq, wait);
	return doorbell_ready(r) ? EPOLLIN | EPOLLRDNORM : 0;
}

static const struct file_operations doorbell_fops = {
	.owner = THIS_MODULE,	/* rmmod refuses while a reader is open */
	.open = doorbell_open,
	.release = doorbell_release,
	.read = doorbell_read,
	.poll = doorbell_poll,
};

static void doorbell_cancel_work(void *data)
{
	struct doorbell *db = data;

	cancel_delayed_work_sync(&db->debounce);
}

static void doorbell_misc_deregister(void *data)
{
	misc_deregister(data);
}

/*
 * Everything is devm_, so there is no remove(): the core undoes each step in
 * reverse order when the device goes. That order is the design. Teardown runs
 * the misc device out first, then frees the IRQ, then cancels the work - so
 * the IRQ cannot re-arm the work after it has been cancelled.
 *
 * Not handled: removing the overlay while /dev/doorbell is open. rmmod is
 * refused then (fops.owner), but an unbind is not, and an open reader would
 * be left pointing at freed memory. Close readers first.
 */
static int doorbell_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct doorbell *db;
	int ret;

	db = devm_kzalloc(dev, sizeof(*db), GFP_KERNEL);
	if (!db)
		return -ENOMEM;

	db->dev = dev;
	spin_lock_init(&db->lock);
	init_waitqueue_head(&db->wq);
	INIT_DELAYED_WORK(&db->debounce, doorbell_debounced);

	/* "button" + "-gpios" is the property name in the overlay. */
	db->gpio = devm_gpiod_get(dev, "button", GPIOD_IN);
	if (IS_ERR(db->gpio))
		return dev_err_probe(dev, PTR_ERR(db->gpio),
				     "no button-gpios (is porchlightd holding it?)\n");

	/* Held down at load time is not a press. */
	ret = gpiod_get_value_cansleep(db->gpio);
	db->pressed = ret > 0;

	db->irq = gpiod_to_irq(db->gpio);
	if (db->irq < 0)
		return dev_err_probe(dev, db->irq, "line has no IRQ\n");

	/* Registered before the IRQ so that it is undone after it. */
	ret = devm_add_action_or_reset(dev, doorbell_cancel_work, db);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, db->irq, doorbell_irq,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			       "doorbell", db);
	if (ret)
		return dev_err_probe(dev, ret, "requesting irq %d\n", db->irq);

	db->misc.minor = MISC_DYNAMIC_MINOR;
	db->misc.name = "doorbell";
	db->misc.fops = &doorbell_fops;
	db->misc.parent = dev;
	/* World-readable: porchlightd runs as its own user, not root. */
	db->misc.mode = 0444;

	ret = misc_register(&db->misc);
	if (ret)
		return dev_err_probe(dev, ret, "registering /dev/doorbell\n");

	ret = devm_add_action_or_reset(dev, doorbell_misc_deregister, &db->misc);
	if (ret)
		return ret;

	dev_info(dev, "button on irq %d, /dev/%s\n", db->irq, db->misc.name);
	return 0;
}

static const struct of_device_id doorbell_of_match[] = {
	{ .compatible = "porchlight,doorbell-button" },
	{ }
};
MODULE_DEVICE_TABLE(of, doorbell_of_match);

static struct platform_driver doorbell_driver = {
	.probe = doorbell_probe,
	.driver = {
		.name = "doorbell",
		.of_match_table = doorbell_of_match,
		/* No unbind through sysfs: see the note above probe. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(doorbell_driver);

MODULE_DESCRIPTION("Doorbell button: GPIO interrupt to /dev/doorbell");
MODULE_LICENSE("GPL");	/* the gpiod_ and misc_ symbols are GPL-only */
