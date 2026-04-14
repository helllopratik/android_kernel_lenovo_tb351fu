// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

/*
 *
 * Filename:
 * ---------
 *    mtk_basic_charger.c
 *
 * Project:
 * --------
 *   Android_Software
 *
 * Description:
 * ------------
 *   This Module defines functions of Battery charging
 *
 * Author:
 * -------
 * Wy Chuang
 *
 */
#include <linux/init.h>		/* For init/exit macros */
#include <linux/module.h>	/* For MODULE_ marcros  */
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/power_supply.h>
#include <linux/pm_wakeup.h>
#include <linux/time.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/scatterlist.h>
#include <linux/suspend.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/reboot.h>

#include "mtk_charger.h"
// TN modified by wangyang 20231207 OCALLA-335 begin
#include "../../oem/switch_charger/charge_common.h"
// TN modified by wangyang 20231207 OCALLA-335 end

// TN modified by liliangwu 20231222 OCALLA-336 begin
#define RECHG_SOC                      94
#define PPS_INPUT_CURRENT              2000000
#define PPS_CHARGER_CURRENT            4000000
#define PD_CHARGER_VOLTAGE_5V          5000
#define PD_CHARGER_VOLTAGE_9V          9000
#define HVDCP_VINDPM_UV                8400000
// TN modified by liliangwu 20231222 OCALLA-336 end

// TN modified by wangyang 20240110 OCALLA-1352 begin
#define HVDCP_CHARGE_CURRENT_UA        4000000
#define PARALLEL_CHARGE_THRES_UA       2000000
// TN modified by wangyang 20240110 OCALLA-1352 end

// TN modified by liliangwu 20240220 OCALLA-6391 begin
#define PD30_INPUT_CURRENT             2000000
#define PD9V_VINDPM_UV                 8800000
// TN modified by liliangwu 20240220 OCALLA-6391 end

// TN modified by wangyang 20240112 OCALLA-166 begin
static int get_bat_maint1_cv(struct mtk_charger *info)
{
	union power_supply_propval val;
	int ret;

	if (!info->bat_psy)
		info->bat_psy = power_supply_get_by_name("battery");
	if (!info->bat_psy){
		chr_err("%s: get battery power supply fail\n", __func__);
		return 0;
	}

	ret = power_supply_get_property(info->bat_psy, POWER_SUPPLY_PROP_CYCLE_COUNT, &val);
	if (ret) {
		chr_err("%s: get cycle count fail\n", __func__);
		return 0;
	}

	chr_err("%s: get cycle count = %d\n", __func__, val.intval);

	if ((val.intval >= BAT_CYCLE_COUNT_LEVEL1) && (val.intval < BAT_CYCLE_COUNT_LEVEL2))
		return BAT_MAINT_CHARGE_CV1_UV;
	else if ((val.intval >= BAT_CYCLE_COUNT_LEVEL2) && (val.intval < BAT_CYCLE_COUNT_LEVEL3))
		return BAT_MAINT_CHARGE_CV2_UV;
	else if (val.intval >= BAT_CYCLE_COUNT_LEVEL3)
		return BAT_MAINT_CHARGE_CV3_UV;
	return 0;
}

static int get_bat_volt_setting(struct mtk_charger *info)
{
	union power_supply_propval val;
	int ret;

	if (!info->bat_psy)
		info->bat_psy = power_supply_get_by_name("battery");
	if (!info->bat_psy){
		chr_err("%s: get battery power supply fail\n", __func__);
		return 0;
	}

	ret = power_supply_get_property(info->bat_psy, POWER_SUPPLY_PROP_VOLTAGE_MAX, &val);
	if (ret) {
		chr_err("%s: get battery voltage setting fail\n", __func__);
		return 0;
	}
	return val.intval;
}

static int write_bat_volt_setting(struct mtk_charger *info, int volt)
{
	union power_supply_propval val;

	val.intval = volt;
	if (!info->bat_psy)
		info->bat_psy = power_supply_get_by_name("battery");
	if (!info->bat_psy){
		chr_err("%s: get battery power supply fail\n", __func__);
		return -1;
	}
	return power_supply_set_property(info->bat_psy, POWER_SUPPLY_PROP_VOLTAGE_MAX, &val);
}
// TN modified by wangyang 20240112 OCALLA-166 end

// TN modified by wangyang 20231229 OCALLA-782 begin
#define BAT_MAINT2_CV 4330000

