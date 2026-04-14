// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include <linux/types.h>
#include "mtk_charger.h"
#include "mtk_disp_notify.h"
#include <linux/fb.h>
#include "charger_cooling.h"

struct charger_cooler_info {
	int num_state;
	int cur_current;
	int cur_state;
	int charger_type;
};


int lcm_status = 0; /**1 is lcm on, 0 is lcm off */
int lcm_status_switched = 1;//TN add for OCALLA-817 by lgf 20231225 
static struct charger_cooler_info charger_cl_data;

static const int master_charger_state_to_current_limit_lcdoff[CHARGER_STATE_NUM] = {
	-1, 7000000, 6000000, 4000000, 2000000, 1000000, 1000000, 1000000, 0
};

 static const int master_charger_state_to_current_limit_lcdoff_qc[CHARGER_STATE_NUM] = {
         -1, 4000000, 3000000, 2000000, 1500000, 1500000, 1000000, 1000000, 0
 };


/*==================================================
 * cooler callback functions
 *==================================================
 */
static int charger_throttle(struct charger_cooling_device *charger_cdev, unsigned long state)
{
	struct device *dev = charger_cdev->dev;
	charger_cdev->target_state = state;
        charger_cl_data.charger_type = mtk_chg_get_charger_type();
	charger_cdev->pdata->state_to_charger_limit(charger_cdev);
	charger_cl_data.cur_state = state;
	if(charger_cl_data.charger_type == CHARGER_TYPE_PPS)
		charger_cl_data.cur_current = master_charger_state_to_current_limit_lcdoff[state];
	else if(charger_cl_data.charger_type == CHARGER_TYPE_QC)
		charger_cl_data.cur_current = master_charger_state_to_current_limit_lcdoff_qc[state];
	else
		charger_cl_data.cur_current = -1;
	dev_info(dev, "%s: set lv = %ld done\n", charger_cdev->name, state);
	return 0;
}
static int charger_cooling_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct charger_cooling_device *charger_cdev = cdev->devdata;
	pr_info("charger_cooling_get_max_state begin\n");

	*state = charger_cdev->max_state;

	return 0;
}

static int charger_cooling_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct charger_cooling_device *charger_cdev = cdev->devdata;
	*state = charger_cdev->target_state;

	return 0;
}

static int charger_cooling_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct charger_cooling_device *charger_cdev = cdev->devdata;
	int ret;
	int charger_type;
	//only active when lcd off
	if (lcm_status == 1)
		return 0;
	charger_type = mtk_chg_get_charger_type();
	pr_info("charger_cooling_set_cur_state lcdoff begin,charger_type=%d,charger_cl_data.charger_type=%d,state=%d\n",charger_type,charger_cl_data.charger_type,state);
	/* Request state should be less than max_state */
	if (WARN_ON(state > charger_cdev->max_state || !charger_cdev->throttle))
		return -EINVAL;
	//TN modify begin for OCALLA-817 by lgf 20231225
	if (charger_cdev->target_state == state && charger_type == charger_cl_data.charger_type && lcm_status_switched == 0)
		return 0;
	//TN modify end for OCALLA-817 by lgf 20231225
	lcm_status_switched = 0;//TN add for OCALLA-817 by lgf 20231225
	ret = charger_cdev->throttle(charger_cdev, state);
	return ret;
}

/*==================================================
 * platform data and platform driver callbacks
 *==================================================
 */

