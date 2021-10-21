/*
 *  Linux kernel modules for 3-Axis Orientation/Motion
 *  Detection Sensor for mma8653 (Based on mma8453)
 *
 *  Copyright 2009-2011 Amazon Technologies Inc., All rights reserved
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/input-polldev.h>
#include <linux/sysfs.h>
#include <linux/sysdev.h>
#include <linux/workqueue.h>

#include <mach/hardware.h>
#include <asm/setup.h>
#include <asm/io.h>
#include <asm/irq.h>

extern int gpio_accelerometer_int1(void);
extern int gpio_accelerometer_int2(void);

/*
 * Defines
 */
#define assert(expr)\
	if (!(expr)) {\
		printk(KERN_ERR "Assertion failed! %s,%d,%s,%s\n",\
			__FILE__, __LINE__, __func__, #expr);\
	}


    
    #define MMA8653_DRV_NAME	"mma8653"
    #define MMA8653_ID		0x5A
    #define MMA8653_I2C_ADDR	0x1D


#define POLL_INTERVAL		100
#define INPUT_FUZZ		32
#define INPUT_FLAT		32
#define MODE_CHANGE_DELAY_MS	100
#define DATA_RATE_SELECTION	0x20 /* 50HZ */

#define MAX_KEY_CODES 4

/* Accelerometer IRQ */
static int irq1;
static int irq2; 

//#define POLLING_MODE

atomic_t mma8653_suspended = ATOMIC_INIT(0);
u8       current_rotation  = 0;
static int keycodes[MAX_KEY_CODES] = 
{
	KEY_F1,
	KEY_F2,
	KEY_F3,
	KEY_F4, 
};

/* register enum for mma8653 registers */
enum {
	MMA8653_STATUS = 0x00,
	MMA8653_OUT_X_MSB,
	MMA8653_OUT_X_LSB,
	MMA8653_OUT_Y_MSB,
	MMA8653_OUT_Y_LSB,
	MMA8653_OUT_Z_MSB,
	MMA8653_OUT_Z_LSB,
    
	MMA8653_SYSMOD = 0x0B,
	MMA8653_INT_SOURCE,
	MMA8653_WHO_AM_I,
	MMA8653_XYZ_DATA_CFG,
	MMA8653_HP_FILTER_CUTOFF,

	MMA8653_PL_STATUS,
	MMA8653_PL_CFG,
	MMA8653_PL_COUNT,
	MMA8653_PL_BF_ZCOMP,
	MMA8653_P_L_THS_REG,

	MMA8653_FF_MT_CFG,
	MMA8653_FF_MT_SRC,
	MMA8653_FF_MT_THS,
	MMA8653_FF_MT_COUNT,

	MMA8653_ASLP_COUNT = 0x29,
	MMA8653_CTRL_REG1,
	MMA8653_CTRL_REG2,
	MMA8653_CTRL_REG3,
	MMA8653_CTRL_REG4,
	MMA8653_CTRL_REG5,

	MMA8653_OFF_X,
	MMA8653_OFF_Y,
	MMA8653_OFF_Z,

	MMA8653_REG_END,
};

enum {
	MODE_2G = 0,
	MODE_4G,
	MODE_8G,
};

#define INT_SOURCE_LNDPRT	0x10
#define INT_SOURCE_TRANS	0x20

/* mma8653 status */
struct mma8653_status {
	u8 mode;
	u8 ctl_reg1;
};

static struct mma8653_status mma_status = {
	.mode = 0,
	.ctl_reg1 = 0
};

#ifdef POLLING_MODE
static struct input_polled_dev *mma8653_idev = NULL;
#else
static struct input_dev *mma8653_idev = NULL;
#endif
static struct device *hwmon_dev;
static struct i2c_client *mma8653_i2c_client;

/* Configure Freefall detection */
static int mma8653_init_freefall_detect(struct i2c_client *client)
{
	volatile int result;
	volatile u32 reg;

	reg = 0xb8;
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_FF_MT_CFG, reg);

	assert(result == 0);

	reg = 0x06;
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_FF_MT_THS, reg);

	assert(result == 0);

	reg = 0x0a;
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_FF_MT_COUNT, reg);
	
	assert(result == 0);

	reg = 0x14;
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_CTRL_REG4, reg);

	assert(result == 0);

	mdelay(MODE_CHANGE_DELAY_MS);

	return result;
}

/***************************************************************
 *
 * Initialization function
 *
 **************************************************************/