static void select_bat_maint_cv(struct mtk_charger *info)
{
	int maint1_cv = 0;
	int bat_volt_set;

	bat_volt_set = get_bat_volt_setting(info);

	if (info->bat_maint2) {
		if (bat_volt_set != BAT_MAINT_GAUGE_VOL3_MV)
			write_bat_volt_setting(info, BAT_MAINT_GAUGE_VOL3_MV);
		if (BAT_MAINT2_CV < info->setting.cv)
			info->setting.cv = BAT_MAINT2_CV;
	} else if (info->bat_maint) {
		maint1_cv = get_bat_maint1_cv(info);
		if (maint1_cv == BAT_MAINT_CHARGE_CV1_UV) {
			if (bat_volt_set != BAT_MAINT_GAUGE_VOL1_MV)
				write_bat_volt_setting(info, BAT_MAINT_GAUGE_VOL1_MV);
		} else if (maint1_cv == BAT_MAINT_CHARGE_CV2_UV) {
			if (bat_volt_set != BAT_MAINT_GAUGE_VOL2_MV)
				write_bat_volt_setting(info, BAT_MAINT_GAUGE_VOL2_MV);
		} else if (maint1_cv == BAT_MAINT_CHARGE_CV3_UV) {
			if (bat_volt_set != BAT_MAINT_GAUGE_VOL3_MV)
				write_bat_volt_setting(info, BAT_MAINT_GAUGE_VOL3_MV);
		} else {
			if (bat_volt_set != BAT_MAINT_GAUGE_VOL0_MV)
				write_bat_volt_setting(info, BAT_MAINT_GAUGE_VOL0_MV);
		}
		if (maint1_cv > 0 && maint1_cv < info->setting.cv)
			info->setting.cv = maint1_cv;
	} else {
		if (bat_volt_set != BAT_MAINT_GAUGE_VOL0_MV)
			write_bat_volt_setting(info, BAT_MAINT_GAUGE_VOL0_MV);
	}
	chr_err("%s: bat_maint1 = %d, bat_maint2 = %d, maint1_cv = %d, bat_volt_set = %d\n",
		__func__, info->bat_maint, info->bat_maint2, maint1_cv, bat_volt_set);
}
// TN modified by wangyang 20231229 OCALLA-782 end

static int _uA_to_mA(int uA)
{
	if (uA == -1)
		return -1;
	else
		return uA / 1000;
}

static void select_cv(struct mtk_charger *info)
{
	u32 constant_voltage;

	if (info->enable_sw_jeita)
		if (info->sw_jeita.cv != 0) {
			info->setting.cv = info->sw_jeita.cv;
// TN modified by wangyang 20231229 OCALLA-782 begin
			select_bat_maint_cv(info);
// TN modified by wangyang 20231229 OCALLA-782 end
			return;
		}

	constant_voltage = info->data.battery_cv;
	info->setting.cv = constant_voltage;
}

static bool is_typec_adapter(struct mtk_charger *info)
{
	int rp;

	rp = adapter_dev_get_property(info->pd_adapter, TYPEC_RP_LEVEL);
	if (info->pd_type == MTK_PD_CONNECT_TYPEC_ONLY_SNK &&
			rp != 500 &&
// TN modified by wangyang 20240110 OCALLA-1352 begin
			info->chr_type == POWER_SUPPLY_TYPE_USB_OTHER)
// TN modified by wangyang 20240110 OCALLA-1352 end
		return true;

	return false;
}

static bool support_fast_charging(struct mtk_charger *info)
{
	struct chg_alg_device *alg;
	int i = 0, state = 0;
	bool ret = false;

	for (i = 0; i < MAX_ALG_NO; i++) {
		alg = info->alg[i];
		if (alg == NULL)
			continue;

		if (info->enable_fast_charging_indicator &&
		    ((alg->alg_id & info->fast_charging_indicator) == 0))
			continue;

		chg_alg_set_current_limit(alg, &info->setting);
		state = chg_alg_is_algo_ready(alg);
		chr_debug("%s %s ret:%s\n", __func__, dev_name(&alg->dev),
			chg_alg_state_to_str(state));

		if (state == ALG_READY || state == ALG_RUNNING) {
			ret = true;
			break;
		}
	}
	return ret;
}

static bool select_charging_current_limit(struct mtk_charger *info,
	struct chg_limit_setting *setting)
{
	struct charger_data *pdata, *pdata2, *pdata_dvchg, *pdata_dvchg2;
	bool is_basic = false;
	u32 ichg1_min = 0, aicr1_min = 0;
	int ret;
// TN modified by liliangwu 20240220 OCALLA-6391 begin
	int i;
	struct adapter_power_cap acap = {0};
// TN modified by liliangwu 20240220 OCALLA-6391 end

	select_cv(info);

	pdata = &info->chg_data[CHG1_SETTING];
	pdata2 = &info->chg_data[CHG2_SETTING];
	pdata_dvchg = &info->chg_data[DVCHG1_SETTING];
	pdata_dvchg2 = &info->chg_data[DVCHG2_SETTING];
	if (info->usb_unlimited) {
		pdata->input_current_limit =
					info->data.ac_charger_input_current;
		pdata->charging_current_limit =
					info->data.ac_charger_current;
		is_basic = true;
		goto done;
	}

