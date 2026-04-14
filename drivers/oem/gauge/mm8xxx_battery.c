// SPDX-License-Identifier: GPL-2.0
/*
 * MM8xxx battery driver
 */
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <asm/unaligned.h>
#include <linux/iio/consumer.h>
#include <linux/kernel.h>
#include <linux/reboot.h>

#define MM8XXX_MANUFACTURER	"MITSUMI ELECTRIC"
#define BATTERY_RESISTANCE	25
#define VBAT_OVERLOW_VOLTAGE	3400000
#define BATTERY_FULL_CAPACITY	8600000
#define BATTERY_PRESENT_VOLT_MV	2000

#define MM8XXX_MSLEEP(i) usleep_range((i)*1000, (i)*1000+500)

static DEFINE_IDR(battery_id);
static DEFINE_MUTEX(battery_mutex);
static DEFINE_MUTEX(bat_i2c_mutex);

enum mm8xxx_chip {
	MM8118G01 = 1,
	MM8118W02,
	MM8013C10,
};

struct mm8xxx_device_info;

struct mm8xxx_access_methods {
	int (*read)(struct mm8xxx_device_info *di, u8 cmd);
	int (*write)(struct mm8xxx_device_info *di, u8 cmd, int value);
};

struct mm8xxx_state_cache {
	int temperature;
	int avg_time_to_empty;
	int full_charge_capacity;
	int cycle_count;
	int soc;
	int flags;
	int health;
	int elapsed_months;
	int elapsed_days;
	int elapsed_hours;
};

struct mm8xxx_device_info {
	struct device *dev;
	int id;
	enum mm8xxx_chip chip;
	const char *name;
	struct mm8xxx_access_methods bus;
	struct mm8xxx_state_cache cache;
	int charge_design_full;
	unsigned long last_update;
	struct delayed_work work;
	struct delayed_work check_vbat_overlow_work;
	struct power_supply *psy;
	struct list_head list;
	struct mutex lock;
	u8 *cmds;
	struct iio_channel *iio_vbat;
};

static void mm8xxx_battery_update(struct mm8xxx_device_info *di);
static int mm8xxx_battery_setup(struct mm8xxx_device_info *di);
static void mm8xxx_battery_teardown(struct mm8xxx_device_info *di);

static irqreturn_t mm8xxx_battery_irq_handler_thread(int irq, void *data)
{
	struct mm8xxx_device_info *di = data;
	mm8xxx_battery_update(di);
	return IRQ_HANDLED;
}

static int mm8xxx_battery_read(struct mm8xxx_device_info *di, u8 cmd)
{
	struct i2c_client *client = to_i2c_client(di->dev);
	struct i2c_msg msg[2];
	u8 data[2];
	int ret, retry = 3;

	if (!client->adapter)
		return -ENODEV;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &cmd;
	msg[0].len = sizeof(cmd);
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = data;
	msg[1].len = 2;

	do {
		mutex_lock(&bat_i2c_mutex);
		ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
		mutex_unlock(&bat_i2c_mutex);
		if (ret < 0)
			MM8XXX_MSLEEP(5);
		--retry;
	} while (ret < 0 && retry > 0);

	if (ret < 0)
		return ret;

	ret = get_unaligned_le16(data);

	return ret;
}

static int mm8xxx_battery_write(struct mm8xxx_device_info *di, u8 cmd,
				int value)
{
	struct i2c_client *client = to_i2c_client(di->dev);
	struct i2c_msg msg;
	u8 data[4];
	int ret, retry = 3;

	if (!client->adapter)
		return -ENODEV;

	data[0] = cmd;
	put_unaligned_le16(value, &data[1]);
	msg.len = 3;

	msg.buf = data;
	msg.addr = client->addr;
	msg.flags = 0;

	do {
		mutex_lock(&bat_i2c_mutex);
		ret = i2c_transfer(client->adapter, &msg, 1);
		mutex_unlock(&bat_i2c_mutex);
		if (ret < 0)
			MM8XXX_MSLEEP(5);
		--retry;
	} while (ret < 0 && retry > 0);

	if (ret < 0)
		return ret;
	else if (ret != 1)
		return -EINVAL;

	return 0;
}

#if IS_ENABLED(CONFIG_FACTORY_BUILD)
static enum power_supply_property no_i2c_bat_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
};

static int no_i2c_bat_get_property(struct power_supply *psy, enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct mm8xxx_device_info *di = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (di->iio_vbat) {
			iio_read_channel_processed(di->iio_vbat, &(val->intval));
			val->intval *= 1000;
		} else
			val->intval = 3800 * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = 500 * 1000;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = 50;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = 250;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct power_supply_desc no_i2c_bat_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = no_i2c_bat_props,
	.num_properties = ARRAY_SIZE(no_i2c_bat_props),
	.get_property = no_i2c_bat_get_property,
};