static int mma8653_init_client(struct i2c_client *client)
{
	volatile int result;
	volatile u32 reg;

	mma_status.ctl_reg1 = 0x00;
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_CTRL_REG1,
				      mma_status.ctl_reg1);
	assert(result == 0);

	mma_status.mode = MODE_2G;
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_XYZ_DATA_CFG,
				      mma_status.mode);
	assert(result == 0);

	/* Enable portrait/landscape detect */
	reg = 0xC0;
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_PL_CFG,
				      reg);
	assert(result == 0);

	/* Orientation Wake up, ACTIVE low, Push-Pull */
	reg = 0x20;
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_CTRL_REG3,
				      reg);
	assert(result == 0);

	/* Orientation Interrupts enabled  */
	reg = 0x10;
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_CTRL_REG4,
				      reg);
	assert(result == 0);

	/* Orientation to INT1, Transient to INT2 */
	reg = 0x10;
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_CTRL_REG5,
				      reg);
	assert(result == 0);

	mma_status.ctl_reg1 |= 0x01 | DATA_RATE_SELECTION;
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_CTRL_REG1,
				      mma_status.ctl_reg1);
	assert(result == 0);

	mdelay(MODE_CHANGE_DELAY_MS);

	return result;
}

#ifdef POLLING_MODE
/***************************************************************
*
* read sensor data from mma8653
*
***************************************************************/
static int mma8653_read_data(short *x, short *y, short *z)
{
	u8 tmp_data[7];

	if (i2c_smbus_read_i2c_block_data
	    (mma8653_i2c_client, MMA8653_OUT_X_MSB, 7, tmp_data) < 7) {
		dev_err(&mma8653_i2c_client->dev, "i2c block read failed\n");
		return -3;
	}

	*x = ((tmp_data[0] << 8) & 0xff00) | tmp_data[1];
	*y = ((tmp_data[2] << 8) & 0xff00) | tmp_data[3];
	*z = ((tmp_data[4] << 8) & 0xff00) | tmp_data[5];

	*x = (short)(*x) >> 2;
	*y = (short)(*y) >> 2;
	*z = (short)(*z) >> 2;

	if (mma_status.mode == MODE_4G) {
		(*x) = (*x) << 1;
		(*y) = (*y) << 1;
		(*z) = (*z) << 1;
	} else if (mma_status.mode == MODE_8G) {
		(*x) = (*x) << 2;
		(*y) = (*y) << 2;
		(*z) = (*z) << 2;
	}

	return 0;
}

static void report_abs(void)
{
	short x, y, z;
	int result;

	do {
		if (atomic_read(&mma8653_suspended))
			break;

		result =
		    i2c_smbus_read_byte_data(mma8653_i2c_client,
					     MMA8653_STATUS);
	} while (!(result & 0x08));	/* wait for new data */

	if (atomic_read(&mma8653_suspended))
		return;

	if (mma8653_read_data(&x, &y, &z) != 0)
		return;

	input_report_abs(mma8653_idev->input, ABS_X, x);
	input_report_abs(mma8653_idev->input, ABS_Y, y);
	input_report_abs(mma8653_idev->input, ABS_Z, z);
	input_sync(mma8653_idev->input);
}

static void mma8653_dev_poll(struct input_polled_dev *dev)
{
	report_abs();
}

#else
/* The rotation sent has one of the following values :	*/
/* 0 = PORTRAIT_UP																			*/
/* 1 = PORTRAIT_DOWN																		*/
/* 2 = LANDSCAPE_RIGHT																	*/
/* 3 = LANDSCAPE_LEFT																		*/
static void send_rotation_event(u32 reg)
{
	unsigned int* keycodes = (unsigned int *)mma8653_idev->keycode;
	u8 rotation = (reg & 0x06) >> 1;
	if (rotation >= MAX_KEY_CODES || rotation == current_rotation)
		return;
	input_event(mma8653_idev, EV_KEY, keycodes[rotation], 1);
	input_event(mma8653_idev, EV_KEY, keycodes[rotation], 0);
	current_rotation = rotation;
	input_sync(mma8653_idev);
}
#endif

static void do_accelerometer_work(struct work_struct *dummy)
{
    u32 int_reg, reg;

    /* set interrupt value */
    int_reg =
	i2c_smbus_read_byte_data(mma8653_i2c_client,
				 MMA8653_INT_SOURCE); 
    
    if (int_reg & INT_SOURCE_LNDPRT) {
	/* reading PL_STATUS clears interrupt value */
	reg = i2c_smbus_read_byte_data(mma8653_i2c_client,
				 MMA8653_PL_STATUS);
#ifndef POLLING_MODE
	send_rotation_event(reg);
#endif
    }

    enable_irq(irq1);
    enable_irq(irq2);
}

static DECLARE_WORK(accelerometer_work, do_accelerometer_work);