	if (info->water_detected) {
		pdata->input_current_limit = info->data.usb_charger_current;
		pdata->charging_current_limit = info->data.usb_charger_current;
		is_basic = true;
		goto done;
	}

	if (((info->bootmode == 1) ||
	    (info->bootmode == 5)) && info->enable_meta_current_limit != 0) {
		pdata->input_current_limit = 200000; // 200mA
		is_basic = true;
		goto done;
	}

// TN modified by wangyang 20231207 OCALLA-424 begin
#if 0
	if (info->atm_enabled == true
		&& (info->chr_type == POWER_SUPPLY_TYPE_USB ||
		info->chr_type == POWER_SUPPLY_TYPE_USB_CDP)
		) {
		pdata->input_current_limit = 100000; /* 100mA */
		is_basic = true;
		goto done;
	}
#endif
// TN modified by wangyang 20231207 OCALLA-424 end

// TN modified by liliangwu 20231222 OCALLA-336 begin
	if (info->pd_type == MTK_PD_CONNECT_PE_READY_SNK_APDO &&
		info->chr_type != POWER_SUPPLY_TYPE_UNKNOWN) {
		pdata->input_current_limit = PPS_INPUT_CURRENT;
		pdata->charging_current_limit = PPS_CHARGER_CURRENT;
	} else if (info->pd_type == MTK_PD_CONNECT_PE_READY_SNK_PD30 || info->pd_type == MTK_PD_CONNECT_PE_READY_SNK) {
		if (info->sub_pd_type == CHARGER_TYPE_UNKNOW) {
			adapter_dev_get_cap(info->pd_adapter, MTK_PD, &acap);
			for (i = 0; i < acap.nr; i++) {
				if(info->sub_pd_type == CHARGER_TYPE_PD_9V && acap.max_mv[i] == PD_CHARGER_VOLTAGE_9V
					&& acap.ma[i] > info->pd_input_current_limit) {
					info->pd_input_current_limit = acap.ma[i];
				}else if (info->sub_pd_type != CHARGER_TYPE_PD_9V && acap.max_mv[i] == PD_CHARGER_VOLTAGE_9V) {
					info->sub_pd_type = CHARGER_TYPE_PD_9V;
					info->pd_input_current_limit = acap.ma[i];
				} else if (info->sub_pd_type == CHARGER_TYPE_PD && acap.max_mv[i] == PD_CHARGER_VOLTAGE_5V
					&& acap.ma[i] > info->pd_input_current_limit) {
					info->pd_input_current_limit = acap.ma[i];
				} else if (info->sub_pd_type == CHARGER_TYPE_UNKNOW && acap.max_mv[i] == PD_CHARGER_VOLTAGE_5V) {
					info->sub_pd_type = CHARGER_TYPE_PD;
					info->pd_input_current_limit = acap.ma[i];
				}
			}
		}
		pdata->input_current_limit = min(PD30_INPUT_CURRENT, info->pd_input_current_limit * 1000);
		if (info->sub_pd_type == CHARGER_TYPE_PD_9V) {
			charger_dev_set_mivr(info->chg1_dev, PD9V_VINDPM_UV);
			charger_dev_set_mivr(info->sub_chg1_dev, PD9V_VINDPM_UV);
			charger_dev_set_mivr(info->sub_chg2_dev, PD9V_VINDPM_UV);
			adapter_dev_set_cap(info->pd_adapter, MTK_PD, PD_CHARGER_VOLTAGE_9V, _uA_to_mA(pdata->input_current_limit));
			pdata->charging_current_limit = pdata->input_current_limit << 1;
		} else if (info->sub_pd_type == CHARGER_TYPE_PD) {
			adapter_dev_set_cap(info->pd_adapter, MTK_PD, PD_CHARGER_VOLTAGE_5V, _uA_to_mA(pdata->input_current_limit));
			pdata->charging_current_limit = pdata->input_current_limit;
		}
	} else if (info->chr_type == POWER_SUPPLY_TYPE_USB &&
	    info->usb_type == POWER_SUPPLY_USB_TYPE_SDP) {
// TN modified by liliangwu 20231222 OCALLA-336 end
		pdata->input_current_limit =
				info->data.usb_charger_current;
		/* it can be larger */
		pdata->charging_current_limit =
				info->data.usb_charger_current;
		is_basic = true;
	} else if (info->chr_type == POWER_SUPPLY_TYPE_USB_CDP) {
		pdata->input_current_limit =
			info->data.charging_host_charger_current;
		pdata->charging_current_limit =
			info->data.charging_host_charger_current;
		is_basic = true;

	} else if (info->chr_type == POWER_SUPPLY_TYPE_USB_DCP) {
		pdata->input_current_limit =
			info->data.ac_charger_input_current;
		pdata->charging_current_limit =
			info->data.ac_charger_current;
		if (info->config == DUAL_CHARGERS_IN_SERIES) {
			pdata2->input_current_limit =
				pdata->input_current_limit;
			pdata2->charging_current_limit = 2000000;
		}
// TN modified by wangyang 20231207 OCALLA-335 begin
		/* NONSTANDARD_CHARGER */
	} else if (info->chr_type == POWER_SUPPLY_TYPE_USB_DIVIDER1) {
		pdata->input_current_limit = DIVIDER1_IINDPM_UA;
		pdata->charging_current_limit = DIVIDER1_IINDPM_UA;
		is_basic = true;
	} else if (info->chr_type == POWER_SUPPLY_TYPE_USB_DIVIDER2) {
		pdata->input_current_limit = DIVIDER2_IINDPM_UA;
		pdata->charging_current_limit = DIVIDER2_IINDPM_UA;
		is_basic = true;
	} else if (info->chr_type == POWER_SUPPLY_TYPE_USB_DIVIDER3) {
		pdata->input_current_limit = DIVIDER3_IINDPM_UA;
		pdata->charging_current_limit = DIVIDER3_IINDPM_UA;
		is_basic = true;
	} else if (info->chr_type == POWER_SUPPLY_TYPE_USB_DIVIDER4) {
		pdata->input_current_limit = DIVIDER4_IINDPM_UA;
		pdata->charging_current_limit = DIVIDER4_IINDPM_UA;
		is_basic = true;
// TN modified by wangyang 20231207 OCALLA-335 end
// TN modified by wangyang 20240110 OCALLA-1352 begin
	} else if (info->chr_type == POWER_SUPPLY_TYPE_USB_HVDCP) {
		pdata->input_current_limit = info->data.ac_charger_input_current;
		pdata->charging_current_limit = HVDCP_CHARGE_CURRENT_UA;
// TN modified by wangyang 20240110 OCALLA-1352 end
	} else {
		/*chr_type && usb_type cannot match above, set 500mA*/
		pdata->input_current_limit =
				info->data.usb_charger_current;
		pdata->charging_current_limit =
				info->data.usb_charger_current;
		is_basic = true;
	}