static int no_i2c_bat_psy_register(struct mm8xxx_device_info *di)
{
	struct power_supply_config cfg = {
		.of_node = di->dev->of_node,
		.drv_data = di,
	};

	di->psy = power_supply_register_no_ws(di->dev, &no_i2c_bat_desc, &cfg);
	if (IS_ERR(di->psy)) {
		dev_err(di->dev, "failed to register no i2c battery\n");
		return PTR_ERR(di->psy);
	}

	return 0;
}

static int no_i2c_bat_init(struct mm8xxx_device_info *di)
{
	if (no_i2c_bat_psy_register(di))
		return -1;

	return 0;
}
#endif

static int mm8xxx_battery_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct mm8xxx_device_info *di;
	int ret;
	char *name;
	int num;

	mutex_lock(&battery_mutex);
	num = idr_alloc(&battery_id, client, 0, 0, GFP_KERNEL);
	mutex_unlock(&battery_mutex);
	if (num < 0)
		return num;

	name = devm_kasprintf(&client->dev, GFP_KERNEL, "%s-%d", id->name, num);
	if (!name)
		goto mem_err;

	di = devm_kzalloc(&client->dev, sizeof(*di), GFP_KERNEL);
	if (!di)
		goto mem_err;

	di->id = num;
	di->dev = &client->dev;
	di->chip = id->driver_data;
	di->name = name;

	di->bus.read = mm8xxx_battery_read;
	di->bus.write = mm8xxx_battery_write;

	di->iio_vbat = devm_iio_channel_get(di->dev, "pmic_vbat");
	if (IS_ERR_OR_NULL(di->iio_vbat))
		dev_err(di->dev, "%s: get iio channel pmic_vbat failed\n", __func__);

	ret = mm8xxx_battery_setup(di);
	if (ret) {
#if IS_ENABLED(CONFIG_FACTORY_BUILD)
		if (no_i2c_bat_init(di))
			goto failed;
		else
			return 0;
#else
		goto failed;
#endif
	}

	schedule_delayed_work(&di->work, 6 * HZ);
	schedule_delayed_work(&di->check_vbat_overlow_work, 10 * HZ);
	i2c_set_clientdata(client, di);

	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
				NULL, mm8xxx_battery_irq_handler_thread,
				IRQF_ONESHOT,
				di->name, di);
		if (ret) {
			dev_err(&client->dev,
				"Unable to register IRQ %d error %d\n",
				client->irq, ret);

			return ret;
		}
	}

	return 0;

mem_err:
	ret = -ENOMEM;

failed:
	mutex_lock(&battery_mutex);
	idr_remove(&battery_id, num);
	mutex_unlock(&battery_mutex);

	return -EPROBE_DEFER;
}

static int mm8xxx_battery_remove(struct i2c_client *client)
{
	struct mm8xxx_device_info *di = i2c_get_clientdata(client);

	mm8xxx_battery_teardown(di);

	mutex_lock(&battery_mutex);
	idr_remove(&battery_id, di->id);
	mutex_unlock(&battery_mutex);

	return 0;
}

static const struct i2c_device_id mm8xxx_battery_id_table[] = {
	{ "mm8118g01", MM8118G01 },
	{ "mm8118w02", MM8118W02 },
	{ "mm8013c10", MM8013C10 },
	{},
};
MODULE_DEVICE_TABLE(i2c, mm8xxx_battery_id_table);

#ifdef CONFIG_OF
static const struct of_device_id mm8xxx_battery_of_match_table[] = {
	{ .compatible = "mitsumi,mm8118g01" },
	{ .compatible = "mitsumi,mm8118w02" },
	{ .compatible = "mitsumi,mm8013c10" },
	{},
};
MODULE_DEVICE_TABLE(of, mm8xxx_battery_of_match_table);
#endif

static struct i2c_driver mm8xxx_battery_driver = {
	.driver = {
		.name = "mm8xxx-battery",
		.of_match_table = of_match_ptr(mm8xxx_battery_of_match_table),
	},
	.probe = mm8xxx_battery_probe,
	.remove = mm8xxx_battery_remove,
	.id_table = mm8xxx_battery_id_table,
};
module_i2c_driver(mm8xxx_battery_driver);