static int cooling_state_to_charger_limit_v1(struct charger_cooling_device *chg)
{
	union power_supply_propval prop_bat_chr;
	union power_supply_propval prop_input;
	union power_supply_propval prop_vbus;
	int ret = -1;

	if (chg->chg_psy == NULL || IS_ERR(chg->chg_psy)) {
		pr_info("Couldn't get chg_psy\n");
		return ret;
	}
        if(charger_cl_data.charger_type == CHARGER_TYPE_PPS)
        	prop_bat_chr.intval = master_charger_state_to_current_limit_lcdoff[chg->target_state];
	else if (charger_cl_data.charger_type == CHARGER_TYPE_QC)
                prop_bat_chr.intval = master_charger_state_to_current_limit_lcdoff_qc[chg->target_state];
        else
                prop_bat_chr.intval = -1;
        ret = power_supply_set_property(chg->chg_psy,
		POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
		&prop_bat_chr);
	if (ret != 0) {
		pr_notice("set bat curr fail\n");
		return ret;
	}
	if (prop_bat_chr.intval == 0)
		prop_input.intval = 0;
	else
		prop_input.intval = -1;
	ret = power_supply_set_property(chg->chg_psy,
		POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &prop_input);
	if (ret != 0) {
		pr_notice("set input curr fail\n");
		return ret;
	}

	/* High Voltage (Vbus) control*/
	/*Only master charger need to control Vbus*/
	/*prop.intval = 0, vbus 5V*/
	/*prop.intval = 1, vbus 9V*/
	prop_vbus.intval = 1;

	if (prop_bat_chr.intval == 0)
		prop_vbus.intval = 0;

	ret = power_supply_set_property(chg->chg_psy,
		POWER_SUPPLY_PROP_VOLTAGE_MAX, &prop_vbus);
	if (ret != 0) {
		pr_notice("set vbus fail\n");
		return ret;
	}

	pr_notice("chr limit state %lu, chr %d, input %d, vbus %d\n",
		chg->target_state, prop_bat_chr.intval, prop_input.intval, prop_vbus.intval);

	power_supply_changed(chg->chg_psy);

	return ret;
}


static const struct charger_cooling_platform_data mt6360_pdata = {
	.state_to_charger_limit = cooling_state_to_charger_limit_v1,
};

static const struct charger_cooling_platform_data mt6375_pdata = {
	.state_to_charger_limit = cooling_state_to_charger_limit_v1,
};

static const struct of_device_id charger_cooling_of_match[] = {
	{
		.compatible = "mediatek,mt6360-charger-cooler-lcdoff",
		.data = (void *)&mt6360_pdata,
	},
	{
		.compatible = "mediatek,mt6375-charger-cooler-lcdoff",
		.data = (void *)&mt6375_pdata,
	},
	{},
};
MODULE_DEVICE_TABLE(of, charger_cooling_of_match);

static struct thermal_cooling_device_ops charger_cooling_ops = {
	.get_max_state		= charger_cooling_get_max_state,
	.get_cur_state		= charger_cooling_get_cur_state,
	.set_cur_state		= charger_cooling_set_cur_state,
};
/*return 0:single charger*/
/*return 1,2:dual charger*/
static int get_charger_config(void)
{
	struct device_node *node = NULL;
	u32 val = 0;

	node = of_find_compatible_node(NULL, NULL, "mediatek,charger");
	WARN_ON_ONCE(node == 0);

	if (of_property_read_u32(node, "charger_configuration", &val))
		return 0;
	else
		return val;
}
static ssize_t charger_curr_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "%d\n",
				charger_cl_data.cur_current);
	return len;
}

static ssize_t charger_table_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int i;
	int len = 0;

	for (i = 0; i < CHARGER_STATE_NUM; i++) {
		if (i == 0)
			len += snprintf(buf + len, PAGE_SIZE - len, "[%d,",
				-1);
		else if (i == CHARGER_STATE_NUM - 1)
			len += snprintf(buf + len, PAGE_SIZE - len, "%d]\n",
				master_charger_state_to_current_limit_lcdoff[i]/1000);
		else
			len += snprintf(buf + len, PAGE_SIZE - len, "%d,",
				master_charger_state_to_current_limit_lcdoff[i]/1000);
	}

	return len;
}

static struct kobj_attribute charger_curr_attr = __ATTR_RO(charger_curr);
static struct kobj_attribute charger_table_attr = __ATTR_RO(charger_table);

static struct attribute *charger_cooler_attrs[] = {
	&charger_curr_attr.attr,
	&charger_table_attr.attr,
	NULL
};
static struct attribute_group charger_cooler_attr_group = {
	.name	= "charg_cooler_lcdoff",
	.attrs	= charger_cooler_attrs,
};