	if (support_fast_charging(info))
		is_basic = false;
	else {
		is_basic = true;
		/* AICL */
		if (!info->disable_aicl)
			charger_dev_run_aicl(info->chg1_dev,
				&pdata->input_current_limit_by_aicl);
		if (info->enable_dynamic_mivr) {
			if (pdata->input_current_limit_by_aicl >
				info->data.max_dmivr_charger_current)
				pdata->input_current_limit_by_aicl =
					info->data.max_dmivr_charger_current;
		}
		if (is_typec_adapter(info)) {
			if (adapter_dev_get_property(info->pd_adapter, TYPEC_RP_LEVEL)
				== 3000) {
				pdata->input_current_limit = 3000000;
				pdata->charging_current_limit = 3000000;
			} else if (adapter_dev_get_property(info->pd_adapter,
				TYPEC_RP_LEVEL) == 1500) {
				pdata->input_current_limit = 1500000;
				pdata->charging_current_limit = 2000000;
			} else {
				chr_err("type-C: inquire rp error\n");
				pdata->input_current_limit = 500000;
				pdata->charging_current_limit = 500000;
			}

			chr_err("type-C:%d current:%d\n",
				info->pd_type,
				adapter_dev_get_property(info->pd_adapter,
					TYPEC_RP_LEVEL));
		}
	}

// TN modified by liliangwu 20231215 OCALLA-336 begin
	info->setting.input_current_limit_dvchg1 = pdata->thermal_input_current_limit;
	info->setting.charging_current_limit_dvchg1 = pdata->thermal_charging_current_limit;
	if (info->enable_sw_jeita) {
		if (info->sw_jeita.jeita_charging_current_limit != -1 &&
			info->sw_jeita.jeita_charging_current_limit <= pdata->charging_current_limit) {
			pdata->charging_current_limit = info->sw_jeita.jeita_charging_current_limit;
			info->setting.charging_current_limit1 = pdata->charging_current_limit;
		}

		if (info->setting.charging_current_limit_dvchg1 == -1 || (info->sw_jeita.jeita_charging_current_limit != -1 &&
			info->sw_jeita.jeita_charging_current_limit <= info->setting.charging_current_limit_dvchg1)) {
			info->setting.charging_current_limit_dvchg1 = info->sw_jeita.jeita_charging_current_limit;
		}
	}
// TN modified by liliangwu 20231215 OCALLA-336 end

	sc_select_charging_current(info, pdata);

	if (pdata->thermal_charging_current_limit != -1) {
		if (pdata->thermal_charging_current_limit <=
			pdata->charging_current_limit) {
			pdata->charging_current_limit =
					pdata->thermal_charging_current_limit;
			info->setting.charging_current_limit1 =
					pdata->thermal_charging_current_limit;
		}
		pdata->thermal_throttle_record = true;
	} else
		info->setting.charging_current_limit1 = info->sc.sc_ibat;