/* MM8XXX Flags */
#define MM8XXX_FLAG_DSG		BIT(0)
#define MM8XXX_FLAG_SOCF	BIT(1)
#define MM8XXX_FLAG_SOC1	BIT(2)
#define MM8XXX_FLAG_UT		BIT(3)
#define MM8XXX_FLAG_OT		BIT(4)
#define MM8XXX_FLAG_ODC		BIT(5)
#define MM8XXX_FLAG_OCC		BIT(6)
#define MM8XXX_FLAG_OCVTAKEN	BIT(7)
#define MM8XXX_FLAG_CHG		BIT(8)
#define MM8XXX_FLAG_FC		BIT(9)
#define MM8XXX_FLAG_CHGINH	BIT(11)
#define MM8XXX_FLAG_BATLOW	BIT(12)
#define MM8XXX_FLAG_BATHI	BIT(13)
#define MM8XXX_FLAG_OTD		BAT(14)
#define MM8XXX_FLAG_OTC		BAT(15)

#define INVALID_COMMAND		0xff

enum mm8xxx_cmd_index {
	MM8XXX_CMD_CONTROL = 0,
	MM8XXX_CMD_TEMPERATURE,
	MM8XXX_CMD_VOLTAGE,
	MM8XXX_CMD_FLAGS,
	MM8XXX_CMD_REMAININGCAPACITY,
	MM8XXX_CMD_FULLCHARGECAPACITY,
	MM8XXX_CMD_AVERAGECURRENT,
	MM8XXX_CMD_AVERAGETIMETOEMPTY,
	MM8XXX_CMD_CYCLECOUNT,
	MM8XXX_CMD_STATEOFCHARGE,
	MM8XXX_CMD_CHARGEVOLTAGE,
	MM8XXX_CMD_DESIGNCAPACITY,
	MM8XXX_CMD_CURRENT,
	MM8XXX_CMD_ELAPSEDTIMEM,
	MM8XXX_CMD_ELAPSEDTIMED,
	MM8XXX_CMD_ELAPSEDTIMEH,

	MM8XXX_CMD_EOI,
};

static u8
	mm8118g01_cmds[MM8XXX_CMD_EOI] = {
		[MM8XXX_CMD_CONTROL] 		= 0x00,
		[MM8XXX_CMD_TEMPERATURE] 	= 0x06,
		[MM8XXX_CMD_VOLTAGE] 		= 0x08,
		[MM8XXX_CMD_FLAGS] 		= 0x0A,
		[MM8XXX_CMD_REMAININGCAPACITY]	= 0x10,
		[MM8XXX_CMD_FULLCHARGECAPACITY]	= 0x12,
		[MM8XXX_CMD_AVERAGECURRENT]	= 0x14,
		[MM8XXX_CMD_AVERAGETIMETOEMPTY]	= 0x16,
		[MM8XXX_CMD_CYCLECOUNT]		= 0x2A,
		[MM8XXX_CMD_STATEOFCHARGE]	= 0x2C,
		[MM8XXX_CMD_CHARGEVOLTAGE]	= INVALID_COMMAND,
		[MM8XXX_CMD_DESIGNCAPACITY]	= 0x3C,
		[MM8XXX_CMD_ELAPSEDTIMEM]	= INVALID_COMMAND,
		[MM8XXX_CMD_ELAPSEDTIMED]	= INVALID_COMMAND,
		[MM8XXX_CMD_ELAPSEDTIMEH]	= INVALID_COMMAND,
	},
	mm8118w02_cmds[MM8XXX_CMD_EOI] = {
		[MM8XXX_CMD_CONTROL] 		= 0x00,
		[MM8XXX_CMD_TEMPERATURE] 	= 0x06,
		[MM8XXX_CMD_VOLTAGE] 		= 0x08,
		[MM8XXX_CMD_FLAGS] 		= 0x0A,
		[MM8XXX_CMD_REMAININGCAPACITY]	= 0x10,
		[MM8XXX_CMD_FULLCHARGECAPACITY]	= 0x12,
		[MM8XXX_CMD_AVERAGECURRENT]	= 0x14,
		[MM8XXX_CMD_AVERAGETIMETOEMPTY]	= 0x16,
		[MM8XXX_CMD_CYCLECOUNT]		= 0x2A,
		[MM8XXX_CMD_STATEOFCHARGE]	= 0x2C,
		[MM8XXX_CMD_CHARGEVOLTAGE]	= INVALID_COMMAND,
		[MM8XXX_CMD_DESIGNCAPACITY]	= 0x3C,
		[MM8XXX_CMD_ELAPSEDTIMEM]	= INVALID_COMMAND,
		[MM8XXX_CMD_ELAPSEDTIMED]	= INVALID_COMMAND,
		[MM8XXX_CMD_ELAPSEDTIMEH]	= INVALID_COMMAND,
	},
	mm8013c10_cmds[MM8XXX_CMD_EOI] = {
		[MM8XXX_CMD_CONTROL] 		= 0x00,
		[MM8XXX_CMD_TEMPERATURE] 	= 0x06,
		[MM8XXX_CMD_VOLTAGE] 		= 0x08,
		[MM8XXX_CMD_FLAGS] 		= 0x0A,
		[MM8XXX_CMD_REMAININGCAPACITY]	= 0x10,
		[MM8XXX_CMD_FULLCHARGECAPACITY]	= 0x12,
		[MM8XXX_CMD_AVERAGECURRENT]	= 0x14,
		[MM8XXX_CMD_AVERAGETIMETOEMPTY]	= 0x16,
		[MM8XXX_CMD_CYCLECOUNT]		= 0x2A,
		[MM8XXX_CMD_STATEOFCHARGE]	= 0x2C,
		[MM8XXX_CMD_CHARGEVOLTAGE]	= 0x30,
		[MM8XXX_CMD_DESIGNCAPACITY]	= 0x3C,
		[MM8XXX_CMD_CURRENT]		= 0x72,
		[MM8XXX_CMD_ELAPSEDTIMEM]	= 0x74,
		[MM8XXX_CMD_ELAPSEDTIMED]	= 0x76,
		[MM8XXX_CMD_ELAPSEDTIMEH]	= 0x78,
	};