static int lcm_notifier_callback(
struct notifier_block *self, unsigned long event, void *data)
{
	//struct fb_event *evdata = data;
	int blank;
	/* skip if it's not a blank event */
	if ((event != MTK_DISP_EVENT_BLANK) || (data == NULL))
		return 0;

	blank = *(int *)data;

	switch (blank) {
	/* LCM ON */
	case  MTK_DISP_BLANK_UNBLANK:
		lcm_status = 1;
		lcm_status_switched = 1;//TN add for OCALLA-817 by lgf 20231225
		break;
	/* LCM OFF */
	case MTK_DISP_BLANK_POWERDOWN:
		lcm_status = 0;
		break;
	default:
		break;
	}
	return 0;
}

static struct notifier_block chlmt_lcm_notifier = {
	.notifier_call = lcm_notifier_callback,
};


static int charger_cooling_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct thermal_cooling_device *cdev;
	struct charger_cooling_device *charger_cdev;
	int ret;

	pr_err("charger_cooling_probe lcdoff\n");


	charger_cdev = devm_kzalloc(dev, sizeof(*charger_cdev), GFP_KERNEL);
	if (!charger_cdev)
		return -ENOMEM;

	charger_cdev->pdata = of_device_get_match_data(dev);

	strncpy(charger_cdev->name, np->name, strlen(np->name));
	charger_cdev->target_state = CHARGER_COOLING_UNLIMITED_STATE;
	charger_cdev->dev = dev;
	charger_cdev->max_state = 5;//CHARGER_STATE_NUM - 1;
	charger_cdev->throttle = charger_throttle;
	charger_cdev->pdata = of_device_get_match_data(dev);
	charger_cdev->type = get_charger_config();
	charger_cdev->chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (charger_cdev->chg_psy == NULL || IS_ERR(charger_cdev->chg_psy)) {
		pr_info("Couldn't get chg_psy\n");
		return -EINVAL;
	}
	if (charger_cdev->type == THERMAL_DUAL_CHARGER) {
		charger_cdev->s_chg_psy = power_supply_get_by_name("mtk-slave-charger");
		if (charger_cdev->s_chg_psy == NULL || IS_ERR(charger_cdev->s_chg_psy)) {
			pr_err("Couldn't get s_chg_psy\n");
			return -EINVAL;
		}
	}

	ret = sysfs_create_group(kernel_kobj, &charger_cooler_attr_group);
	if (ret) {
		pr_err("failed to create charger cooler sysfs,!\n");
		return ret;
	}
	charger_cl_data.cur_state = 0;
	charger_cl_data.cur_current = -1;
	charger_cl_data.num_state = CHARGER_STATE_NUM;


	cdev = thermal_of_cooling_device_register(np, charger_cdev->name,
		charger_cdev, &charger_cooling_ops);
	if (IS_ERR(cdev))
		return -EINVAL;
	charger_cdev->cdev = cdev;

	platform_set_drvdata(pdev, charger_cdev);
	dev_info(dev, "register %s done\n", charger_cdev->name);

	if (mtk_disp_notifier_register("thernal_chlmt", &chlmt_lcm_notifier)) {
		dev_info(dev,"%s: register thernal_chlmt client failed!\n", __func__);
		return -EINVAL;

	}
	return 0;
}

static int charger_cooling_remove(struct platform_device *pdev)
{
	struct charger_cooling_device *charger_cdev;

	charger_cdev = (struct charger_cooling_device *)platform_get_drvdata(pdev);

	thermal_cooling_device_unregister(charger_cdev->cdev);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver charger_cooling_driver = {
	.probe = charger_cooling_probe,
	.remove = charger_cooling_remove,
	.driver = {
		.name = "mtk-charger-cooling-lcdoff",
		.of_match_table = charger_cooling_of_match,
	},
};
module_platform_driver(charger_cooling_driver);

MODULE_AUTHOR("Henry Huang <henry.huang@mediatek.com>");
MODULE_DESCRIPTION("Mediatek charger cooling driver");
MODULE_LICENSE("GPL v2");