	if (pdata->thermal_input_current_limit != -1) {
		if (pdata->thermal_input_current_limit <=
			pdata->input_current_limit) {
			pdata->input_current_limit =
					pdata->thermal_input_current_limit;
			info->setting.input_current_limit1 =
					pdata->input_current_limit;
		}
		pdata->thermal_throttle_record = true;
	} else
		info->setting.input_current_limit1 = -1;

	if (pdata2->thermal_charging_current_limit != -1) {
		if (pdata2->thermal_charging_current_limit <=
			pdata2->charging_current_limit) {
			pdata2->charging_current_limit =
					pdata2->thermal_charging_current_limit;
			info->setting.charging_current_limit2 =
					pdata2->charging_current_limit;
		}
	} else
		info->setting.charging_current_limit2 = info->sc.sc_ibat;

	if (pdata2->thermal_input_current_limit != -1) {
		if (pdata2->thermal_input_current_limit <=
			pdata2->input_current_limit) {
			pdata2->input_current_limit =
					pdata2->thermal_input_current_limit;
			info->setting.input_current_limit2 =
					pdata2->input_current_limit;
		}
	} else
		info->setting.input_current_limit2 = -1;

	if (is_basic == true && pdata->input_current_limit_by_aicl != -1
		&& !info->charger_unlimited
		&& !info->disable_aicl) {
		if (pdata->input_current_limit_by_aicl <
		    pdata->input_current_limit)
			pdata->input_current_limit =
					pdata->input_current_limit_by_aicl;
	}

done:

	ret = charger_dev_get_min_charging_current(info->chg1_dev, &ichg1_min);
	if (ret != -EOPNOTSUPP && pdata->charging_current_limit < ichg1_min) {
		pdata->charging_current_limit = 0;
		/* For TC_018, pleasae don't modify the format */
		chr_err("min_charging_current is too low %d %d\n",
			pdata->charging_current_limit, ichg1_min);
		is_basic = true;
	}

	ret = charger_dev_get_min_input_current(info->chg1_dev, &aicr1_min);
	if (ret != -EOPNOTSUPP && pdata->input_current_limit < aicr1_min) {
		pdata->input_current_limit = 0;
		/* For TC_018, pleasae don't modify the format */
		chr_err("min_input_current is too low %d %d\n",
			pdata->input_current_limit, aicr1_min);
		is_basic = true;
	}
// TN modified by liliangwu 20231215 OCALLA-336 begin
	/* For TC_018, pleasae don't modify the format */
	chr_err("m:%d chg1:%d,%d,%d,%d chg2:%d,%d,%d,%d dvchg1:%d sc:%d %d %d type:%d:%d usb_unlimited:%d usbif:%d usbsm:%d aicl:%d atm:%d bm:%d b:%d jeita_limit:%d\n",
		info->config,
		_uA_to_mA(pdata->thermal_input_current_limit),
		_uA_to_mA(pdata->thermal_charging_current_limit),
		_uA_to_mA(pdata->input_current_limit),
		_uA_to_mA(pdata->charging_current_limit),
		_uA_to_mA(pdata2->thermal_input_current_limit),
		_uA_to_mA(pdata2->thermal_charging_current_limit),
		_uA_to_mA(pdata2->input_current_limit),
		_uA_to_mA(pdata2->charging_current_limit),
		_uA_to_mA(pdata_dvchg->thermal_input_current_limit),
		info->sc.pre_ibat,
		info->sc.sc_ibat,
		info->sc.solution,
		info->chr_type, info->pd_type,
		info->usb_unlimited,
		IS_ENABLED(CONFIG_USBIF_COMPLIANCE), info->usb_state,
		pdata->input_current_limit_by_aicl, info->atm_enabled,
		info->bootmode, is_basic,
		_uA_to_mA(info->sw_jeita.jeita_charging_current_limit));
// TN modified by liliangwu 20231215 OCALLA-336 end

	return is_basic;
}

// TN modified by wangyang 20240110 OCALLA-1352 begin
static void sub_buck_current_config(struct mtk_charger *info)
{
	struct charger_data *pdata = &info->chg_data[CHG1_SETTING];

	if (info->chr_type != POWER_SUPPLY_TYPE_USB && info->chr_type != POWER_SUPPLY_TYPE_USB_OTHER) {
		charger_dev_set_input_current(info->sub_chg1_dev, pdata->input_current_limit);
		charger_dev_set_input_current(info->sub_chg2_dev, pdata->input_current_limit);
	}
	if (info->chr_type == POWER_SUPPLY_TYPE_USB_HVDCP || info->sub_pd_type == CHARGER_TYPE_PD_9V) {
		if (pdata->charging_current_limit > PARALLEL_CHARGE_THRES_UA) {
			pdata->charging_current_limit /= 3;
			charger_dev_set_charging_current(info->sub_chg1_dev, pdata->charging_current_limit);
			charger_dev_set_charging_current(info->sub_chg2_dev, pdata->charging_current_limit);
			charger_dev_enable(info->sub_chg1_dev, true);
			charger_dev_enable(info->sub_chg2_dev, true);
		} else {
			charger_dev_enable(info->sub_chg1_dev, false);
			charger_dev_enable(info->sub_chg2_dev, false);
		}
	}
}
// TN modified by wangyang 20240110 OCALLA-1352 end