static irqreturn_t accelerometer_irq(int irq, void *devid)
{
	disable_irq_nosync(irq1);
	disable_irq_nosync(irq2);

	schedule_work(&accelerometer_work);

	return IRQ_HANDLED;
}

static void dump_regs(void)
{
	int i;

	printk(KERN_INFO "mma8653q accelerometer dump:\n");

	for (i = 0; i <= 0x31; i++) {
		printk(KERN_INFO "Reg=%d value=0x%x\n", i,
			i2c_smbus_read_byte_data(mma8653_i2c_client, i));
	}
}

static int mma8653_reg_number = 0;

static ssize_t mma8653_reg_store(struct sys_device *dev, struct sysdev_attribute *attr,
					const char *buf, size_t size)
{
	int value = 0;

	if (sscanf(buf, "%x", &value) <= 0) {
		printk(KERN_ERR "Could not store the codec register value\n");
		return -EINVAL;
	}

	mma8653_reg_number = value;
	return size;
}

static ssize_t mma8653_reg_show(struct sys_device *dev, struct sysdev_attribute *attr, char *buf)
{
	char *curr = buf;

	curr += sprintf(curr, "MMA8653 Register Number: %d\n", mma8653_reg_number);
	curr += sprintf(curr, "\n");

	return curr - buf;
}

static SYSDEV_ATTR(mma8653_reg, 0644, mma8653_reg_show, mma8653_reg_store);

static struct sysdev_class mma8653_reg_sysclass = {
	.name	= "mma8653_reg",
};

static struct sys_device device_mma8653_reg = {
	.id	= 0,
	.cls	= &mma8653_reg_sysclass,
};

static ssize_t mma8653_register_show(struct sys_device *dev, struct sysdev_attribute *attr, char *buf)
{
	char *curr = buf;
	int value = 0;
	
	if (mma8653_reg_number >= MMA8653_REG_END) {
		curr += sprintf(curr, "MMA8653 Registers\n");
		curr += sprintf(curr, "\n");
		dump_regs();
	}
	else {
		value = i2c_smbus_read_byte_data(mma8653_i2c_client, mma8653_reg_number);
		curr += sprintf(curr, "MMA8653 Register %d\n", mma8653_reg_number);
		curr += sprintf(curr, "\n");
		curr += sprintf(curr, " Value: 0x%x\n", value);
		curr += sprintf(curr, "\n");
	}

	return curr - buf;
}

static ssize_t mma8653_register_store(struct sys_device *dev, struct sysdev_attribute *attr,
					const char *buf, size_t size)
{
	int value = 0;
	u8 tmp = 0;

	if (mma8653_reg_number >= MMA8653_REG_END) {
		printk(KERN_ERR "No mma8653 register %d\n", mma8653_reg_number);
		return size;
	}

	if (sscanf(buf, "%x", &value) <= 0) {
		printk(KERN_ERR "Error setting the value in the register\n");
		return -EINVAL;
	}

	tmp = (u8)value;

	i2c_smbus_write_byte_data(mma8653_i2c_client, mma8653_reg_number, tmp);
	return size;
}

static SYSDEV_ATTR(mma8653_register, 0644, mma8653_register_show, mma8653_register_store);

static struct sysdev_class mma8653_register_sysclass = {
	.name	= "mma8653_register",
};

static struct sys_device device_mma8653_register = {
	.id	= 0,
	.cls	= &mma8653_register_sysclass,
};


static ssize_t mma8653_rotation_show(struct sys_device *dev, struct sysdev_attribute *attr, char *buf)
{
	char *curr = buf;
	
	curr += sprintf(buf, "%d\n", current_rotation);
	
	return curr - buf;
}

static SYSDEV_ATTR(mma8653_rotation, 0444, mma8653_rotation_show, 0);

static struct sysdev_class mma8653_rotation_sysclass = {
	.name = "mma8653_rotation",
};

static struct sys_device device_mma8653_rotation = {
	.id = 0,
	.cls = &mma8653_rotation_sysclass,
};