static enum power_supply_property mm8118g01_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};
#define mm8118w02_props mm8118g01_props

static enum power_supply_property mm8013c10_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
//	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
};

#define MM8XXX_DATA(ref) { 			\
	.cmds = ref##_cmds,			\
	.props = ref##_props,			\
	.props_size = ARRAY_SIZE(ref##_props) }

static struct {
	u8 *cmds;
	enum power_supply_property *props;
	size_t props_size;
} mm8xxx_chip_data[] = {
	[MM8118G01]	= MM8XXX_DATA(mm8118g01),
	[MM8118W02]	= MM8XXX_DATA(mm8118w02),
	[MM8013C10]	= MM8XXX_DATA(mm8013c10),
};

static DEFINE_MUTEX(mm8xxx_list_lock);
static LIST_HEAD(mm8xxx_battery_devices);

static int poll_interval_param_set(const char *val,
				   const struct kernel_param *kp)
{
	struct mm8xxx_device_info *di;
	unsigned int prev_val = *(unsigned int *) kp->arg;
	int ret;

	ret = param_set_uint(val, kp);
	if ((ret < 0) || (prev_val == *(unsigned int *) kp->arg))
		return ret;

	mutex_lock(&mm8xxx_list_lock);
	list_for_each_entry(di, &mm8xxx_battery_devices, list) {
		cancel_delayed_work_sync(&di->work);
		schedule_delayed_work(&di->work, 0);
	}
	mutex_unlock(&mm8xxx_list_lock);

	return ret;
}

static const struct kernel_param_ops param_ops_poll_interval = {
	.get = param_get_uint,
	.set = poll_interval_param_set,
};

static unsigned int poll_interval = 6;
module_param_cb(poll_interval, &param_ops_poll_interval, &poll_interval, 0644);
MODULE_PARM_DESC(poll_interval,
		"battery poll interval in seconds - 0 disables polling");

static inline int mm8xxx_read(struct mm8xxx_device_info *di, int cmd_index)
{
	int ret;

	if ((!di) || (di->cmds[cmd_index] == INVALID_COMMAND))
		return -EINVAL;

	if (!di->bus.read)
		return -EPERM;

	ret = di->bus.read(di, di->cmds[cmd_index]);
	if (ret < 0)
		dev_err(di->dev, "failed to read command 0x%02x (index %d)\n",
			di->cmds[cmd_index], cmd_index);

	return ret;
}

static inline int mm8xxx_write(struct mm8xxx_device_info *di, int cmd_index,
			       u16 value)
{
	int ret;

	if ((!di) || (di->cmds[cmd_index] == INVALID_COMMAND))
		return -EINVAL;

	if (!di->bus.write)
		return -EPERM;

	ret = di->bus.write(di, di->cmds[cmd_index], value);
	if (ret < 0)
		dev_err(di->dev, "failed to write command 0x%02x (index %d)\n",
			di->cmds[cmd_index], cmd_index);

	return ret;
}

/*
 * Return the battery State-of-Charge
 * Or < 0 if somthing fails.
 */
static int mm8xxx_battery_read_stateofcharge(struct mm8xxx_device_info *di)
{
	int soc;

	soc = mm8xxx_read(di, MM8XXX_CMD_STATEOFCHARGE);

	if (soc < 0)
		dev_err(di->dev, "error reading State-of-Charge\n");

	return soc;
}

/*
 * Return the battery Remaining Capacity in µAh
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_read_remainingcapacity(struct mm8xxx_device_info *di)
{
	int rc;

	rc = mm8xxx_read(di, MM8XXX_CMD_REMAININGCAPACITY);
	if (rc < 0)
		dev_err(di->dev, "error reading Remaining Capacity\n");

	rc *= 1000;

	return rc;
}

/*
 * Return the battery Full Charge Capacity in µAh
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_read_fullchargecapacity(struct mm8xxx_device_info *di)
{
	int fcc;

	fcc = mm8xxx_read(di, MM8XXX_CMD_FULLCHARGECAPACITY);
	if (fcc < 0)
		dev_err(di->dev, "error reading Full Charge Capacity\n");

	fcc *= 1000;

	return fcc;
}

/*
 * Return the battery Design Capacity in µAh
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_read_designcapacity(struct mm8xxx_device_info *di)
{
	int dc;

	dc = mm8xxx_read(di, MM8XXX_CMD_DESIGNCAPACITY);
	if (dc < 0)
		dev_err(di->dev, "error reading Design Capacity\n");

	dc *= 1000;

	return dc;
}

/*
 * Return the battery temperature in tenths of degree Kelvin
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_read_temperature(struct mm8xxx_device_info *di)
{
	int temp;

	temp = mm8xxx_read(di, MM8XXX_CMD_TEMPERATURE);
	if (temp < 0)
		dev_err(di->dev, "error reading temperature\n");

	return temp;
}

/*
 * Return the battery Cycle Count
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_read_cyclecount(struct mm8xxx_device_info *di)
{
	int cc;

	cc = mm8xxx_read(di, MM8XXX_CMD_CYCLECOUNT);
	if (cc < 0)
		dev_err(di->dev, "error reading cycle count\n");

	return cc;
}

/*
 * Return the battery usable time
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_read_averagetimetoempty(struct mm8xxx_device_info *di)
{
	int atte;

	atte = mm8xxx_read(di, MM8XXX_CMD_AVERAGETIMETOEMPTY);
	if (atte < 0) {
		dev_err(di->dev, "error reading average time to empty\n");
		return atte;
	}

	if (atte == 65535)
		return -ENODATA;

	return atte * 60;
}

/*
 * Returns true if a battery over temperature condition is detected.
 */
static inline bool mm8xxx_battery_overtemperature(u16 flags)
{
	return flags & MM8XXX_FLAG_OT;
}

/*
 * Returns true if a battery under temperature condition is detected.
 */
static inline bool mm8xxx_battery_undertemperature(u16 flags)
{
	return flags & MM8XXX_FLAG_UT;
}

/*
 * Returns true if a low state of charge condition is detected.
 */
static inline bool mm8xxx_battery_dead(u16 flags)
{
	return flags & MM8XXX_FLAG_SOCF;
}

/*
 * Returns POWER_SUPPLY_HEALTH_(OVERHEAT/COLD/DEAD) if a battery is in abnormal
 * condition, POWER_SUPPLY_HEALTH_GOOD otherwise.
 */
static int mm8xxx_battery_read_health(struct mm8xxx_device_info *di)
{
	if (unlikely(mm8xxx_battery_overtemperature(di->cache.flags)))
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	if (unlikely(mm8xxx_battery_undertemperature(di->cache.flags)))
		return POWER_SUPPLY_HEALTH_COLD;

	return POWER_SUPPLY_HEALTH_GOOD;
}

/*
 * Return the battery Charge Voltage
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_read_chargevoltage(struct mm8xxx_device_info *di)
{
	int cv;

	cv = mm8xxx_read(di, MM8XXX_CMD_CHARGEVOLTAGE);
	if (cv < 0)
		dev_err(di->dev, "error reading charge voltage\n");

	return cv;
}

/*
 * Write the battery Charge Voltage.
 */
static int mm8xxx_battery_write_chargevoltage(struct mm8xxx_device_info *di,
					      u16 cv)
{
	int ret;

	ret = mm8xxx_write(di, MM8XXX_CMD_CHARGEVOLTAGE, cv);
	if (ret < 0) {
		dev_err(di->dev, "error writing charge voltage\n");
		return ret;
	}

	return 0;
}

/*
 * Return the battery usage time in month
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_read_elapsedmonths(struct mm8xxx_device_info *di)
{
	int elapsed;

	elapsed = mm8xxx_read(di, MM8XXX_CMD_ELAPSEDTIMEM);
	if (elapsed < 0)
		dev_err(di->dev, "error reading elapsed months\n");

	return elapsed;
}

/*
 * Return the battery usage time in day
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_read_elapseddays(struct mm8xxx_device_info *di)
{
	int elapsed;

	elapsed = mm8xxx_read(di, MM8XXX_CMD_ELAPSEDTIMED);
	if (elapsed < 0)
		dev_err(di->dev, "error reading elapsed days\n");

	return elapsed;
}

/*
 * Return the battery usage time in hour
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_read_elapsedhours(struct mm8xxx_device_info *di)
{
	int elapsed;

	elapsed = mm8xxx_read(di, MM8XXX_CMD_ELAPSEDTIMEH);
	if (elapsed < 0)
		dev_err(di->dev, "error reading elapsed hours\n");

	return elapsed;
}

static void mm8xxx_battery_update(struct mm8xxx_device_info *di)
{
	struct mm8xxx_state_cache cache = {0, };
#ifdef CONFIG_BATTERY_MM8XXX_DYNAMIC_CHARGE_VOLTAGE
	int cv;
	int req = 0;
#endif
	cache.flags = mm8xxx_read(di, MM8XXX_CMD_FLAGS);
	if ((cache.flags & 0xFFFF) == 0xFFFF)
		cache.flags = -1;
	if (cache.flags < 0)
		goto out;

	cache.temperature = mm8xxx_battery_read_temperature(di);
	cache.avg_time_to_empty = mm8xxx_battery_read_averagetimetoempty(di);
	cache.soc = mm8xxx_battery_read_stateofcharge(di);
	cache.full_charge_capacity = mm8xxx_battery_read_fullchargecapacity(di);
	di->cache.flags = cache.flags;
	cache.health = mm8xxx_battery_read_health(di);
	cache.cycle_count = mm8xxx_battery_read_cyclecount(di);

	if (di->charge_design_full <= 0)
		di->charge_design_full = mm8xxx_battery_read_designcapacity(di);

	if (di->cmds[MM8XXX_CMD_ELAPSEDTIMEM] != INVALID_COMMAND)
		cache.elapsed_months = mm8xxx_battery_read_elapsedmonths(di);
	if (di->cmds[MM8XXX_CMD_ELAPSEDTIMED] != INVALID_COMMAND)
		cache.elapsed_days = mm8xxx_battery_read_elapseddays(di);
	if (di->cmds[MM8XXX_CMD_ELAPSEDTIMEH] != INVALID_COMMAND)
		cache.elapsed_hours = mm8xxx_battery_read_elapsedhours(di);

#ifdef CONFIG_BATTERY_MM8XXX_DYNAMIC_CHARGE_VOLTAGE
	if (di->chip != MM8013C10)
		goto out;

	cv = mm8xxx_battery_read_chargevoltage(di);
	if (cv < 0)
		goto out;

	/*
	 * TODO: Change cycle-counts and voltages according to each devices.
	 */
	if ((cache.cycle_count < 250) && (cv != 4400))
		req = 4400;
	else if ((cache.cycle_count < 500) && (cv != 4350))
		req = 4350;
	else if ((cache.cycle_count < 800) && (cv != 4300))
		req = 4300;
	else if (cv != 4250)
		req = 4250;

	if (req == 0)
		goto out;

	mm8xxx_battery_write_chargevoltage(di, cv);
#endif

out:
//	if ((di->cache.soc != cache.soc) ||
//	    (di->cache.flags != cache.flags))
	printk(KERN_EMERG "%s: ------------\n", __func__);
		power_supply_changed(di->psy);

	if (memcmp(&di->cache, &cache, sizeof(cache)) != 0)
		di->cache = cache;

	di->last_update = jiffies;
}