// TN modified by liliangwu 20231222 OCALLA-336 begin
void reset_buck_to_default(struct mtk_charger *info);
// TN modified by liliangwu 20231222 OCALLA-336 end
static int do_algorithm(struct mtk_charger *info)
{
	struct chg_alg_device *alg;
	struct charger_data *pdata;
	struct chg_alg_notify notify;
	bool is_basic = true;
	bool chg_done = false;
	int i;
	int ret;
	int val = 0;
// TN modified by liliangwu 20231222 OCALLA-336 begin
	int cv;
// TN modified by liliangwu 20231222 OCALLA-336 end

	pdata = &info->chg_data[CHG1_SETTING];
	charger_dev_is_charging_done(info->chg1_dev, &chg_done);
	is_basic = select_charging_current_limit(info, &info->setting);

// TN modified by liliangwu 20231222 OCALLA-336 begin
	if (info->is_chg_done) {
		if (get_uisoc(info) <= RECHG_SOC) {
			info->is_chg_done = false;
			charger_dev_do_event(info->chg1_dev, EVENT_RECHARGE, 0);
			info->polling_interval = CHARGING_INTERVAL;
			chr_err("%s battery recharge\n", __func__);
		} else {
			chr_err("%s charger termination\n", __func__);
			return 0;
		}
	} else if (info->is_chg_done != chg_done) {
		info->is_chg_done = chg_done;
		charger_dev_do_event(info->chg1_dev, EVENT_FULL, 0);
		info->polling_interval = CHARGING_FULL_INTERVAL;
		chr_err("%s battery full\n", __func__);

		reset_buck_to_default(info);
		if ((info->pd_type == MTK_PD_CONNECT_NONE || info->pd_type == MTK_PD_CONNECT_TYPEC_ONLY_SNK) &&
			(info->chr_type == POWER_SUPPLY_TYPE_USB || info->chr_type == POWER_SUPPLY_TYPE_USB_OTHER)) {
			charger_dev_enable_hz(info->sub_chg1_dev, true);
			charger_dev_enable_hz(info->sub_chg2_dev, true);
		}

		return 0;
	}
// TN modified by liliangwu 20231222 OCALLA-336 end

	chr_err("%s is_basic:%d\n", __func__, is_basic);
	if (is_basic != true) {
		is_basic = true;
		for (i = 0; i < MAX_ALG_NO; i++) {
			alg = info->alg[i];
			if (alg == NULL)
				continue;

			if (info->enable_fast_charging_indicator &&
			    ((alg->alg_id & info->fast_charging_indicator) == 0))
				continue;

			if (!info->enable_hv_charging ||
			    pdata->charging_current_limit == 0 ||
			    pdata->input_current_limit == 0) {
				chg_alg_get_prop(alg, ALG_MAX_VBUS, &val);
				if (val > 5000)
					chg_alg_stop_algo(alg);
				chr_err("%s: alg:%s alg_vbus:%d\n", __func__,
					dev_name(&alg->dev), val);
				continue;
			}

			if (chg_done != info->is_chg_done) {
				if (chg_done) {
					notify.evt = EVT_FULL;
					notify.value = 0;
				} else {
					notify.evt = EVT_RECHARGE;
					notify.value = 0;
				}
				chg_alg_notifier_call(alg, &notify);
				chr_err("%s notify:%d\n", __func__, notify.evt);
			}

			chg_alg_set_current_limit(alg, &info->setting);
			ret = chg_alg_is_algo_ready(alg);

			chr_err("%s %s ret:%s\n", __func__,
				dev_name(&alg->dev),
				chg_alg_state_to_str(ret));

			if (ret == ALG_INIT_FAIL || ret == ALG_TA_NOT_SUPPORT) {
				/* try next algorithm */
				continue;
			} else if (ret == ALG_TA_CHECKING || ret == ALG_DONE ||
						ret == ALG_NOT_READY) {
				/* wait checking , use basic first */
				is_basic = true;
				break;
			} else if (ret == ALG_READY || ret == ALG_RUNNING) {
				is_basic = false;
				//chg_alg_set_setting(alg, &info->setting);
				chg_alg_start_algo(alg);
				break;
			} else {
				chr_err("algorithm ret is error");
				is_basic = true;
			}
		}
	} else {
		if (info->enable_hv_charging != true ||
		    pdata->charging_current_limit == 0 ||
		    pdata->input_current_limit == 0) {
			for (i = 0; i < MAX_ALG_NO; i++) {
				alg = info->alg[i];
				if (alg == NULL)
					continue;

				chg_alg_get_prop(alg, ALG_MAX_VBUS, &val);
				if (val > 5000 && chg_alg_is_algo_running(alg))
					chg_alg_stop_algo(alg);

				chr_err("%s: Stop hv charging. en_hv:%d alg:%s alg_vbus:%d\n",
					__func__, info->enable_hv_charging,
					dev_name(&alg->dev), val);
			}
		}
	}
	info->is_chg_done = chg_done;

	if (is_basic == true) {
// TN modified by wangyang 20240110 OCALLA-1352 begin
		sub_buck_current_config(info);
// TN modified by wangyang 20240110 OCALLA-1352 end
		charger_dev_set_input_current(info->chg1_dev,
			pdata->input_current_limit);
		charger_dev_set_charging_current(info->chg1_dev,
			pdata->charging_current_limit);

		chr_debug("%s:old_cv=%d,cv=%d, vbat_mon_en=%d\n",
			__func__,
			info->old_cv,
			info->setting.cv,
			info->setting.vbat_mon_en);
// TN modified by liliangwu 20231222 OCALLA-336 begin
		cv = min(info->setting.cv, info->data.battery_cv);
		if (info->old_cv == 0 || (info->old_cv != cv)
		    || info->setting.vbat_mon_en == 0) {
			charger_dev_enable_6pin_battery_charging(
				info->chg1_dev, false);
			charger_dev_set_constant_voltage(info->chg1_dev, cv);
// TN modified by wangyang 20240115 OCALLA-308 begin
			charger_dev_set_constant_voltage(info->sub_chg1_dev, cv);
			charger_dev_set_constant_voltage(info->sub_chg2_dev, cv);
// TN modified by wangyang 20240115 OCALLA-308 end
			if (info->setting.vbat_mon_en && info->stop_6pin_re_en != 1)
				charger_dev_enable_6pin_battery_charging(
					info->chg1_dev, true);
			info->old_cv = cv;
// TN modified by liliangwu 20231222 OCALLA-336 end
		} else {
			if (info->setting.vbat_mon_en && info->stop_6pin_re_en != 1) {
				info->stop_6pin_re_en = 1;
				charger_dev_enable_6pin_battery_charging(
					info->chg1_dev, true);
			}
		}
	}

	if (pdata->input_current_limit == 0 ||
	    pdata->charging_current_limit == 0)
		charger_dev_enable(info->chg1_dev, false);
	else {
		alg = get_chg_alg_by_name("pe5");
		ret = chg_alg_is_algo_ready(alg);
		if (!(ret == ALG_READY || ret == ALG_RUNNING))
			charger_dev_enable(info->chg1_dev, true);
	}

	if (info->chg1_dev != NULL)
		charger_dev_dump_registers(info->chg1_dev);

	if (info->chg2_dev != NULL)
		charger_dev_dump_registers(info->chg2_dev);

// TN modified by liliangwu 20231227 OCALLA-873 begin
	if (info->sub_chg1_dev != NULL)
		charger_dev_dump_registers(info->sub_chg1_dev);

	if (info->sub_chg2_dev != NULL)
		charger_dev_dump_registers(info->sub_chg2_dev);
// TN modified by liliangwu 20231227 OCALLA-873 begin

	return 0;
}