static int __devinit mma8653_probe(struct i2c_client *client,
				   const struct i2c_device_id *id)
{
	int result;
	struct input_dev *idev;
	int error = 0;
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	int i;

	irq1 = gpio_accelerometer_int1();
	irq2 = gpio_accelerometer_int2();
  
	dev_info(&client->dev, "mma8653 driver probe enter\n");
#ifdef POLLING_MODE	
	/*input poll device register */
	mma8653_idev = input_allocate_polled_device();
	if (!mma8653_idev) {
		dev_err(&client->dev, "alloc poll device failed!\n");
		result = -ENOMEM;
		return result;
	}
	mma8653_idev->poll = mma8653_dev_poll;
	mma8653_idev->poll_interval = POLL_INTERVAL;
	idev = mma8653_idev->input;
	idev->name = MMA8653_DRV_NAME;
	idev->id.bustype = BUS_I2C;
	idev->dev.parent = &client->dev;
	idev->evbit[0] = BIT_MASK(EV_ABS);

	input_set_abs_params(idev, ABS_X, -8192, 8191, INPUT_FUZZ, INPUT_FLAT);
	input_set_abs_params(idev, ABS_Y, -8192, 8191, INPUT_FUZZ, INPUT_FLAT);
	input_set_abs_params(idev, ABS_Z, -8192, 8191, INPUT_FUZZ, INPUT_FLAT);
	result = input_register_polled_device(mma8653_idev);
	if (result) {
		dev_err(&client->dev, "register poll device failed!\n");
		goto err_free_input_device;
	}
#else
	mma8653_idev = input_allocate_device();
	if (!mma8653_idev) {
		dev_err(&client->dev, "alloc input device failed!\n");
		result = -ENOMEM;
		return result;
	}
	idev = mma8653_idev;
	idev->name = MMA8653_DRV_NAME;
	idev->id.bustype = BUS_I2C;
	idev->dev.parent = &client->dev;
	
	__set_bit(EV_KEY, idev->evbit);
	for (i = 0; i < MAX_KEY_CODES; i++)
		__set_bit(keycodes[i], idev->keybit);
	__clear_bit(KEY_RESERVED, idev->keybit);

	idev->keycode = keycodes;
	idev->keycodesize = sizeof(*keycodes);
	idev->keycodemax = MAX_KEY_CODES;
  
	result = input_register_device(idev);
	if (result) {
		dev_err(&client->dev, "register input device failed!\n");
		goto err_free_input_device;
	}
#endif

	result = request_irq(irq1, accelerometer_irq,
			IRQF_DISABLED|IRQF_TRIGGER_RISING|IRQF_TRIGGER_FALLING, "mma8653_int1", NULL);

	if (result != 0) {
		printk(KERN_ERR "IRQF_DISABLED: Could not get IRQ %d\n", irq1);
		goto err_unreg_input_device;
	}

	result = request_irq(irq2, accelerometer_irq,
			IRQF_DISABLED|IRQF_TRIGGER_RISING|IRQF_TRIGGER_FALLING, "mma8653_int2", NULL);

	if (result != 0) {
		printk(KERN_ERR "IRQF_DISABLED: Could not get IRQ %d\n", irq2);
		free_irq(irq1, NULL);
		goto err_unreg_input_device;
	}

	mma8653_i2c_client = client;

	result = i2c_check_functionality(adapter,
					 I2C_FUNC_SMBUS_BYTE |
					 I2C_FUNC_SMBUS_BYTE_DATA);
	assert(result);

	printk(KERN_INFO "check mma8653 chip ID\n");
	result = i2c_smbus_read_byte_data(client, MMA8653_WHO_AM_I);

	if (MMA8653_ID != result) {
		dev_err(&client->dev,
			"read chip ID 0x%x is not equal to 0x%x!\n", result,
			MMA8653_ID);
		printk(KERN_INFO "read chip ID failed\n");
		result = -EINVAL;
		goto err_detach_client;
	}

	/* Initialize the MMA8653 chip */
	result = mma8653_init_client(client);
	assert(result == 0);

	hwmon_dev = hwmon_device_register(&client->dev);
	assert(!(IS_ERR(hwmon_dev)));

	dev_info(&client->dev, "build time %s %s\n", __DATE__, __TIME__);

	error = sysdev_class_register(&mma8653_reg_sysclass);
	if (!error)
		error = sysdev_register(&device_mma8653_reg);
	if (!error)
		error = sysdev_create_file(&device_mma8653_reg, &attr_mma8653_reg);

	error = sysdev_class_register(&mma8653_register_sysclass);
	if (!error)
		error = sysdev_register(&device_mma8653_register);
	if (!error)
		error = sysdev_create_file(&device_mma8653_register, &attr_mma8653_register);
	error = sysdev_class_register(&mma8653_rotation_sysclass);
	if (!error)
		error = sysdev_register(&device_mma8653_rotation);
	if (!error)
		error = sysdev_create_file(&device_mma8653_rotation, &attr_mma8653_rotation);

	return result;

err_detach_client:
err_unreg_input_device:
#ifdef POLLING_MODE
	input_unregister_polled_device(mma8653_idev);
#else
	input_unregister_device(mma8653_idev);
#endif
err_free_input_device:
#ifdef POLLING_MODE
	input_free_polled_device(mma8653_idev);
#else
	input_free_device(mma8653_idev);
#endif
	return result;
}