static void mm8xxx_battery_poll(struct work_struct *work)
{
	struct mm8xxx_device_info *di =
		container_of(work, struct mm8xxx_device_info, work.work);

	mm8xxx_battery_update(di);

	if (poll_interval > 0)
		schedule_delayed_work(&di->work, poll_interval * HZ);
}

static void mm8xxx_check_vbat_overlow_poll(struct work_struct *work)
{
	static int count = 0;
	int ret;
	union power_supply_propval vbat, ibat;
	struct mm8xxx_device_info *di =
		container_of(work, struct mm8xxx_device_info, check_vbat_overlow_work.work);

	ret = power_supply_get_property(di->psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &vbat);
	if (ret < 0) {
		dev_err(di->dev, "vbat overlow power_supply_get_property vbat fail\n");
		return;
	}

	ret = power_supply_get_property(di->psy, POWER_SUPPLY_PROP_CURRENT_NOW, &ibat);
	if (ret < 0) {
		dev_err(di->dev, "vbat overlow power_supply_get_property ibat fail\n");
		return;
	}

	if (ibat.intval <= 0 && (vbat.intval - (ibat.intval * BATTERY_RESISTANCE / 1000)) < VBAT_OVERLOW_VOLTAGE) {
		count++;
		if (count == 3) {
			dev_err(di->dev, "vbat overlow power off\n");
			kernel_power_off();
		}
	} else {
		count = 0;
	}

	schedule_delayed_work(&di->check_vbat_overlow_work, HZ);
}