// TN modified by wangyang 20240111 OCALLA-979 begin
static void charging_restore_sub_buck_config(struct mtk_charger *info)
{
	if (info->pd_type == MTK_PD_CONNECT_NONE || info->pd_type == MTK_PD_CONNECT_TYPEC_ONLY_SNK) {
		if (info->chr_type == POWER_SUPPLY_TYPE_USB_HVDCP)  {
			charger_dev_enable(info->sub_chg1_dev, true);
			charger_dev_enable(info->sub_chg2_dev, true);
		}
	} else if (info->sub_pd_type == CHARGER_TYPE_PD_9V){
		charger_dev_enable(info->sub_chg1_dev, true);
		charger_dev_enable(info->sub_chg2_dev, true);
	}
}
// TN modified by wangyang 20240111 OCALLA-979 end

static int enable_charging(struct mtk_charger *info,
						bool en)
{
	int i;
	struct chg_alg_device *alg;

	chr_err("%s %d\n", __func__, en);

	if (en == false) {
		for (i = 0; i < MAX_ALG_NO; i++) {
			alg = info->alg[i];
			if (alg == NULL)
				continue;
			chg_alg_stop_algo(alg);
		}
		charger_dev_enable(info->chg1_dev, false);
// TN modified by wangyang 20240111 OCALLA-979 begin
		charger_dev_enable(info->sub_chg1_dev, false);
		charger_dev_enable(info->sub_chg2_dev, false);
// TN modified by wangyang 20240111 OCALLA-979 end
		charger_dev_do_event(info->chg1_dev, EVENT_DISCHARGE, 0);
	} else {
		charger_dev_enable(info->chg1_dev, true);
// TN modified by wangyang 20240111 OCALLA-979 begin
		charging_restore_sub_buck_config(info);
// TN modified by wangyang 20240111 OCALLA-979 end
		charger_dev_do_event(info->chg1_dev, EVENT_RECHARGE, 0);
	}

	return 0;
}