static int __devexit mma8653_remove(struct i2c_client *client)
{
	int result;
	mma_status.ctl_reg1 =
	    i2c_smbus_read_byte_data(client, MMA8653_CTRL_REG1);
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_CTRL_REG1,
				      mma_status.ctl_reg1 & 0xFE);
	assert(result == 0);

	sysdev_remove_file(&device_mma8653_reg, &attr_mma8653_reg);
	sysdev_unregister(&device_mma8653_reg);
	sysdev_class_unregister(&mma8653_reg_sysclass);

	sysdev_remove_file(&device_mma8653_register, &attr_mma8653_register);
	sysdev_unregister(&device_mma8653_register);
	sysdev_class_unregister(&mma8653_register_sysclass);

	sysdev_remove_file(&device_mma8653_rotation, &attr_mma8653_rotation);
	sysdev_unregister(&device_mma8653_rotation);
	sysdev_class_unregister(&mma8653_rotation_sysclass);
	
	hwmon_device_unregister(hwmon_dev);

	free_irq(gpio_accelerometer_int1(), NULL);
	free_irq(gpio_accelerometer_int2(), NULL);

	if (mma8653_idev)
	{
#ifdef POLLING_MODE
		input_unregister_polled_device(mma8653_idev);
		input_free_polled_device(mma8653_idev);
#else
		input_unregister_device(mma8653_idev);
		input_free_device(mma8653_idev);
#endif
		mma8653_idev = NULL;
	}
  
	return result;
}

static void mma8653_resume_late(struct work_struct *work)
{
	int result = 0;

	result = request_irq(irq1, accelerometer_irq,
		IRQF_DISABLED|IRQF_TRIGGER_RISING|IRQF_TRIGGER_FALLING, "mma8653_int1", NULL);

	if (result != 0) {
		printk(KERN_ERR "IRQF_DISABLED: Could not get IRQ %d\n", irq1);
		goto out;
	}

	result = request_irq(irq2, accelerometer_irq,
		IRQF_DISABLED|IRQF_TRIGGER_RISING|IRQF_TRIGGER_FALLING, "mma8653_int2", NULL);

	if (result != 0) {
		printk(KERN_ERR "IRQF_DISABLED: Could not get IRQ %d\n", irq2);
		goto out;
	}

	mma8653_init_client(mma8653_i2c_client);

	atomic_set(&mma8653_suspended, 0);
out:
	return;
}

DECLARE_DELAYED_WORK(mma8653_resume_work, mma8653_resume_late);

static int mma8653_suspend(struct i2c_client *client, pm_message_t mesg)
{
	int result;

	free_irq(gpio_accelerometer_int1(), NULL);
	free_irq(gpio_accelerometer_int2(), NULL);

	atomic_set(&mma8653_suspended, 1);

	mma_status.ctl_reg1 =
	    i2c_smbus_read_byte_data(client, MMA8653_CTRL_REG1);
	result =
	    i2c_smbus_write_byte_data(client, MMA8653_CTRL_REG1,
				      mma_status.ctl_reg1 & 0xFE);
	assert(result == 0);
	return result;
}

static int mma8653_resume(struct i2c_client *client)
{
	schedule_delayed_work(&mma8653_resume_work, msecs_to_jiffies(1000));
	return 0;
}

static const struct i2c_device_id mma8653_id[] = {
	{MMA8653_DRV_NAME, 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, mma8653_id);

static struct i2c_driver mma8653_driver = {
	.driver = {
		   .name = MMA8653_DRV_NAME,
		   .owner = THIS_MODULE,
		   },
	.suspend = mma8653_suspend,
	.resume = mma8653_resume,
	.probe = mma8653_probe,
	.remove = __devexit_p(mma8653_remove),
	.id_table = mma8653_id,
};

static int __init mma8653_init(void)
{
	/* register driver */
	int res;

	res = i2c_add_driver(&mma8653_driver);
	if (res < 0) {
		printk(KERN_INFO "add mma8653 i2c driver failed\n");
		return -ENODEV;
	}
	printk(KERN_INFO "add mma8653 i2c driver\n");

	return res;
}

static void __exit mma8653_exit(void)
{
	printk(KERN_INFO "remove mma8653 i2c driver.\n");
	i2c_del_driver(&mma8653_driver);
}

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("MMA8653 3-Axis Orientation/Motion Detection Sensor driver");
MODULE_LICENSE("GPL");

module_init(mma8653_init);
module_exit(mma8653_exit);