/*
 * Gets the battery average current in µA and returns 0
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_average_current(struct mm8xxx_device_info *di,
				  union power_supply_propval *val)
{
	int curr;
	curr = mm8xxx_read(di, MM8XXX_CMD_AVERAGECURRENT);
	if (curr < 0) {
		dev_err(di->dev, "error reading average current\n");
		return curr;
	}

	curr = (int)((s16)curr) * 1000;
	val->intval = curr;

	return 0;
}

static int mm8xxx_battery_current(struct mm8xxx_device_info *di,
				  union power_supply_propval *val)
{
	int curr;
	curr = mm8xxx_read(di, MM8XXX_CMD_CURRENT);
	if (curr < 0) {
		dev_err(di->dev, "error reading current\n");
		return curr;
	}

	curr = (int)((s16)curr) * 1000;
	val->intval = curr;

	return 0;
}

static int mm8xxx_battery_status(struct mm8xxx_device_info *di,
				 union power_supply_propval *val)
{
	static struct power_supply *chg_main = NULL;

	if (!chg_main)
		chg_main = power_supply_get_by_name("mtk-master-charger");
	if (chg_main)
		power_supply_get_property(chg_main, POWER_SUPPLY_PROP_STATUS, val);
	else
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;

	return 0;
}

static int mm8xxx_battery_capacity_level(struct mm8xxx_device_info *di,
					 union power_supply_propval *val)
{
	int level;

	if (di->cache.flags & MM8XXX_FLAG_FC)
		level = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
	else
		level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;

	val->intval = level;

	return 0;
}

/*
 * Gets the battery Voltage in µV and returns 0
 * Or < 0 if something fails.
 */