static int charger_dev_event(struct notifier_block *nb, unsigned long event,
				void *v)
{
	struct chg_alg_device *alg;
	struct chg_alg_notify notify;
	struct mtk_charger *info =
			container_of(nb, struct mtk_charger, chg1_nb);
	struct chgdev_notify *data = v;
	int i;

	chr_err("%s %lu\n", __func__, event);

	switch (event) {
	case CHARGER_DEV_NOTIFY_EOC:
		info->stop_6pin_re_en = 1;
		notify.evt = EVT_FULL;
		notify.value = 0;
		for (i = 0; i < 10; i++) {
			alg = info->alg[i];
			chg_alg_notifier_call(alg, &notify);
		}

		break;
	case CHARGER_DEV_NOTIFY_RECHG:
		pr_info("%s: recharge\n", __func__);
		break;
	case CHARGER_DEV_NOTIFY_SAFETY_TIMEOUT:
		info->safety_timeout = true;
		pr_info("%s: safety timer timeout\n", __func__);
		break;
	case CHARGER_DEV_NOTIFY_VBUS_OVP:
		info->vbusov_stat = data->vbusov_stat;
		pr_info("%s: vbus ovp = %d\n", __func__, info->vbusov_stat);
		break;
	case CHARGER_DEV_NOTIFY_BATPRO_DONE:
		info->batpro_done = true;
		info->setting.vbat_mon_en = 0;
		notify.evt = EVT_BATPRO_DONE;
		notify.value = 0;
		for (i = 0; i < 10; i++) {
			alg = info->alg[i];
			chg_alg_notifier_call(alg, &notify);
		}
		pr_info("%s: batpro_done = %d\n", __func__, info->batpro_done);
		break;
	default:
		return NOTIFY_DONE;
	}

	if (info->chg1_dev->is_polling_mode == false)
		_wake_up_charger(info);

	return NOTIFY_DONE;
}

static int to_alg_notify_evt(unsigned long evt)
{
	switch (evt) {
	case CHARGER_DEV_NOTIFY_VBUS_OVP:
		return EVT_VBUSOVP;
	case CHARGER_DEV_NOTIFY_IBUSOCP:
		return EVT_IBUSOCP;
	case CHARGER_DEV_NOTIFY_IBUSUCP_FALL:
		return EVT_IBUSUCP_FALL;
	case CHARGER_DEV_NOTIFY_BAT_OVP:
		return EVT_VBATOVP;
	case CHARGER_DEV_NOTIFY_IBATOCP:
		return EVT_IBATOCP;
	case CHARGER_DEV_NOTIFY_VBATOVP_ALARM:
		return EVT_VBATOVP_ALARM;
	case CHARGER_DEV_NOTIFY_VBUSOVP_ALARM:
		return EVT_VBUSOVP_ALARM;
	case CHARGER_DEV_NOTIFY_VOUTOVP:
		return EVT_VOUTOVP;
	case CHARGER_DEV_NOTIFY_VDROVP:
		return EVT_VDROVP;
	default:
		return -EINVAL;
	}
}

static int dvchg1_dev_event(struct notifier_block *nb, unsigned long event,
			    void *data)
{
	struct mtk_charger *info =
		container_of(nb, struct mtk_charger, dvchg1_nb);
	int alg_evt = to_alg_notify_evt(event);

	chr_info("%s %ld", __func__, event);
	if (alg_evt < 0)
		return NOTIFY_DONE;
	mtk_chg_alg_notify_call(info, alg_evt, 0);
	return NOTIFY_OK;
}

static int dvchg2_dev_event(struct notifier_block *nb, unsigned long event,
			    void *data)
{
	struct mtk_charger *info =
		container_of(nb, struct mtk_charger, dvchg1_nb);
	int alg_evt = to_alg_notify_evt(event);

	chr_info("%s %ld", __func__, event);
	if (alg_evt < 0)
		return NOTIFY_DONE;
	mtk_chg_alg_notify_call(info, alg_evt, 0);
	return NOTIFY_OK;
}


int mtk_basic_charger_init(struct mtk_charger *info)
{

	info->algo.do_algorithm = do_algorithm;
	info->algo.enable_charging = enable_charging;
	info->algo.do_event = charger_dev_event;
	info->algo.do_dvchg1_event = dvchg1_dev_event;
	info->algo.do_dvchg2_event = dvchg2_dev_event;
	//info->change_current_setting = mtk_basic_charging_current;
	return 0;
}