static int mm8xxx_battery_voltage(struct mm8xxx_device_info *di,
				  union power_supply_propval *val)
{
	int voltage;

	voltage = mm8xxx_read(di, MM8XXX_CMD_VOLTAGE);
	if (voltage < 0) {
		dev_err(di->dev, "error reading voltage\n");
		return voltage;
	}

	val->intval = voltage * 1000;

	return 0;
}

static int mm8xxx_simple_value(int value, union power_supply_propval *val)
{
	if (value < 0)
		return value;

	val->intval = value;

	return 0;
}

static void mm8xxx_is_battery_present(struct mm8xxx_device_info *di, union power_supply_propval *val)
{
	int ret = 0, bat_volt;

	ret = mm8xxx_read(di, MM8XXX_CMD_FLAGS);
	if (ret < 0) {
		dev_err(di->dev, "%s: read flags ret = %d\n", __func__, ret);
		if (!di->iio_vbat) {
			dev_err(di->dev, "%s: iio_vbat is null\n", __func__);
			val->intval = 0;
			return;
		} else {
			ret = iio_read_channel_processed(di->iio_vbat, &bat_volt);
			dev_err(di->dev, "%s: read vbat adc ret = %d\n", __func__, ret);
			if (ret < 0) {
				val->intval = 0;
				return;
			} else {
				dev_err(di->dev, "%s: bat_volt = %d\n", __func__, bat_volt);
				if (bat_volt < BATTERY_PRESENT_VOLT_MV) {
					val->intval = 0;
					return;
				}
			}
		}
	}
	val->intval = 1;
}

static int mm8xxx_battery_get_capacity(struct mm8xxx_device_info *di)
{
	int soc;

	soc = mm8xxx_battery_read_stateofcharge(di);
	if (soc < 0) {
		dev_err(di->dev, "%s: read soc failed, ret = %d\n", __func__, soc);
		soc = 50;
	} else if (soc == 0 && (mm8xxx_read(di, MM8XXX_CMD_VOLTAGE) == 0)) {
		dev_err(di->dev, "%s: battery gauge chip abnormal, soc and volt is 0!\n", __func__);
		soc = 50;
	}
	return soc;
}

static int mm8xxx_battery_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	int ret = 0;
	struct mm8xxx_device_info *di = power_supply_get_drvdata(psy);

	if ((psp != POWER_SUPPLY_PROP_PRESENT) && (di->cache.flags < 0))
		return -ENODEV;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = mm8xxx_battery_status(di, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = mm8xxx_battery_voltage(di, val);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		mm8xxx_is_battery_present(di, val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = mm8xxx_battery_current(di, val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		ret = mm8xxx_battery_average_current(di, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = mm8xxx_battery_get_capacity(di);
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		ret = mm8xxx_battery_capacity_level(di, val);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = mm8xxx_simple_value(mm8xxx_battery_read_temperature(di), val);
		if (ret == 0)
			val->intval += -2731;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		ret = mm8xxx_simple_value(di->cache.avg_time_to_empty, val);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		ret = mm8xxx_simple_value(
			mm8xxx_battery_read_remainingcapacity(di), val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = mm8xxx_simple_value(BATTERY_FULL_CAPACITY, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = mm8xxx_simple_value(di->charge_design_full, val);
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = mm8xxx_simple_value(di->cache.cycle_count, val);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = mm8xxx_simple_value(di->cache.health, val);
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = MM8XXX_MANUFACTURER;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = mm8xxx_battery_read_chargevoltage(di);
		break;

	/*
	 * TODO: Implement appropriate POWER_SUPPLY_PROP property and read
	 * elapsed time.
	 *
	 *   di->cache.elapsed_months
	 *   di->cache.elapsed_days
	 *   di->cache.elapsed_hours
	 */

	default:
		return -EINVAL;
	}

	return ret;
}

static int mm8xxx_battery_set_property(struct power_supply *psy, enum power_supply_property psp,
					const union power_supply_propval *val)
{
	struct mm8xxx_device_info *di = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		dev_err(di->dev, "%s: write voltage %d\n", __func__, val->intval);
		mm8xxx_battery_write_chargevoltage(di, val->intval);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

static int mm8xxx_battery_property_is_writeable(struct power_supply *psy, enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return 1;
	default:
		return 0;
	}
}

static void mm8xxx_external_power_changed(struct power_supply *psy)
{
	struct mm8xxx_device_info *di = power_supply_get_drvdata(psy);

	cancel_delayed_work_sync(&di->work);
	schedule_delayed_work(&di->work, 0);
}

static ssize_t remaining_cap_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct mm8xxx_device_info *di = power_supply_get_drvdata(psy);
	dev_err(di->dev, "%s\n", __func__);
	return sprintf(buf, "%u\n", mm8xxx_battery_read_fullchargecapacity(di) * 100 / BATTERY_FULL_CAPACITY);
}

static DEVICE_ATTR_RO(remaining_cap);

static int mm8xxx_battery_setup(struct mm8xxx_device_info *di)
{
	struct power_supply_desc *ps_desc;
	struct power_supply_config ps_cfg = {
		.of_node = di->dev->of_node,
		.drv_data = di,
	};
	int val;

	INIT_DELAYED_WORK(&di->work, mm8xxx_battery_poll);
	INIT_DELAYED_WORK(&di->check_vbat_overlow_work, mm8xxx_check_vbat_overlow_poll);
	mutex_init(&di->lock);

	di->cmds = mm8xxx_chip_data[di->chip].cmds;

	mm8xxx_write(di, MM8XXX_CMD_CONTROL, 0x0003);
	val = mm8xxx_read(di, MM8XXX_CMD_CONTROL);
	if (val < 0) {
		dev_err(di->dev, "failed to communicate with the battery\n");
		return val;
	}

	ps_desc = devm_kzalloc(di->dev, sizeof(*ps_desc), GFP_KERNEL);
	if (!ps_desc)
		return -ENOMEM;

	ps_desc->name = "battery";
	ps_desc->type = POWER_SUPPLY_TYPE_BATTERY;
	ps_desc->properties = mm8xxx_chip_data[di->chip].props;
	ps_desc->num_properties = mm8xxx_chip_data[di->chip].props_size;
	ps_desc->get_property = mm8xxx_battery_get_property;
	ps_desc->set_property = mm8xxx_battery_set_property,
	ps_desc->property_is_writeable = mm8xxx_battery_property_is_writeable,
	ps_desc->external_power_changed = mm8xxx_external_power_changed;

	di->psy = power_supply_register_no_ws(di->dev, ps_desc, &ps_cfg);
	if (IS_ERR(di->psy)) {
		dev_err(di->dev, "failed to register battery\n");
		return PTR_ERR(di->psy);
	}
	device_create_file(&(di->psy->dev), &dev_attr_remaining_cap);

	dev_dbg(di->dev, "MM8118 HW_VERSION = 0x%04X\n", val);

	mm8xxx_battery_update(di);

	mutex_lock(&mm8xxx_list_lock);
	list_add(&di->list, &mm8xxx_battery_devices);
	mutex_unlock(&mm8xxx_list_lock);

	return 0;
}

static void mm8xxx_battery_teardown(struct mm8xxx_device_info *di)
{
	poll_interval = 0;

	cancel_delayed_work_sync(&di->work);
	power_supply_unregister(di->psy);

	mutex_lock(&mm8xxx_list_lock);
	list_del(&di->list);
	mutex_unlock(&mm8xxx_list_lock);

	mutex_destroy(&di->lock);
}

MODULE_AUTHOR("Takayuki Sugaya");
MODULE_AUTHOR("Yasuhiro Kinoshita");
MODULE_DESCRIPTION("MM8xxx battery monitor");
MODULE_LICENSE("GPL");
