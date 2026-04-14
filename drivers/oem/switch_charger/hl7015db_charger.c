// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the TI hl7015 battery charger.
 *
 * Author: Mark A. Greer <mgreer@animalcreek.com>
 */
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/of_irq.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/power_supply.h>
//#include <linux/power/hl7015_charger.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/iio/consumer.h>
#include "../../power/supply/charger_class.h"
#include "../../power/supply/mtk_charger.h"
#include "../../misc/mediatek/usb20/mtk_musb.h"
#include "charge_common.h"

#define	HL7015_MANUFACTURER	"Texas Instruments"
#define HL7015_REG_ISC		0x00 /* Input Source Control */
#define HL7015_REG_ISC_EN_HIZ_MASK		BIT(7)
#define HL7015_REG_ISC_EN_HIZ_SHIFT		7
#define HL7015_REG_ISC_VINDPM_MASK		(BIT(6) | BIT(5) | BIT(4) | \
						 BIT(3))
#define HL7015_REG_ISC_VINDPM_SHIFT		3
#define HL7015_REG_ISC_IINLIM_MASK		(BIT(2) | BIT(1) | BIT(0))
#define HL7015_REG_ISC_IINLIM_SHIFT		0
#define HL7015_REG_POC		0x01 /* Power-On Configuration */
#define HL7015_REG_POC_RESET_MASK		BIT(7)
#define HL7015_REG_POC_RESET_SHIFT		7
#define HL7015_REG_POC_WDT_RESET_MASK		BIT(6)
#define HL7015_REG_POC_WDT_RESET_SHIFT		6
#define HL7015_REG_POC_CHG_CONFIG_MASK		(BIT(5) | BIT(4))
#define HL7015_REG_POC_CHG_CONFIG_SHIFT	4
#define HL7015_REG_POC_CHG_CONFIG_DISABLE		0x0
#define HL7015_REG_POC_CHG_CONFIG_CHARGE		0x1
#define HL7015_REG_POC_CHG_CONFIG_OTG			0x2
#define HL7015_REG_POC_SYS_MIN_MASK		(BIT(3) | BIT(2) | BIT(1))
#define HL7015_REG_POC_SYS_MIN_SHIFT		1
#define HL7015_REG_POC_SYS_MIN_MIN			3000
#define HL7015_REG_POC_SYS_MIN_MAX			3700
#define HL7015_REG_POC_BOOST_LIM_MASK		BIT(0)
#define HL7015_REG_POC_BOOST_LIM_SHIFT		0
#define HL7015_REG_CCC		0x02 /* Charge Current Control */
#define HL7015_REG_CCC_ICHG_MASK		(BIT(7) | BIT(6) | BIT(5) | \
						 BIT(4) | BIT(3) | BIT(2))
#define HL7015_REG_CCC_ICHG_SHIFT		2
#define HL7015_REG_CCC_FORCE_20PCT_MASK	BIT(0)
#define HL7015_REG_CCC_FORCE_20PCT_SHIFT	0
#define HL7015_REG_PCTCC	0x03 /* Pre-charge/Termination Current Cntl */
#define HL7015_REG_PCTCC_IPRECHG_MASK		(BIT(7) | BIT(6) | BIT(5) | \
						 BIT(4))
#define HL7015_REG_PCTCC_IPRECHG_SHIFT		4
#define HL7015_REG_PCTCC_IPRECHG_MIN			128
#define HL7015_REG_PCTCC_IPRECHG_MAX			2048
#define HL7015_REG_PCTCC_ITERM_MASK		(BIT(3) | BIT(2) | BIT(1) | \
						 BIT(0))
#define HL7015_REG_PCTCC_ITERM_SHIFT		0
#define HL7015_REG_PCTCC_ITERM_MIN			128
#define HL7015_REG_PCTCC_ITERM_MAX			2048
#define HL7015_REG_CVC		0x04 /* Charge Voltage Control */
#define HL7015_REG_CVC_VREG_MASK		(BIT(7) | BIT(6) | BIT(5) | \
						 BIT(4) | BIT(3) | BIT(2))
#define HL7015_REG_CVC_VREG_SHIFT		2
#define HL7015_REG_CVC_BATLOWV_MASK		BIT(1)
#define HL7015_REG_CVC_BATLOWV_SHIFT		1
#define HL7015_REG_CVC_VRECHG_MASK		BIT(0)
#define HL7015_REG_CVC_VRECHG_SHIFT		0
#define HL7015_REG_CTTC	0x05 /* Charge Term/Timer Control */
#define HL7015_REG_CTTC_EN_TERM_MASK		BIT(7)
#define HL7015_REG_CTTC_EN_TERM_SHIFT		7
#define HL7015_REG_CTTC_TERM_STAT_MASK		BIT(6)
#define HL7015_REG_CTTC_TERM_STAT_SHIFT	6
#define HL7015_REG_CTTC_WATCHDOG_MASK		(BIT(5) | BIT(4))
#define HL7015_REG_CTTC_WATCHDOG_SHIFT		4
#define HL7015_REG_CTTC_EN_TIMER_MASK		BIT(3)
#define HL7015_REG_CTTC_EN_TIMER_SHIFT		3
#define HL7015_REG_CTTC_CHG_TIMER_MASK		(BIT(2) | BIT(1))
#define HL7015_REG_CTTC_CHG_TIMER_SHIFT	1
#define HL7015_REG_CTTC_JEITA_ISET_MASK	BIT(0)
#define HL7015_REG_CTTC_JEITA_ISET_SHIFT	0
#define HL7015_REG_ICTRC	0x0C /* IR Comp/Thermal Regulation Control */
#define HL7015_REG_ICTRC_BAT_COMP_MASK		(BIT(7) | BIT(6) | BIT(5))
#define HL7015_REG_ICTRC_BAT_COMP_SHIFT	5
#define HL7015_REG_ICTRC_VCLAMP_MASK		(BIT(4) | BIT(3) | BIT(2))
#define HL7015_REG_ICTRC_VCLAMP_SHIFT		2
#define HL7015_REG_THERMAL	0x06
#define HL7015_REG_ICTRC_TREG_MASK		(BIT(1) | BIT(0))
#define HL7015_REG_ICTRC_TREG_SHIFT		0
#define HL7015_REG_MOC		0x07 /* Misc. Operation Control */
#define HL7015_REG_MOC_DPDM_EN_MASK		BIT(7)
#define HL7015_REG_MOC_DPDM_EN_SHIFT		7
#define HL7015_REG_MOC_TMR2X_EN_MASK		BIT(6)
#define HL7015_REG_MOC_TMR2X_EN_SHIFT		6
#define HL7015_REG_MOC_BATFET_DISABLE_MASK	BIT(5)
#define HL7015_REG_MOC_BATFET_DISABLE_SHIFT	5
#define HL7015_REG_MOC_JEITA_VSET_MASK		BIT(4)
#define HL7015_REG_MOC_JEITA_VSET_SHIFT	4
#define HL7015_REG_MOC_INT_MASK_MASK		(BIT(1) | BIT(0))
#define HL7015_REG_MOC_INT_MASK_SHIFT		0
#define HL7015_REG_SS		0x08 /* System Status */
#define HL7015_REG_SS_VBUS_STAT_MASK		(BIT(7) | BIT(6))
#define HL7015_REG_SS_VBUS_STAT_SHIFT		6
#define HL7015_REG_SS_CHRG_STAT_MASK		(BIT(5) | BIT(4))
#define HL7015_REG_SS_CHRG_STAT_SHIFT		4
#define HL7015_REG_SS_DPM_STAT_MASK		BIT(3)
#define HL7015_REG_SS_DPM_STAT_SHIFT		3
#define HL7015_REG_SS_PG_STAT_MASK		BIT(2)
#define HL7015_REG_SS_PG_STAT_SHIFT		2
#define HL7015_REG_SS_THERM_STAT_MASK		BIT(1)
#define HL7015_REG_SS_THERM_STAT_SHIFT		1
#define HL7015_REG_SS_VSYS_STAT_MASK		BIT(0)
#define HL7015_REG_SS_VSYS_STAT_SHIFT		0
#define HL7015_REG_F		0x09 /* Fault */
#define HL7015_REG_F_WATCHDOG_FAULT_MASK	BIT(7)
#define HL7015_REG_F_WATCHDOG_FAULT_SHIFT	7
#define HL7015_REG_F_BOOST_FAULT_MASK		BIT(6)
#define HL7015_REG_F_BOOST_FAULT_SHIFT		6
#define HL7015_REG_F_CHRG_FAULT_MASK		(BIT(5) | BIT(4))
#define HL7015_REG_F_CHRG_FAULT_SHIFT		4
#define HL7015_REG_F_BAT_FAULT_MASK		BIT(3)
#define HL7015_REG_F_BAT_FAULT_SHIFT		3
#define HL7015_REG_F_NTC_FAULT_MASK		(BIT(2) | BIT(1) | BIT(0))
#define HL7015_REG_F_NTC_FAULT_SHIFT		0
#define HL7015_REG_VPRS	0x0A /* Vendor/Part/Revision Status */
#define HL7015_REG_VPRS_PN_MASK		(BIT(5) | BIT(4) | BIT(3))
#define HL7015_REG_VPRS_PN_SHIFT		3
#define HL7015_REG_VPRS_PN_24190			0x6//0x4
#define HL7015_REG_VPRS_PN_24192			0x5 /* Also 24193, 24196 */
#define HL7015_REG_VPRS_PN_24192I			0x3
#define HL7015_REG_VPRS_TS_PROFILE_MASK	BIT(2)
#define HL7015_REG_VPRS_TS_PROFILE_SHIFT	2
#define HL7015_REG_VPRS_DEV_REG_MASK		(BIT(1) | BIT(0))
#define HL7015_REG_VPRS_DEV_REG_SHIFT		0
#define HL7015_CON0		0x00
#define HL7015_CON1		0x01
#define HL7015_CON2		0x02
#define HL7015_CON3		0x03
#define HL7015_CON4		0x04
#define HL7015_CON5		0x05
#define HL7015_CON6		0x06
#define HL7015_CON7		0x07
#define HL7015_CON8		0x08
#define HL7015_CON9		0x09
#define HL7015_CONA		0x0A
#define HL7015_CONB		0x0B
#define HL7015_CONC		0x0C
#define HL7015_COND		0x0D
#define HL7015_CONE		0x0E
#define HL7015_CONF		0x0F
#define HL7015_CON10		0x10
#define HL7015_REG_NUM		0x10
/* CON0 */
#define CON0_EN_HIZ_MASK	0x01
#define CON0_EN_HIZ_SHIFT	7
#define CON0_VINDPM_MASK	0x0F
#define CON0_VINDPM_SHIFT	3
#define CON0_IINLIM_MASK	0x07
#define CON0_IINLIM_SHIFT	0
/* CON1 */
#define CON1_REG_RESET_MASK			0x01
#define CON1_REG_RESET_SHIFT		7
#define CON1_I2C_WDT_RESET_MASK		0x01
#define CON1_I2C_WDT_RESET_SHIFT	6
#define CON1_CHG_CONFIG_MASK		0x03
#define CON1_CHG_CONFIG_SHIFT		4
#define CON1_SYS_MIN_MASK			0x07
#define CON1_SYS_MIN_SHIFT			1
#define CON1_BOOST_LIM_MASK			0x01
#define CON1_BOOST_LIM_SHIFT		0
/* CON2 */
#define CON2_ICHG_MASK			0x3F
#define CON2_ICHG_SHIFT			2
#define CON2_BCLOD_MASK			0x01
#define CON2_BCLOD_SHIFT		1
#define CON2_FORCE_20PCT_MASK	0x01
#define CON2_FORCE_20PCT_SHIFT	0
/* CON3 */
#define CON3_IPRECHG_MASK	0x0F
#define CON3_IPRECHG_SHIFT	4
#define CON3_ITERM_MASK		0x0F
#define CON3_ITERM_SHIFT	0
/* CON4 */
#define CON4_VREG_MASK		0x3F
#define CON4_VREG_SHIFT		2
#define CON4_BATLOWV_MASK	0x01
#define CON4_BATLOWV_SHIFT	1
#define CON4_VRECHG_MASK	0x01
#define CON4_VRECHG_SHIFT	0
/* CON5 */
#define CON5_EN_TERM_MASK			0x01
#define CON5_EN_TERM_SHIFT			7
#define CON5_TERM_STAT_MASK			0x01
#define CON5_TERM_STAT_SHIFT		6
#define CON5_WATCHDOG_MASK			0x03
#define CON5_WATCHDOG_SHIFT			4
#define CON5_EN_SAFE_TIMER_MASK		0x01
#define CON5_EN_SAFE_TIMER_SHIFT	3
#define CON5_CHG_TIMER_MASK			0x03
#define CON5_CHG_TIMER_SHIFT		1
#define CON5_Reserved_MASK			0x01
#define CON5_Reserved_SHIFT			0
/* CON6 */
#define CON6_BOOSTV_MASK	0x0F
#define CON6_BOOSTV_SHIFT	4
#define CON6_BHOT_MASK		0x03
#define CON6_BHOT_SHIFT		2
#define CON6_TREG_MASK		0x03
#define CON6_TREG_SHIFT		0
/* CON7 */
#define CON7_DPDM_EN_MASK				0x01
#define CON7_DPDM_EN_SHIFT				7
#define CON7_TMR2X_EN_MASK				0x01
#define CON7_TMR2X_EN_SHIFT				6
#define CON7_PPFET_DISABLE_MASK			0x01
#define CON7_PPFET_DISABLE_SHIFT		5
//three bits were reserved
#define CON7_CHRG_FAULT_INT_MASK_MASK	0x01
#define CON7_CHRG_FAULT_INT_MASK_SHIFT	1
#define CON7_BAT_FAULT_INT_MASK_MASK	0x01
#define CON7_BAT_FAULT_INT_MASK_SHIFT	0
/* CON8 */
#define CON8_VIN_STAT_MASK		0x03
#define CON8_VIN_STAT_SHIFT		6
#define CON8_CHRG_STAT_MASK		0x03
#define CON8_CHRG_STAT_SHIFT	4
#define CON8_DPM_STAT_MASK		0x01
#define CON8_DPM_STAT_SHIFT		3
#define CON8_PG_STAT_MASK		0x01
#define CON8_PG_STAT_SHIFT		2
#define CON8_THERM_STAT_MASK	0x01
#define CON8_THERM_STAT_SHIFT	1
#define CON8_VSYS_STAT_MASK		0x01
#define CON8_VSYS_STAT_SHIFT	0
/* CON9 */
#define CON9_WATCHDOG_FAULT_MASK	0x01
#define CON9_WATCHDOG_FAULT_SHIFT	7
#define CON9_OTG_FAULT_MASK			0x01
#define CON9_OTG_FAULT_SHIFT		6
#define CON9_CHRG_FAULT_MASK		0x03
#define CON9_CHRG_FAULT_SHIFT		4
#define CON9_BAT_FAULT_MASK			0x01
#define CON9_BAT_FAULT_SHIFT		3
// one bit was reserved
#define CON9_NTC_FAULT_MASK			0x07
#define CON9_NTC_FAULT_SHIFT		0
/* CONA */
// vender info register
/* CONB */
#define CONB_TSR_MASK				0x03
#define CONB_TSR_SHIFT				6
#define CONB_TRSP_MASK				0x01
#define CONB_TRSP_SHIFT				5
#define CONB_DIS_RECONNECT_MASK		0x01
#define CONB_DIS_RECONNECT_SHIFT	4
#define CONB_DIS_SR_INCHG_MASK		0x01
#define CONB_DIS_SR_INCHG_SHIFT		3
#define CONB_TSHIP_MASK				0x07
#define CONB_TSHIP_SHIFT			0
/* CONC */
#define CONC_BAT_COMP_MASK			0x07
#define CONC_BAT_COMP_SHIFT			5
#define CONC_BAT_VCLAMP_MASK		0x07
#define CONC_BAT_VCLAMP_SHIFT		2
// one bit was reserved
#define CONC_BOOST_9V_EN_MASK		0x01
#define CONC_BOOST_9V_EN_SHIFT		0
/* COND */
// one bit was reserved
#define COND_DISABLE_TS_MASK		0x01
#define COND_DISABLE_TS_SHIFT		6
#define COND_VINDPM_OFFSET_MASK		0x03
#define COND_VINDPM_OFFSET_SHIFT	4
#define COND_VINDPM_OFFSET_X1		1
#define COND_VINDPM_OFFSET_X1P7		2
#define COND_VINDPM_OFFSET_X2P14	3
// five bits were reserved
#define R_VBUS_CHARGER_1   330
#define R_VBUS_CHARGER_2   39
/* CONE */
#define CONE_PPON_SR_EN_MASK            0x01
#define CONE_PPON_SR_EN_SHIFT           7
#define CONE_IPRE_ITERM_STEP_MASK       0x01
#define CONE_IPRE_ITERM_STEP_SHIFT      6
#define CONE_TSHIP_EXIT_MASK            0x01
#define CONE_TSHIP_EXIT_SHIFT           5
/* CONF */
#define CONF_FORCE_DP_MASK              0x03
#define CONF_FORCE_DP_SHIFT             6
#define CONF_FORCE_DM_MASK              0x03
#define CONF_FORCE_DM_SHIFT             4
#define CONF_HVDCP_DET_EN_MASK          0x01
#define CONF_HVDCP_DET_EN_SHIFT         3
#define CONF_RDM_100K_ON_MASK           0x01
#define CONF_RDM_100K_ON_SHIFT          2
#define CONF_INT_DMSTAT_MSK_MASK        0x01
#define CONF_INT_DMSTAT_MSK_SHIFT       1
/* CON10 */
#define CON10_VIN_STAT1_MASK            0x03
#define CON10_VIN_STAT1_SHIFT           6
#define CON10_DM_STAT_MASK              0x01
#define CON10_DM_STAT_SHIFT             5
#define VIN_STAT_UNKNOWN                0
#define VIN_STAT_USB_HOST               1
#define VIN_STAT_ADAPTER_PORT           2
#define VIN_STAT_OTG                    3
#define VIN_STAT1_USB_DCP               0
#define VIN_STAT1_USB_CDP               1
#define VIN_STAT1_NON_STD               2
#define VIN_STAT1_UNKNOWN               3

#define VBUS_MIN_UV                   4400000

enum {
	HL7015_DPDM_HIZ = 0,
	HL7015_DPDM_0V,
	HL7015_DPDM_0V6,
	HL7015_DPDM_3V3,
};

struct hl7015_platform_data {
	const struct regulator_init_data *regulator_init_data;
};

struct hl7015_cfg {
	int wd_time;
	int vbat_cv;
	int iterm;
	int vrechg;
};

static struct hl7015_cfg hl7015_default_cfg = {
	.wd_time = 0,
	.vbat_cv = 4480000,
	.iterm   = 512000,
	.vrechg  = 1,
};

/*
 * The FAULT register is latched by the hl7015 (except for NTC_FAULT)
 * so the first read after a fault returns the latched value and subsequent
 * reads return the current value.  In order to return the fault status
 * to the user, have the interrupt handler save the reg's value and retrieve
 * it in the appropriate health/status routine.
 */
struct hl7015_dev_info {
	struct i2c_client		*client;
	struct device			*dev;
	struct power_supply		*charger;
	struct delayed_work		input_current_limit_work;
	char				model_name[I2C_NAME_SIZE];
	bool				initialized;
	bool				irq_event;
	u16				sys_min;
	u16				iprechg;
	u16				iterm;
	struct mutex			f_reg_lock;
	u8				f_reg;
	u8				ss_reg;
	u8				watchdog;
	int psy_usb_type;
	int chg_type;
	struct power_supply_desc psy_desc;
	struct charger_device *chg_dev;
	struct power_supply *psy;
	struct charger_properties chg_props;
	struct alarm otg_kthread_gtimer;
	struct workqueue_struct *otg_boost_workq;
	struct work_struct kick_work;
	unsigned int polling_interval;
	bool polling_enabled;
	const char *chg_dev_name;
	const char *eint_name;
	u8 power_good;
	int irq;
	int is_hvdcp;
	struct iio_channel *vbus;
	struct hl7015_cfg cfg;
	struct delayed_work force_detect_dwork;
	atomic_t attached;
};
static struct i2c_client *new_client;
static int is_otg_mode = 0;
static DEFINE_MUTEX(hl7015_i2c_access);
static DEFINE_MUTEX(hl7015_access_lock);
/*
 * The tables below provide a 2-way mapping for the value that goes in
 * the register field and the real-world value that it represents.
 * The index of the array is the value that goes in the register; the
 * number at that index in the array is the real-world value that it
 * represents.
 */
/* REG06[1:0] (TREG) in tenths of degrees Celsius */
static const int hl7015_ictrc_treg_values[] = {
	600, 800, 1000, 1200
};
const unsigned int VBAT_CVTH[] = {
	3504000, 3520000, 3536000, 3552000,
	3568000, 3584000, 3600000, 3616000,
	3632000, 3648000, 3664000, 3680000,
	3696000, 3712000, 3728000, 3744000,
	3760000, 3776000, 3792000, 3808000,
	3824000, 3840000, 3856000, 3872000,
	3888000, 3904000, 3920000, 3936000,
	3952000, 3968000, 3984000, 4000000,
	4016000, 4032000, 4048000, 4064000,
	4080000, 4096000, 4112000, 4128000,
	4144000, 4160000, 4176000, 4192000,
	4208000, 4224000, 4240000, 4256000,
	4272000, 4288000, 4304000, 4320000,
	4336000, 4352000, 4368000, 4386000,
	4400000, 4416000, 4432000, 4448000,
	4464000, 4480000, 4496000, 4512000
};

const unsigned int VINDPM_NORMAL_CVTH[] = {
	3880000, 3960000, 4040000, 4120000,
	4200000, 4280000, 4360000, 4440000,
	4520000, 4600000, 4680000, 4760000,
	4840000, 4920000, 5000000, 5080000
};

const unsigned int VINDPM_MEDIUM_CVTH[] = {
	6596000, 6732000, 6868000, 7004000,
	7140000, 7276000, 7412000, 7548000,
	7684000, 7820000, 7956000, 8092000,
	8228000, 8364000, 8500000, 8636000
};

const unsigned int VINDPM_HIGH_CVTH[] = {
	8303200, 8474400, 8645600, 8816800,
	8988000, 9159200, 9330400, 9501600,
	9672800, 9844000, 10015200, 10186400,
	10357600, 10528800, 10700000, 10871200
};

const unsigned int SYS_MIN_CVTH[] = {
	3000000, 3100000, 3200000, 3300000,
	3400000, 3500000, 3600000, 3700000
};
const unsigned int BOOSTV_CVTH[] = {
	4550000, 4614000, 4678000, 4742000,
	4806000, 4870000, 4934000, 4998000,
	5062000, 5126000, 5190000, 5254000,
	5318000, 5382000, 5446000, 5510000
};
const unsigned int CSTH[] = {
	512000, 576000, 640000, 704000,
	768000, 832000, 896000, 960000,
	1024000, 1088000, 1152000, 1216000,
	1280000, 1344000, 1408000, 1472000,
	1536000, 1600000, 1664000, 1728000,
	1792000, 1856000, 1920000, 1984000,
	2048000, 2112000, 2176000, 2240000,
	2304000, 2368000, 2432000, 2496000,
	2560000, 2624000, 2688000, 2752000,
	2816000, 2880000, 2944000, 3008000,
	3072000, 3200000, 3264000, 3328000,
	3392000, 3456000, 3520000, 3584000,
	3648000, 3712000, 3776000, 3840000,
	3904000, 3968000, 4032000, 4096000,
	4160000, 4224000, 4288000, 4352000,
	4416000, 4480000, 4544000
};
/*HL7015 REG00 IINLIM[5:0]*/
const unsigned int INPUT_CSTH[] = {
	100000, 150000, 500000, 900000,
	1000000, 1500000, 2000000, 3000000
};
const unsigned int IPRE_CSTH[] = {
	128000, 256000, 384000, 512000,
	640000, 768000, 896000, 1024000,
	1152000, 1280000, 1408000, 1536000,
	1664000, 1792000, 1920000, 2048000
};
const unsigned int ITERM_CSTH[] = {
	128000, 256000, 384000, 512000,
	640000, 768000, 896000, 1024000,
	1152000, 1280000, 1408000, 1536000,
	1664000, 1792000, 1920000, 2048000
};
unsigned int charging_value_to_parameter_hl7015(const unsigned int *parameter, const unsigned int array_size,
					const unsigned int val)
{
	if (val < array_size)
		return parameter[val];
	pr_err("[hl7015] Can't find the parameter\n");
	return parameter[0];
}
unsigned int charging_parameter_to_value_hl7015(const unsigned int *parameter, const unsigned int array_size,
					const unsigned int val)
{
	unsigned int i;
	pr_err("[hl7015] array_size = %d\n", array_size);
	for (i = 0; i < array_size; i++) {
		if (val == *(parameter + i))
			return i;
	}
	pr_err("[hl7015] NO register value match\n");
	/* TODO: ASSERT(0);	// not find the value */
	return 0;
}
static unsigned int bmt_find_closest_level(const unsigned int *pList, unsigned int number,
					 unsigned int level)
{
	unsigned int i;
	unsigned int max_value_in_last_element;
	if (pList[0] < pList[1])
		max_value_in_last_element = 1;
	else
		max_value_in_last_element = 0;
	if (max_value_in_last_element == 1) {
		for (i = (number - 1); i != 0; i--) {	/* max value in the last element */
			if (pList[i] <= level) {
				pr_err("[hl7019d] zzf_%d<=%d, i=%d\n", pList[i], level, i);
				return pList[i];
			}
		}
		pr_err("[hl7019d] Can't find closest level\n");
		return pList[0];
		/* return 000; */
	} else {
		for (i = 0; i < number; i++) {	/* max value in the first element */
			if (pList[i] <= level)
				return pList[i];
		}
		pr_err("[hl7019d] Can't find closest level\n");
		return pList[number - 1];
		/* return 000; */
	}
}
/*
 * Return the index in 'tbl' of greatest value that is less than or equal to
 * 'val'.  The index range returned is 0 to 'tbl_size' - 1.  Assumes that
 * the values in 'tbl' are sorted from smallest to largest and 'tbl_size'
 * is less than 2^8.
 */
static u8 hl7015_find_idx(const int tbl[], int tbl_size, int v)
{
	int i;
	for (i = 1; i < tbl_size; i++)
		if (v < tbl[i])
			break;
	return i - 1;
}
/* Basic driver I/O routines */
static int hl7015_read(struct hl7015_dev_info *bdi, u8 reg, u8 *data)
{
	int ret;
	ret = i2c_smbus_read_byte_data(bdi->client, reg);
	if (ret < 0)
		return ret;
	*data = ret;
	return 0;
}
static int hl7015_write(struct hl7015_dev_info *bdi, u8 reg, u8 data)
{
	return i2c_smbus_write_byte_data(bdi->client, reg, data);
}
static int hl7015_read_mask(struct hl7015_dev_info *bdi, u8 reg,
		u8 mask, u8 shift, u8 *data)
{
	u8 v;
	int ret;
	ret = hl7015_read(bdi, reg, &v);
	if (ret < 0)
		return ret;
	v &= mask;
	v >>= shift;
	*data = v;
	return 0;
}
static int hl7015_write_mask(struct hl7015_dev_info *bdi, u8 reg,
		u8 mask, u8 shift, u8 data)
{
	u8 v;
	int ret;
	ret = hl7015_read(bdi, reg, &v);
	if (ret < 0)
		return ret;

	v &= ~mask;
	v |= ((data << shift) & mask);

	return hl7015_write(bdi, reg, v);
}
static int hl7015_get_field_val(struct hl7015_dev_info *bdi,
		u8 reg, u8 mask, u8 shift,
		const int tbl[], int tbl_size,
		int *val)
{
	u8 v;
	int ret;
	ret = hl7015_read_mask(bdi, reg, mask, shift, &v);
	if (ret < 0)
		return ret;
	v = (v >= tbl_size) ? (tbl_size - 1) : v;
	*val = tbl[v];
	return 0;
}
static int hl7015_set_field_val(struct hl7015_dev_info *bdi,
		u8 reg, u8 mask, u8 shift,
		const int tbl[], int tbl_size,
		int val)
{
	u8 idx;
	idx = hl7015_find_idx(tbl, tbl_size, val);
	return hl7015_write_mask(bdi, reg, mask, shift, idx);
}
static int __hl7015_read_byte(struct charger_device *chg_dev,u8 reg_addr, u8 *rd_buf, int rd_len)
{
	int ret;
	ret = hl7015_read(dev_get_drvdata(&chg_dev->dev), reg_addr, rd_buf);
	return ret;
}
static int hl7015_read_byte(struct charger_device *chg_dev,u8 reg_addr, u8 *rd_buf, int rd_len)
{
	int ret = 0;
	mutex_lock(&hl7015_i2c_access);
	ret = __hl7015_read_byte(chg_dev,reg_addr, rd_buf, rd_len);
	mutex_unlock(&hl7015_i2c_access);
	return ret;
}
int __hl7015_write_byte(struct charger_device *chg_dev,unsigned char reg_num, u8 *wr_buf, int wr_len)
{
	int ret = 0;
	ret = hl7015_write(dev_get_drvdata(&chg_dev->dev), reg_num, *wr_buf);
	return ret;
}
int hl7015_write_byte(struct charger_device *chg_dev,unsigned char reg_num, u8 *wr_buf, int wr_len)
{
	int ret = 0;
	mutex_lock(&hl7015_i2c_access);
	ret = __hl7015_write_byte(chg_dev,reg_num, wr_buf, wr_len);
	mutex_unlock(&hl7015_i2c_access);
	return ret;
}

unsigned int hl7015_read_interface(struct charger_device *chg_dev,unsigned char reg_num, unsigned char *val, unsigned char MASK,
				unsigned char SHIFT)
{
	unsigned char hl7015_reg = 0;
	unsigned int ret = 0;
	ret = hl7015_read_byte(chg_dev,reg_num, &hl7015_reg, 1);
	hl7015_reg &= (MASK << SHIFT);
	*val = (hl7015_reg >> SHIFT);
	return ret;
}

unsigned int hl7015_config_interface(struct charger_device *chg_dev,unsigned char reg_num, unsigned char val, unsigned char MASK,
					unsigned char SHIFT)
{
	unsigned char hl7015_reg = 0;
	unsigned char hl7015_reg_ori = 0;
	unsigned int ret = 0;
	mutex_lock(&hl7015_access_lock);
	ret = hl7015_read_byte(chg_dev,reg_num, &hl7015_reg, 1);
	if (ret) {
		dev_err(&(chg_dev->dev), "%s: read reg 0x%02x failed\n", __func__, reg_num);
		return ret;
	}
	hl7015_reg_ori = hl7015_reg;
	hl7015_reg &= ~(MASK << SHIFT);
	hl7015_reg |= (val << SHIFT);
	ret = hl7015_write_byte(chg_dev,reg_num, &hl7015_reg, 2);
	mutex_unlock(&hl7015_access_lock);
	dev_err(&(chg_dev->dev), "[hl7015_config_interface] write Reg[%x]=0x%x from 0x%x\n", reg_num,
			hl7015_reg, hl7015_reg_ori);
	return ret;
}

/* write one register directly */
unsigned int hl7015_reg_config_interface(struct charger_device *chg_dev,unsigned char reg_num, unsigned char val)
{
	unsigned char hl7015_reg = val;
	return hl7015_write_byte(chg_dev,reg_num, &hl7015_reg, 2);
}
#ifdef CONFIG_SYSFS
/*
 * There are a numerous options that are configurable on the hl7015
 * that go well beyond what the power_supply properties provide access to.
 * Provide sysfs access to them so they can be examined and possibly modified
 * on the fly.  They will be provided for the charger power_supply object only
 * and will be prefixed by 'f_' to make them easier to recognize.
 */
#define HL7015_SYSFS_FIELD(_name, r, f, m, store)			\
{									\
	.attr	= __ATTR(f_##_name, m, hl7015_sysfs_show, store),	\
	.reg	= HL7015_REG_##r,					\
	.mask	= HL7015_REG_##r##_##f##_MASK,				\
	.shift	= HL7015_REG_##r##_##f##_SHIFT,			\
}
#define HL7015_SYSFS_FIELD_RW(_name, r, f)				\
		HL7015_SYSFS_FIELD(_name, r, f, S_IWUSR | S_IRUGO,	\
				hl7015_sysfs_store)
#define HL7015_SYSFS_FIELD_RO(_name, r, f)				\
		HL7015_SYSFS_FIELD(_name, r, f, S_IRUGO, NULL)
static ssize_t hl7015_sysfs_show(struct device *dev,
		struct device_attribute *attr, char *buf);
static ssize_t hl7015_sysfs_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);
struct hl7015_sysfs_field_info {
	struct device_attribute	attr;
	u8	reg;
	u8	mask;
	u8	shift;
};
/* On i386 ptrace-abi.h defines SS that breaks the macro calls below. */
#undef SS
static struct hl7015_sysfs_field_info hl7015_sysfs_field_tbl[] = {
			/*	sysfs name	reg	field in reg */
	HL7015_SYSFS_FIELD_RW(en_hiz,		ISC,	EN_HIZ),
	HL7015_SYSFS_FIELD_RW(vindpm,		ISC,	VINDPM),
	HL7015_SYSFS_FIELD_RW(iinlim,		ISC,	IINLIM),
	HL7015_SYSFS_FIELD_RW(chg_config,	POC,	CHG_CONFIG),
	HL7015_SYSFS_FIELD_RW(sys_min,		POC,	SYS_MIN),
	HL7015_SYSFS_FIELD_RW(boost_lim,	POC,	BOOST_LIM),
	HL7015_SYSFS_FIELD_RW(ichg,		CCC,	ICHG),
	HL7015_SYSFS_FIELD_RW(force_20_pct,	CCC,	FORCE_20PCT),
	HL7015_SYSFS_FIELD_RW(iprechg,		PCTCC,	IPRECHG),
	HL7015_SYSFS_FIELD_RW(iterm,		PCTCC,	ITERM),
	HL7015_SYSFS_FIELD_RW(vreg,		CVC,	VREG),
	HL7015_SYSFS_FIELD_RW(batlowv,		CVC,	BATLOWV),
	HL7015_SYSFS_FIELD_RW(vrechg,		CVC,	VRECHG),
	HL7015_SYSFS_FIELD_RW(en_term,		CTTC,	EN_TERM),
	HL7015_SYSFS_FIELD_RW(term_stat,	CTTC,	TERM_STAT),
	HL7015_SYSFS_FIELD_RO(watchdog,	CTTC,	WATCHDOG),
	HL7015_SYSFS_FIELD_RW(en_timer,	CTTC,	EN_TIMER),
	HL7015_SYSFS_FIELD_RW(chg_timer,	CTTC,	CHG_TIMER),
	HL7015_SYSFS_FIELD_RW(jeta_iset,	CTTC,	JEITA_ISET),
	HL7015_SYSFS_FIELD_RW(bat_comp,	ICTRC,	BAT_COMP),
	HL7015_SYSFS_FIELD_RW(vclamp,		ICTRC,	VCLAMP),
	HL7015_SYSFS_FIELD_RW(treg,		ICTRC,	TREG),
	HL7015_SYSFS_FIELD_RW(dpdm_en,		MOC,	DPDM_EN),
	HL7015_SYSFS_FIELD_RW(tmr2x_en,	MOC,	TMR2X_EN),
	HL7015_SYSFS_FIELD_RW(batfet_disable,	MOC,	BATFET_DISABLE),
	HL7015_SYSFS_FIELD_RW(jeita_vset,	MOC,	JEITA_VSET),
	HL7015_SYSFS_FIELD_RO(int_mask,	MOC,	INT_MASK),
	HL7015_SYSFS_FIELD_RO(vbus_stat,	SS,	VBUS_STAT),
	HL7015_SYSFS_FIELD_RO(chrg_stat,	SS,	CHRG_STAT),
	HL7015_SYSFS_FIELD_RO(dpm_stat,	SS,	DPM_STAT),
	HL7015_SYSFS_FIELD_RO(pg_stat,		SS,	PG_STAT),
	HL7015_SYSFS_FIELD_RO(therm_stat,	SS,	THERM_STAT),
	HL7015_SYSFS_FIELD_RO(vsys_stat,	SS,	VSYS_STAT),
	HL7015_SYSFS_FIELD_RO(watchdog_fault,	F,	WATCHDOG_FAULT),
	HL7015_SYSFS_FIELD_RO(boost_fault,	F,	BOOST_FAULT),
	HL7015_SYSFS_FIELD_RO(chrg_fault,	F,	CHRG_FAULT),
	HL7015_SYSFS_FIELD_RO(bat_fault,	F,	BAT_FAULT),
	HL7015_SYSFS_FIELD_RO(ntc_fault,	F,	NTC_FAULT),
	HL7015_SYSFS_FIELD_RO(pn,		VPRS,	PN),
	HL7015_SYSFS_FIELD_RO(ts_profile,	VPRS,	TS_PROFILE),
	HL7015_SYSFS_FIELD_RO(dev_reg,		VPRS,	DEV_REG),
};
static struct attribute *
	hl7015_sysfs_attrs[ARRAY_SIZE(hl7015_sysfs_field_tbl) + 1];
ATTRIBUTE_GROUPS(hl7015_sysfs);
static void hl7015_sysfs_init_attrs(void)
{
	int i, limit = ARRAY_SIZE(hl7015_sysfs_field_tbl);
	for (i = 0; i < limit; i++)
		hl7015_sysfs_attrs[i] = &hl7015_sysfs_field_tbl[i].attr.attr;
	hl7015_sysfs_attrs[limit] = NULL; /* Has additional entry for this */
}
static struct hl7015_sysfs_field_info *hl7015_sysfs_field_lookup(
		const char *name)
{
	int i, limit = ARRAY_SIZE(hl7015_sysfs_field_tbl);
	for (i = 0; i < limit; i++)
		if (!strcmp(name, hl7015_sysfs_field_tbl[i].attr.attr.name))
			break;
	if (i >= limit)
		return NULL;
	return &hl7015_sysfs_field_tbl[i];
}
static ssize_t hl7015_sysfs_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct hl7015_dev_info *bdi = power_supply_get_drvdata(psy);
	struct hl7015_sysfs_field_info *info;
	ssize_t count;
	int ret;
	u8 v;
	info = hl7015_sysfs_field_lookup(attr->attr.name);
	if (!info)
		return -EINVAL;
	ret = pm_runtime_get_sync(bdi->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(bdi->dev);
		return ret;
	}
	ret = hl7015_read_mask(bdi, info->reg, info->mask, info->shift, &v);
	if (ret)
		count = ret;
	else
		count = scnprintf(buf, PAGE_SIZE, "%hhx\n", v);
	pm_runtime_mark_last_busy(bdi->dev);
	pm_runtime_put_autosuspend(bdi->dev);
	return count;
}
static ssize_t hl7015_sysfs_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct hl7015_dev_info *bdi = power_supply_get_drvdata(psy);
	struct hl7015_sysfs_field_info *info;
	int ret;
	u8 v;
	info = hl7015_sysfs_field_lookup(attr->attr.name);
	if (!info)
		return -EINVAL;
	ret = kstrtou8(buf, 0, &v);
	if (ret < 0)
		return ret;
	ret = pm_runtime_get_sync(bdi->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(bdi->dev);
		return ret;
	}
	ret = hl7015_write_mask(bdi, info->reg, info->mask, info->shift, v);
	if (ret)
		count = ret;
	pm_runtime_mark_last_busy(bdi->dev);
	pm_runtime_put_autosuspend(bdi->dev);
	return count;
}
#endif
void hl7015_set_iinlim(struct charger_device *chg_dev, unsigned int val)
{
	if(val > 7) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev, (unsigned char)(HL7015_CON0),
				(unsigned char)(val),
				(unsigned char)(CON0_IINLIM_MASK),
				(unsigned char)(CON0_IINLIM_SHIFT)
				);
}
/* CON0 */
static int hl7015_set_en_hiz(struct charger_device *chg_dev, bool en)
{
	if(!chg_dev)
		return -EINVAL;
	pr_err("[%s] en:%d\n", __func__, en);
	hl7015_config_interface(chg_dev, (unsigned char)(HL7015_CON0),
				(unsigned char)(en),
				(unsigned char)(CON0_EN_HIZ_MASK),
				(unsigned char)(CON0_EN_HIZ_SHIFT)
				);
	return 0;
}
static int hl7015_get_adc(struct charger_device *chg_dev,
		enum adc_channel chan, int *min, int *max)
{
        int ret, value;
	struct hl7015_dev_info *bdi = dev_get_drvdata(&chg_dev->dev);
	switch (chan) {
	case ADC_CHANNEL_VBUS:
		ret = iio_read_channel_processed(bdi->vbus, &value);
		if (ret < 0) {
			dev_err(bdi->dev, "get vbus voltage failed");
			return -EINVAL;
		}
		*min = value + R_VBUS_CHARGER_1 * value / R_VBUS_CHARGER_2;
		dev_info(bdi->dev, "vbus voltage: %d", *min);
		break;
	case ADC_CHANNEL_VSYS:
		*min = 4000;
		break;
	default:
		return -95;
	}
	*min = *min * 1000;
	*max = *min;
	return 0;
}
static int hl7015_get_vbus(struct charger_device *chgdev, u32 *vbus)
{
        int ret, value;
        struct hl7015_dev_info *bdi = dev_get_drvdata(&chgdev->dev);

        ret = iio_read_channel_processed(bdi->vbus, &value);
	if (ret < 0) {
		dev_err(bdi->dev, "get vbus voltage failed");
		return -EINVAL;
	}
	*vbus = value + R_VBUS_CHARGER_1 * value / R_VBUS_CHARGER_2;
	*vbus = *vbus * 1000;
	dev_info(bdi->dev, "vbus voltage: %d", *vbus);
	return ret;
}
void hl7015_set_vindpm(struct charger_device *chg_dev, unsigned int val)
{
	if(val > 15) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON0),
				(unsigned char)(val),
				(unsigned char)(CON0_VINDPM_MASK),
				(unsigned char)(CON0_VINDPM_SHIFT)
				);
}
/* CON1 */
void hl7015_set_register_reset(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON1),
				(unsigned char)(val),
				(unsigned char)(CON1_REG_RESET_MASK),
				(unsigned char)(CON1_REG_RESET_SHIFT)
				);
}
void hl7015_set_i2cwatchdog_timer_reset(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev, (unsigned char)(HL7015_CON1),
				(unsigned char)(val),
				(unsigned char)(CON1_I2C_WDT_RESET_MASK),
				(unsigned char)(CON1_I2C_WDT_RESET_SHIFT)
				);
}
void hl7015_set_chg_config(struct charger_device *chg_dev, unsigned int val)
{
	if(val > 3) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev, (unsigned char)(HL7015_CON1),
				(unsigned char)(val),
				(unsigned char)(CON1_CHG_CONFIG_MASK),
				(unsigned char)(CON1_CHG_CONFIG_SHIFT)
				);
}
void hl7015_set_sys_min(struct charger_device *chg_dev, unsigned int val)
{
	if(val > 7) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev, (unsigned char)(HL7015_CON1),
				(unsigned char)(val),
				(unsigned char)(CON1_SYS_MIN_MASK),
				(unsigned char)(CON1_SYS_MIN_SHIFT)
				);
}
void hl7015_set_boost_lim(struct charger_device *chg_dev, unsigned int val)
{
	if(val > 7) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev, (unsigned char)(HL7015_CON1),
				(unsigned char)(val),
				(unsigned char)(CON1_BOOST_LIM_MASK),
				(unsigned char)(CON1_BOOST_LIM_SHIFT)
				);
}
/* CON2 */
void hl7015_set_ichg(struct charger_device *chg_dev, unsigned int val)
{
	if(val > 62) { //hhl modify
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev, (unsigned char)(HL7015_CON2),
				(unsigned char)(val),
				(unsigned char)(CON2_ICHG_MASK),
				(unsigned char)(CON2_ICHG_SHIFT)
				);
}
void hl7015_set_bcold(struct charger_device *chg_dev, unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev, (unsigned char)(HL7015_CON2),
				(unsigned char)(val),
				(unsigned char)(CON2_BCLOD_MASK),
				(unsigned char)(CON2_BCLOD_SHIFT)
				);
}
void hl7015_set_force_20pct(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON2),
				(unsigned char)(val),
				(unsigned char)(CON2_FORCE_20PCT_MASK),
				(unsigned char)(CON2_FORCE_20PCT_SHIFT)
				);
}
/* CON3 */
void hl7015_set_iprechg(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 15) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON3),
				(unsigned char)(val),
				(unsigned char)(CON3_IPRECHG_MASK),
				(unsigned char)(CON3_IPRECHG_SHIFT)
				);
}
void hl7015_set_iterm(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 15) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON3),
				(unsigned char)(val),
				(unsigned char)(CON3_ITERM_MASK),
				(unsigned char)(CON3_ITERM_SHIFT)
				);
}
/* CON4 */
void hl7015_set_vreg(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 63) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON4),
				(unsigned char)(val),
				(unsigned char)(CON4_VREG_MASK),
				(unsigned char)(CON4_VREG_SHIFT)
				);
}
void hl7015_set_batlowv(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}

	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON4),
				(unsigned char)(val),
				(unsigned char)(CON4_BATLOWV_MASK),
				(unsigned char)(CON4_BATLOWV_SHIFT)
				);
}
void hl7015_set_vrechg(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON4),
				(unsigned char)(val),
				(unsigned char)(CON4_VRECHG_MASK),
				(unsigned char)(CON4_VRECHG_SHIFT)
				);
}
/* CON5 */
void hl7015_set_en_term(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON5),
				(unsigned char)(val),
				(unsigned char)(CON5_EN_TERM_MASK),
				(unsigned char)(CON5_EN_TERM_SHIFT)
				);
}
void hl7015_set_term_stat(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON5),
				(unsigned char)(val),
				(unsigned char)(CON5_TERM_STAT_MASK),
				(unsigned char)(CON5_TERM_STAT_SHIFT)
				);
}
void hl7015_set_i2cwatchdog(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 3) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON5),
				(unsigned char)(val),
				(unsigned char)(CON5_WATCHDOG_MASK),
				(unsigned char)(CON5_WATCHDOG_SHIFT)
				);
}
void hl7015_set_en_safty_timer(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON5),
				(unsigned char)(val),
				(unsigned char)(CON5_EN_SAFE_TIMER_MASK),
				(unsigned char)(CON5_EN_SAFE_TIMER_SHIFT)
				);
}
void hl7015_set_charge_timer(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 3) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON5),
				(unsigned char)(val),
				(unsigned char)(CON5_CHG_TIMER_MASK),
				(unsigned char)(CON5_CHG_TIMER_SHIFT)
				);
}
/* CON6 */
void hl7015_set_boostv(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 15) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON6),
				(unsigned char)(val),
				(unsigned char)(CON6_BOOSTV_MASK),
				(unsigned char)(CON6_BOOSTV_SHIFT)
				);
}
void hl7015_set_bhot(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 3) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON6),
				(unsigned char)(val),
				(unsigned char)(CON6_BHOT_MASK),
				(unsigned char)(CON6_BHOT_SHIFT)
				);
}
void hl7015_set_treg(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 3) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON6),
				(unsigned char)(val),
				(unsigned char)(CON6_TREG_MASK),
				(unsigned char)(CON6_TREG_SHIFT)
				);
}

/* CON7 */
static int hl7015_set_dpdm_en(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return -1;
	}
	return hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON7),
				(unsigned char)(val),
				(unsigned char)(CON7_DPDM_EN_MASK),
				(unsigned char)(CON7_DPDM_EN_SHIFT));
}

void hl7015_set_tmr2x_en(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON7),
				(unsigned char)(val),
				(unsigned char)(CON7_TMR2X_EN_MASK),
				(unsigned char)(CON7_TMR2X_EN_SHIFT)
				);
}
void hl7015_set_ppfet_disable(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON7),
				(unsigned char)(val),
				(unsigned char)(CON7_PPFET_DISABLE_MASK),
				(unsigned char)(CON7_PPFET_DISABLE_SHIFT)
				);
}
void hl7015_set_chrgfault_int_mask(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON7),
				(unsigned char)(val),
				(unsigned char)(CON7_CHRG_FAULT_INT_MASK_MASK),
				(unsigned char)(CON7_CHRG_FAULT_INT_MASK_SHIFT)
				);
}
void hl7015_set_batfault_int_mask(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CON7),
				(unsigned char)(val),
				(unsigned char)(CON7_BAT_FAULT_INT_MASK_MASK),
				(unsigned char)(CON7_BAT_FAULT_INT_MASK_SHIFT)
				);
}
/* CON8 */
static unsigned int hl7015_get_vin_status(struct charger_device *chg_dev)
{
	unsigned int ret;
	unsigned char val;
	ret = hl7015_read_interface(chg_dev,(unsigned char)(HL7015_CON8),
			&val,
			(unsigned char)(CON8_VIN_STAT_MASK),
			(unsigned char)(CON8_VIN_STAT_SHIFT)
			);
	return val;
}
static void hl7015_vin_status_dump(struct charger_device *chg_dev)
{
	unsigned int vin_status;
	vin_status = hl7015_get_vin_status(chg_dev);
	switch(vin_status)
	{
		case 0:
			pr_err("[hl7015] no input or dpm detection incomplete.\n");
			break;
		case 1:
			pr_err("[hl7015] USB host inserted.\n");
			break;
		case 2:
			pr_err("[hl7015] Adapter inserted.\n");
			break;
		case 3:
			pr_err("[hl7015] OTG device inserted.\n");
			break;
		default:
			pr_err("[hl7015] wrong vin status.\n");
			break;
	}
}
static unsigned int hl7015_get_chrg_status(struct charger_device *chg_dev)
{
	unsigned int ret;
	unsigned char val;
	ret = hl7015_read_interface(chg_dev,(unsigned char)(HL7015_CON8),
			&val,
			(unsigned char)(CON8_CHRG_STAT_MASK),
			(unsigned char)(CON8_CHRG_STAT_SHIFT)
			);
	return val;
}
static void hl7015_chrg_status_dump(struct charger_device *chg_dev)
{
	unsigned int chrg_status;
	chrg_status = hl7015_get_chrg_status(chg_dev);
	switch(chrg_status)
	{
		case 0:
			pr_err("[hl7015] not charging.\n");
			break;
		case 1:
			pr_err("[hl7015] precharging mode.\n");
			break;
		case 2:
			pr_err("[hl7015] fast charging mode.\n");
			break;
		case 3:
			pr_err("[hl7015] charge termination done.\n");
			break;
		default:
			pr_err("[hl7015] wrong charge status.\n");
			break;
	}
}
static unsigned int hl7015_get_dpm_status(struct charger_device *chg_dev)
{
	unsigned int ret;
	unsigned char val;
	ret = hl7015_read_interface(chg_dev,(unsigned char)(HL7015_CON8),
			&val,
			(unsigned char)(CON8_DPM_STAT_MASK),
			(unsigned char)(CON8_DPM_STAT_SHIFT)
			);
	return val;
}
static void hl7015_dpm_status_dump(struct charger_device *chg_dev)
{
	unsigned int dpm_status;
	dpm_status = hl7015_get_dpm_status(chg_dev);
	if(0x0 == dpm_status)
		pr_err("[hl7015] not in dpm.\n");
	else
		pr_err("[hl7015] in vindpm or ilimdpm.\n");
}
static unsigned int hl7015_get_pg_status(struct charger_device *chg_dev)
{
	unsigned int ret;
	unsigned char val;
	ret = hl7015_read_interface(chg_dev,(unsigned char)(HL7015_CON8),
			&val,
			(unsigned char)(CON8_PG_STAT_MASK),
			(unsigned char)(CON8_PG_STAT_SHIFT)
			);
	return val;
}
static void hl7015_pg_status_dump(struct charger_device *chg_dev)
{
	unsigned int pg_status;
	pg_status = hl7015_get_pg_status(chg_dev);
	if(0x0 == pg_status)
		pr_err("[hl7015] power is not good.\n");
	else
		pr_err("[hl7015] power is good.\n");
}
static unsigned int hl7015_get_therm_status(struct charger_device *chg_dev)
{
	unsigned int ret;
	unsigned char val;
	ret = hl7015_read_interface(chg_dev,(unsigned char)(HL7015_CON8),
			&val,
			(unsigned char)(CON8_THERM_STAT_MASK),
			(unsigned char)(CON8_THERM_STAT_SHIFT)
			);
	return val;
}
static void hl7015_therm_status_dump(struct charger_device *chg_dev)
{
	unsigned int therm_status;
	therm_status = hl7015_get_therm_status(chg_dev);
	if(0x0 == therm_status)
		pr_err("[hl7015] ic's thermal status is in normal.\n");
	else
		pr_err("[hl7015] ic is in thermal regulation.\n");
}
static unsigned int hl7015_get_vsys_status(struct charger_device *chg_dev)
{
	unsigned int ret;
	unsigned char val;
	ret = hl7015_read_interface(chg_dev, (unsigned char)(HL7015_CON8),
			&val,
			(unsigned char)(CON8_VSYS_STAT_MASK),
			(unsigned char)(CON8_VSYS_STAT_SHIFT)
			);
	return val;
}
static void hl7015_vsys_status_dump(struct charger_device *chg_dev)
{
	unsigned int vsys_status;
	vsys_status = hl7015_get_vsys_status(chg_dev);
	if(0x0 == vsys_status)
		pr_err("[hl7015] ic is not in vsysmin regulation(BAT > VSYSMIN).\n");
	else
		pr_err("[hl7015] ic is in vsysmin regulation(BAT < VSYSMIN).\n");
}
static void hl7015_charger_system_status(struct charger_device *chg_dev)
{
	hl7015_vin_status_dump(chg_dev);
	hl7015_chrg_status_dump(chg_dev);
	hl7015_dpm_status_dump(chg_dev);
	hl7015_pg_status_dump(chg_dev);
	hl7015_therm_status_dump(chg_dev);
	hl7015_vsys_status_dump(chg_dev);
}
/* CON9 */
static unsigned int hl7015_get_watchdog_fault(struct charger_device *chg_dev)
{
	unsigned int ret;
	unsigned char val;
	ret = hl7015_read_interface(chg_dev,(unsigned char)(HL7015_CON9),
			&val,
			(unsigned char)(CON9_WATCHDOG_FAULT_MASK),
			(unsigned char)(CON9_WATCHDOG_FAULT_SHIFT)
			);
	return val;
}
static void hl7015_watchdog_fault_dump(struct charger_device *chg_dev)
{
	unsigned int wtd_fault;
	wtd_fault = hl7015_get_watchdog_fault(chg_dev);
	if(0x0 == wtd_fault)
		pr_err("[hl7015] i2c watchdog is normal.\n");
	else
		pr_err("[hl7015] i2c watchdog timer is expirate.\n");
}
static unsigned int hl7015_get_otg_fault(struct charger_device *chg_dev)
{
	unsigned int ret;
	unsigned char val;
	ret = hl7015_read_interface(chg_dev,(unsigned char)(HL7015_CON9),
			&val,
			(unsigned char)(CON9_OTG_FAULT_MASK),
			(unsigned char)(CON9_OTG_FAULT_SHIFT)
			);
	return val;
}
static void hl7015_otg_fault_dump(struct charger_device *chg_dev)
{
	unsigned int otg_fault;
	otg_fault = hl7015_get_otg_fault(chg_dev);
	if(0x0 == otg_fault)
		pr_err("[hl7015] the OTG function is fine.\n");
	else
		pr_err("[hl7015] the OTG function is error.\n");
}
static unsigned int hl7015_get_chrg_fault(struct charger_device *chg_dev)
{
	unsigned int ret;
	unsigned char val;
	ret = hl7015_read_interface(chg_dev,(unsigned char)(HL7015_CON9),
			&val,
			(unsigned char)(CON9_CHRG_FAULT_MASK),
			(unsigned char)(CON9_CHRG_FAULT_SHIFT)
			);
	return val;
}
static void hl7015_chrg_fault_dump(struct charger_device *chg_dev)
{
	unsigned int chrg_fault;
	chrg_fault = hl7015_get_chrg_fault(chg_dev);
	switch(chrg_fault)
	{
		case 0:
			pr_err("[hl7015] the ic charging status is normal.\n");
			break;
		case 1:
			pr_err("[hl7015] the ic's input is fault.\n");
			break;
		case 2:
			pr_err("[hl7015] the ic's thermal is shutdown.\n");
			break;
		case 3:
			pr_err("[hl7015] the ic's charge safety timer is expirate.\n");
			break;
		default:
			pr_err("[hl7015] the ic's charge fault status is unkown.\n");
			break;
	}
}
static unsigned int hl7015_get_ntc_fault(struct charger_device *chg_dev)
{
	unsigned int ret;
	unsigned char val;
	ret = hl7015_read_interface(chg_dev,(unsigned char)(HL7015_CON9),
			&val,
			(unsigned char)(CON9_NTC_FAULT_MASK),
			(unsigned char)(CON9_NTC_FAULT_SHIFT)
			);
	return val;
}
static void hl7015_ntc_fault_dump(struct charger_device *chg_dev)
{
	unsigned int ntc_fault;
	ntc_fault = hl7015_get_ntc_fault(chg_dev);
	switch(ntc_fault)
	{
		case 0:
			pr_err("[hl7015] the ic's body temperature is normal.\n");
			break;
		case 5:
			pr_err("[hl7015] the ic's body temperature is cold.\n");
			break;
		case 6:
			pr_err("[hl7015] the ic's body temperature is hot.\n");
			break;
		default:
			pr_err("[hl7015] the ic's body temperature is unknow.\n");
			break;
	}
}
static void hl7015_charger_fault_status(struct charger_device *chg_dev)
{
	hl7015_watchdog_fault_dump(chg_dev);
	hl7015_otg_fault_dump(chg_dev);
	hl7015_chrg_fault_dump(chg_dev);
	hl7015_ntc_fault_dump(chg_dev);
}
/* CONA */
//vender info register
/* CONB */
void hl7015_set_tsr(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 3) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CONB),
				(unsigned char)(val),
				(unsigned char)(CONB_TSR_MASK),
				(unsigned char)(CONB_TSR_SHIFT)
				);
}
void hl7015_set_tsrp(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CONB),
				(unsigned char)(val),
				(unsigned char)(CONB_TRSP_MASK),
				(unsigned char)(CONB_TRSP_SHIFT)
				);
}
void hl7015_set_dis_connect(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CONB),
				(unsigned char)(val),
				(unsigned char)(CONB_DIS_RECONNECT_MASK),
				(unsigned char)(CONB_DIS_RECONNECT_SHIFT)
				);
}
void hl7015_set_dis_sr_inchg(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CONB),
				(unsigned char)(val),
				(unsigned char)(CONB_DIS_SR_INCHG_MASK),
				(unsigned char)(CONB_DIS_SR_INCHG_SHIFT)
				);
}
void hl7015_set_tship(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 7) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CONB),
				(unsigned char)(val),
				(unsigned char)(CONB_TSHIP_MASK),
				(unsigned char)(CONB_TSHIP_SHIFT)
				);
}
/* CONC */
void hl7015_set_bat_comp(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 7) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CONC),
				(unsigned char)(val),
				(unsigned char)(CONC_BAT_COMP_MASK),
				(unsigned char)(CONC_BAT_COMP_SHIFT)
				);
}
void hl7015_set_bat_vclamp(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 7) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CONC),
				(unsigned char)(val),
				(unsigned char)(CONC_BAT_VCLAMP_MASK),
				(unsigned char)(CONC_BAT_VCLAMP_SHIFT)
				);
}
void hl7015_set_boost_9v_en(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CONC),
				(unsigned char)(val),
				(unsigned char)(CONC_BOOST_9V_EN_MASK),
				(unsigned char)(CONC_BOOST_9V_EN_SHIFT)
				);
}
/* COND */
void hl7015_set_disable_ts(struct charger_device *chg_dev,unsigned int val)
{
	if(val > 1) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_COND),
				(unsigned char)(val),
				(unsigned char)(COND_DISABLE_TS_MASK),
				(unsigned char)(COND_DISABLE_TS_SHIFT)
				);
}
void hl7015_set_vindpm_offset(struct charger_device *chg_dev,unsigned int val)
{
	if (val > COND_VINDPM_OFFSET_X2P14) {
		pr_err("[%s] parameter error.\n", __func__);
		return;
	}
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_COND),
				(unsigned char)(val),
				(unsigned char)(COND_VINDPM_OFFSET_MASK),
				(unsigned char)(COND_VINDPM_OFFSET_SHIFT)
				);
}

void hl7015_set_dp_voltage(struct charger_device *chg_dev,unsigned int val)
{
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CONF),
				(unsigned char)(val),
				(unsigned char)(CONF_FORCE_DP_MASK),
				(unsigned char)(CONF_FORCE_DP_SHIFT)
				);
}

void hl7015_set_dm_voltage(struct charger_device *chg_dev,unsigned int val)
{
	hl7015_config_interface(chg_dev,(unsigned char)(HL7015_CONF),
				(unsigned char)(val),
				(unsigned char)(CONF_FORCE_DM_MASK),
				(unsigned char)(CONF_FORCE_DM_SHIFT)
				);
}

void hl7015_set_en_hvdcp(struct charger_device *chg_dev, bool en)
{
	hl7015_config_interface(chg_dev, (unsigned char)(HL7015_CONF),
				(unsigned char)(en),
				(unsigned char)(CONF_HVDCP_DET_EN_MASK),
				(unsigned char)(CONF_HVDCP_DET_EN_SHIFT)
				);
}

void hl7015_set_en_int_dmstat(struct charger_device *chg_dev, bool en)
{
	hl7015_config_interface(chg_dev, (unsigned char)(HL7015_CONF),
				(unsigned char)(en),
				(unsigned char)(CONF_INT_DMSTAT_MSK_MASK),
				(unsigned char)(CONF_INT_DMSTAT_MSK_SHIFT)
				);
}

void hl7015_start_hvdcp(struct charger_device *chg_dev)
{
	hl7015_set_en_int_dmstat(chg_dev, false);
	hl7015_set_dp_voltage(chg_dev, HL7015_DPDM_0V6);
	hl7015_set_dm_voltage(chg_dev, HL7015_DPDM_HIZ);
	hl7015_set_en_hvdcp(chg_dev, true);
}

static int hl7015_enable_charging(struct charger_device *chg_dev, bool en)
{
	unsigned int status = 0;
	pr_err("[%s] en:%d\n", __func__, en);
	if(!chg_dev)
		return -EINVAL;
	if (is_otg_mode)
	{
		pr_err("[hl7015]%s: OTG mode,do not setting charging\n", __func__);
		return status;
	}
	if (en) {
		hl7015_set_chg_config(chg_dev, 0x1);
	} else {
		hl7015_set_chg_config(chg_dev, 0x0);
	}
	return status;
}
static int hl7015_get_current(struct charger_device *chg_dev, u32 *ichg)
{
	int status = 0;
	unsigned int array_size;
	unsigned char reg_value;
	if(!chg_dev)
		return -EINVAL;
	array_size = ARRAY_SIZE(CSTH);
	hl7015_read_interface(chg_dev,HL7015_CON2, &reg_value, CON2_ICHG_MASK, CON2_ICHG_SHIFT);	/* charge current */
	*ichg = charging_value_to_parameter_hl7015(CSTH, array_size, reg_value);
	return status;
}
static int hl7015_set_current(struct charger_device *chg_dev, u32 current_value)
{
	unsigned int status = 0;
	unsigned int set_chr_current;
	unsigned int array_size;
	unsigned char register_value;
	u8 reg_val;

    pr_err("endy,%s()++\n",__func__);

    pr_err("endy,%s()current_value = %d.\n",__func__,current_value);
	if(!chg_dev)
		return -EINVAL;
	if (is_otg_mode)
	{
		pr_err("[hl7015]%s: OTG mode,do not setting charging\n", __func__);
		return status;
	}
	pr_err("[hl7015] charge current setting value: %d.\n", current_value);
	if (current_value <= 500000) {
		hl7015_set_ichg(chg_dev, 0x0);
	} else {
		array_size = ARRAY_SIZE(CSTH);
		set_chr_current = bmt_find_closest_level(CSTH, array_size, current_value);
		pr_err("[hl7015] charge current finally setting value: %d.\n", set_chr_current);
		register_value = charging_parameter_to_value_hl7015(CSTH, array_size, set_chr_current);
		hl7015_set_ichg(chg_dev, register_value);
	}

	hl7015_read_byte(chg_dev, 2, &reg_val, 1);
    pr_err("endy,%s()--\n",__func__);
	return status;
}
static int hl7015_get_cv_voltage(struct charger_device *chg_dev, u32 *cv)
{
	int status = 0;
	unsigned int array_size;
	unsigned char reg_value;
	if(!chg_dev)
		return -EINVAL;
	array_size = ARRAY_SIZE(VBAT_CVTH);
	hl7015_read_interface(chg_dev,HL7015_CON4, &reg_value, CON4_VREG_MASK, CON4_VREG_SHIFT);
	*cv = charging_value_to_parameter_hl7015(VBAT_CVTH, array_size, reg_value);
	return status;
}
static int hl7015_set_cv_voltage(struct charger_device *chg_dev, u32 cv)
{
	int status = 0;
	unsigned short int array_size;
	unsigned int set_cv_voltage;
	unsigned char  register_value;
	if(!chg_dev)
		return -EINVAL;
	if (is_otg_mode)
	{
		pr_err("[hl7015]%s: OTG mode,do not setting charging\n", __func__);
		return status;
	}
	pr_err("[hl7015] charge voltage setting value: %d.\n", cv);
	/*static kal_int16 pre_register_value; */
	array_size = ARRAY_SIZE(VBAT_CVTH);
	/*pre_register_value = -1; */
	set_cv_voltage = bmt_find_closest_level(VBAT_CVTH, array_size, cv);
	register_value =
	charging_parameter_to_value_hl7015(VBAT_CVTH, array_size, set_cv_voltage);
	pr_err("[hl7015] charging_set_cv_voltage register_value=0x%x %d %d\n",
	 register_value, cv, set_cv_voltage);
	hl7015_set_vreg(chg_dev,register_value);
	return status;
}
static int hl7015_get_input_current(struct charger_device *chg_dev, u32 *aicr)
{
	unsigned int status = 0;
	unsigned int array_size;
	unsigned char register_value;

	if(!chg_dev)
		return -EINVAL;
	array_size = ARRAY_SIZE(INPUT_CSTH);
	hl7015_read_interface(chg_dev,HL7015_CON0, &register_value, CON0_IINLIM_MASK, CON0_IINLIM_SHIFT);
	*aicr = charging_value_to_parameter_hl7015(INPUT_CSTH, array_size, register_value);
	return status;
}
static int hl7015_set_input_current(struct charger_device *chg_dev, u32 current_value)
{
	unsigned int status = 0;
	unsigned int set_chr_current;
	unsigned int array_size;
	unsigned char register_value;

	pr_err("%s(),input current = %d.\n",__func__,current_value);
	if(!chg_dev)
		return -EINVAL;
	if (is_otg_mode)
	{
		pr_err("[hl7015]%s: OTG mode,do not setting charging\n", __func__);
		return status;
	}
	if (current_value < 100000) {
		register_value = 0x0;
	} else {
		array_size = ARRAY_SIZE(INPUT_CSTH);
		set_chr_current = bmt_find_closest_level(INPUT_CSTH, array_size, current_value);
		pr_err("[hl7015] charge input current finally setting value: %d.\n", set_chr_current);
		register_value = charging_parameter_to_value_hl7015(INPUT_CSTH, array_size, set_chr_current);
	}
	hl7015_set_iinlim(chg_dev, register_value);
    pr_err("endy,%s()--\n",__func__);
	return status;
}
static int hl7015_get_termination_curr(struct charger_device *chg_dev, u32 *term_curr)
{
	unsigned int status = 0;
	unsigned int array_size;
	unsigned char register_value;
	if(!chg_dev)
		return -EINVAL;
	array_size = ARRAY_SIZE(ITERM_CSTH);
	hl7015_read_interface(chg_dev,HL7015_CON3, &register_value, CON3_ITERM_MASK, CON3_ITERM_SHIFT);
	*term_curr = charging_parameter_to_value_hl7015(ITERM_CSTH, array_size, register_value);
	return status;
}
static int hl7015_set_termination_curr(struct charger_device *chg_dev, u32 term_curr)
{
	unsigned int status = 0;
	unsigned int set_term_current;
	unsigned int array_size;
	unsigned int register_value = 0;
	if(!chg_dev)
		return -EINVAL;
	pr_err("[hl7015] charge termination current setting value: %d.\n", term_curr);
	if(term_curr < 100000) {
		hl7015_set_iterm(chg_dev,0x0);
	} else {
		array_size = ARRAY_SIZE(ITERM_CSTH);
		set_term_current = bmt_find_closest_level(ITERM_CSTH, array_size, term_curr);
		pr_err("[hl7015] charge termination current finally setting value: %d.\n", set_term_current);
		register_value = charging_parameter_to_value_hl7015(ITERM_CSTH, array_size, set_term_current);
	}
	hl7015_set_iterm(chg_dev,register_value);
	return status;
}
/* charger operation's functions */
static int hl7015_dump_register(struct charger_device *chg_dev)
{
	unsigned int status = 0;
	int i;
	unsigned char reg_val;
	if(!chg_dev)
		return -EINVAL;
	dev_err(&(chg_dev->dev), "[hl7015] dump info:\n");
	for(i = 0; i < HL7015_REG_NUM + 1; i++)
	{
		hl7015_read_byte(chg_dev,i, &reg_val, 1);
		dev_err(&(chg_dev->dev), "hl7015_reg[%d] = 0x%x\n", i, reg_val);
	}
	return status;
}
static int hl7015_reset_watch_dog_timer(struct charger_device *chg_dev)
{
	unsigned int status = 0;
	if(!chg_dev)
		return -EINVAL;
	hl7015_set_i2cwatchdog_timer_reset(chg_dev, 0x1);
	/* charger status polling */
	hl7015_charger_system_status(chg_dev);
	hl7015_charger_fault_status(chg_dev);
	return status;
}
static int hl7015_do_event(struct charger_device *chg_dev, unsigned int event, unsigned int args)
{
	if (chg_dev == NULL)
		return -EINVAL;
	pr_err("[hl7015] %s: event = %d\n", __func__, event);
	switch (event) {
	//case EVENT_EOC:
	case EVENT_FULL:
		charger_dev_notify(chg_dev, CHARGER_DEV_NOTIFY_EOC);
		break;
	case EVENT_RECHARGE:
		charger_dev_notify(chg_dev, CHARGER_DEV_NOTIFY_RECHG);
		break;
	default:
		break;
	}
	return 0;
}
//Antaiui <AI_BSP_CHG> <hehl> <2021-05-11> add reset ta begin
static int hl7015_reset_ta(struct charger_device *chg_dev)
{
	pr_err("%s\n", __func__);
	//hl7015_set_vindpm(chg_dev, 1);
	hl7015_set_chg_config(chg_dev, 0x0);
	msleep(300);
	//hl7015_set_vindpm(chg_dev, 0);
	hl7015_set_chg_config(chg_dev, 0x1);
	return 0;
}
//Antaiui <AI_BSP_CHG> <hehl> <2021-05-11> add reset ta end
static int hl7015_set_ta_current_pattern(struct charger_device *chg_dev, bool is_increase)
{
	unsigned int status = 0;
	if(!chg_dev)
		return -EINVAL;
	if(true == is_increase) {
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_increase() on 1");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_increase() off 1");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_increase() on 2");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_increase() off 2");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_increase() on 3");
		msleep(281);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_increase() off 3");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_increase() on 4");
		msleep(281);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_increase() off 4");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_increase() on 5");
		msleep(281);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_increase() off 5");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_increase() on 6");
		msleep(485);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_increase() off 6");
		msleep(50);
		pr_err("[hl7015] mtk_ta_increase() end\n");
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		msleep(200);
	} else {
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_decrease() on 1");
		msleep(281);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_decrease() off 1");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_decrease() on 2");
		msleep(281);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_decrease() off 2");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_decrease() on 3");
		msleep(281);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_decrease() off 3");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_decrease() on 4");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_decrease() off 4");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_decrease() on 5");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_decrease() off 5");
		msleep(85);
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
		pr_err("[hl7015] mtk_ta_decrease() on 6");
		msleep(485);
		hl7015_set_iinlim(chg_dev, 0x0); /* 100mA */
		pr_err("[hl7015] mtk_ta_decrease() off 6");
		msleep(50);
		pr_err("[hl7015] mtk_ta_decrease() end\n");
		hl7015_set_iinlim(chg_dev, 0x2); /* 500mA */
	}
	return status;
}
static int hl7015_is_powerpath_enabled(struct charger_device *chg_dev, bool *en)
{
	unsigned int status = 0;
	unsigned char register_value = 0;
	if(!chg_dev)
		return -EINVAL;
	hl7015_read_interface(chg_dev,HL7015_CON0, &register_value, CON0_VINDPM_MASK, CON0_VINDPM_SHIFT);

	if(0xf == register_value)
		*en = false;
	else if (0x7 == register_value)
		*en = true;
	return status;
}
static int hl7015_enable_powerpath(struct charger_device *chg_dev, bool en)
{
	unsigned int status = 0;
	if(!chg_dev)
		return -EINVAL;
	pr_err("[hl7015] hl7015_enable_powerpath: %d.-------------\n", en);
	if(true == en)
		hl7015_set_vindpm(chg_dev, 0xa); // 4.68V
	else
		hl7015_set_vindpm(chg_dev, 0xf); // 5.08V
	return status;
}
static int hl7015_get_vindpm_voltage(struct charger_device *chg_dev, bool *in_loop)
{
	unsigned int status = 0;
	unsigned char register_value = 0;
	if(!chg_dev)
		return -EINVAL;

	hl7015_read_interface(chg_dev,HL7015_CON8, &register_value, CON8_DPM_STAT_MASK, CON8_DPM_STAT_SHIFT);
	if(0 == register_value)
		*in_loop = true;
	else
		*in_loop = false;
	return status;
}

static int hl7015_set_vindpm_voltage(struct charger_device *chg_dev, u32 vindpm_vol)
{
	unsigned int array_size = ARRAY_SIZE(VINDPM_NORMAL_CVTH);
	u32 set_vindpm_vol;
	u32 set_vol_m;
	u32 set_vol_h;
	unsigned char register_value;

	if (!chg_dev)
		return -EINVAL;
	if (vindpm_vol < VINDPM_MEDIUM_CVTH[0]) {
		hl7015_set_vindpm_offset(chg_dev, COND_VINDPM_OFFSET_X1);
		set_vindpm_vol = bmt_find_closest_level(VINDPM_NORMAL_CVTH, array_size, vindpm_vol);
		register_value = charging_parameter_to_value_hl7015(VINDPM_NORMAL_CVTH, array_size, set_vindpm_vol);
		hl7015_set_vindpm(chg_dev, register_value);
	} else if (vindpm_vol >= VINDPM_MEDIUM_CVTH[0] && vindpm_vol < VINDPM_HIGH_CVTH[0]) {
		hl7015_set_vindpm_offset(chg_dev, COND_VINDPM_OFFSET_X1P7);
		set_vindpm_vol = bmt_find_closest_level(VINDPM_MEDIUM_CVTH, array_size, vindpm_vol);
		register_value = charging_parameter_to_value_hl7015(VINDPM_MEDIUM_CVTH, array_size, set_vindpm_vol);
		hl7015_set_vindpm(chg_dev, register_value);
	} else if (vindpm_vol >= VINDPM_HIGH_CVTH[0] && vindpm_vol <= VINDPM_MEDIUM_CVTH[array_size - 1]) {
		set_vol_m = bmt_find_closest_level(VINDPM_MEDIUM_CVTH, array_size, vindpm_vol);
		set_vol_h = bmt_find_closest_level(VINDPM_HIGH_CVTH, array_size, vindpm_vol);
		if (abs(set_vol_m - vindpm_vol) < abs(set_vol_h - vindpm_vol)) {
			hl7015_set_vindpm_offset(chg_dev, COND_VINDPM_OFFSET_X1P7);
			register_value = charging_parameter_to_value_hl7015(VINDPM_MEDIUM_CVTH, array_size, set_vol_m);
			hl7015_set_vindpm(chg_dev, register_value);
		} else {
			hl7015_set_vindpm_offset(chg_dev, COND_VINDPM_OFFSET_X2P14);
			register_value = charging_parameter_to_value_hl7015(VINDPM_HIGH_CVTH, array_size, set_vol_h);
			hl7015_set_vindpm(chg_dev, register_value);
		}
	} else {
		hl7015_set_vindpm_offset(chg_dev, COND_VINDPM_OFFSET_X2P14);
		set_vindpm_vol = bmt_find_closest_level(VINDPM_HIGH_CVTH, array_size, vindpm_vol);
		register_value = charging_parameter_to_value_hl7015(VINDPM_HIGH_CVTH, array_size, set_vindpm_vol);
		hl7015_set_vindpm(chg_dev, register_value);
	}

	return 0;
}

static int hl7015_is_safety_timer_enabled(struct charger_device *chg_dev, bool *en)
{
	unsigned int status = 0;
	unsigned char register_value = 0;
	if(!chg_dev)
		return -EINVAL;

	hl7015_read_interface(chg_dev,HL7015_CON5, &register_value, CON5_CHG_TIMER_MASK, CON5_CHG_TIMER_SHIFT);
	if(1 == register_value)
		*en = true;
	else
		*en = false;
	return status;
}
static int hl7015_enable_safety_timer(struct charger_device *chg_dev, bool en)
{
	unsigned int status = 0;
	if(!chg_dev)
		return -EINVAL;
	if(true == en)
		hl7015_set_en_safty_timer(chg_dev,0x1);
	else
		hl7015_set_en_safty_timer(chg_dev,0x0);
	return status;
}
static int hl7015_charger_enable_otg(struct charger_device *chg_dev, bool en)
{
	unsigned int status = 0;
	if(!chg_dev)
		return -EINVAL;

	pr_err("[hl7015] enable otg %d.\n", en);

	if(true == en) {
		hl7015_set_chg_config(chg_dev, 0x2);
		//enable_boost_polling(true);
		is_otg_mode = 1;
	}else {
		hl7015_set_chg_config(chg_dev, 0x0);
		//enable_boost_polling(false);
		is_otg_mode = 0;
	}

	hl7015_set_dp_voltage(chg_dev, HL7015_DPDM_HIZ);
	hl7015_set_dm_voltage(chg_dev, HL7015_DPDM_HIZ);
	return status;
}
static int hl7015_set_boost_current_limit(struct charger_device *chg_dev, u32 boost_curr)
{
	unsigned int status = 0;
	if(!chg_dev)
		return -EINVAL;
#ifdef SET_OTG_CURRENT_TO_1A
	hl7015_set_boost_lim(chg_dev, 0x0);
#else
	if(boost_curr < 1550000)
		hl7015_set_boost_lim(chg_dev, 0x0);
	else
		hl7015_set_boost_lim(chg_dev, 0x1);
#endif
	return status;
}
static int hl7015_get_charging_status(struct charger_device *chg_dev, bool *is_done)
{
	unsigned int status = 0;
	unsigned int register_val;
	if(!chg_dev)
		return -EINVAL;
	register_val = hl7015_get_chrg_status(chg_dev);
	if (0x3 == register_val)
		*is_done = true;
	else
		*is_done = false;
	return status;
}

static int hl7015_adjust_voltage(struct charger_device *chg_dev, u8 vol)
{
	switch (vol) {
	case CHARGER_HVDCP_VOLTAGE_5V:
		hl7015_set_dp_voltage(chg_dev, HL7015_DPDM_0V6);
		hl7015_set_dm_voltage(chg_dev, HL7015_DPDM_0V);
		break;
	case CHARGER_HVDCP_VOLTAGE_9V:
		hl7015_set_dp_voltage(chg_dev, HL7015_DPDM_3V3);
		hl7015_set_dm_voltage(chg_dev, HL7015_DPDM_0V6);
		break;
	case CHARGER_HVDCP_VOLTAGE_12V:
		hl7015_set_dp_voltage(chg_dev, HL7015_DPDM_0V6);
		hl7015_set_dm_voltage(chg_dev, HL7015_DPDM_0V6);
		break;
	case CHARGER_HVDCP_VOLTAGE_20V:
		hl7015_set_dp_voltage(chg_dev, HL7015_DPDM_3V3);
		hl7015_set_dm_voltage(chg_dev, HL7015_DPDM_3V3);
		break;
	default:
		pr_err("[hl7015]hl7015_adjust_voltage not allow voltage\n");
		return -1;
	}
	return 0;
}

static int hl7015_is_hvdcp(struct charger_device *chg_dev)
{
	struct hl7015_dev_info *bdi = dev_get_drvdata(&chg_dev->dev);
	return bdi->is_hvdcp;
}

static int hl7015_ship_enable(struct charger_device *chg_dev, bool en)
{
	if (en) {
		hl7015_set_tship(chg_dev, 1);
	}
	hl7015_set_ppfet_disable(chg_dev, (unsigned int)en);
	return 0;
}

static int hl7015_register_reset(struct hl7015_dev_info *bdi)
{
	int ret;
	//u8 v;
	//int limit = 100
	/*
	 * This prop. can be passed on device instantiation from platform code:
	 * struct property_entry pe[] =
	 *   { PROPERTY_ENTRY_BOOL("disable-reset"), ... };
	 * struct i2c_board_info bi =
	 *   { .type = "hl7015", .addr = 0x6b, .properties = pe, .irq = irq };
	 * struct i2c_adapter ad = { ... };
	 * i2c_add_adapter(&ad);
	 * i2c_new_client_device(&ad, &bi);
	 */
	printk("%s: start \n", __func__);
	if (device_property_read_bool(bdi->dev, "disable-reset"))
		return 0;
	/* Reset the registers */
	ret = hl7015_write_mask(bdi, HL7015_REG_POC,
			HL7015_REG_POC_RESET_MASK,
			HL7015_REG_POC_RESET_SHIFT,
			0x1);
	if (ret < 0) {
		printk("%s failed, ret=%d\n", __func__, ret);
		return ret;
	}
	return 0;
}
/* Charger power supply property routines */
static int hl7015_charger_get_charge_type(struct hl7015_dev_info *bdi,
		union power_supply_propval *val)
{
	u8 v;
	int type, ret;
	ret = hl7015_read_mask(bdi, HL7015_REG_POC,
			HL7015_REG_POC_CHG_CONFIG_MASK,
			HL7015_REG_POC_CHG_CONFIG_SHIFT,
			&v);
	if (ret < 0)
		return ret;
	/* If POC[CHG_CONFIG] (REG01[5:4]) == 0, charge is disabled */
	if (!v) {
		type = POWER_SUPPLY_CHARGE_TYPE_NONE;
	} else {
		ret = hl7015_read_mask(bdi, HL7015_REG_CCC,
				HL7015_REG_CCC_FORCE_20PCT_MASK,
				HL7015_REG_CCC_FORCE_20PCT_SHIFT,
				&v);
		if (ret < 0)
			return ret;
		type = (v) ? POWER_SUPPLY_CHARGE_TYPE_TRICKLE :
			     POWER_SUPPLY_CHARGE_TYPE_FAST;
	}
	val->intval = type;
	return 0;
}
static int hl7015_charger_set_charge_type(struct hl7015_dev_info *bdi,
		const union power_supply_propval *val)
{
	u8 chg_config, force_20pct, en_term;
	int ret;
	/*
	 * According to the "Termination when REG02[0] = 1" section of
	 * the hl7015 manual, the trickle charge could be less than the
	 * termination current so it recommends turning off the termination
	 * function.
	 *
	 * Note: AFAICT from the datasheet, the user will have to manually
	 * turn off the charging when in 20% mode.  If its not turned off,
	 * there could be battery damage.  So, use this mode at your own risk.
	 */
	switch (val->intval) {
	case POWER_SUPPLY_CHARGE_TYPE_NONE:
		chg_config = 0x0;
		break;
	case POWER_SUPPLY_CHARGE_TYPE_TRICKLE:
		chg_config = 0x1;
		force_20pct = 0x1;
		en_term = 0x0;
		break;
	case POWER_SUPPLY_CHARGE_TYPE_FAST:
		chg_config = 0x1;
		force_20pct = 0x0;
		en_term = 0x1;
		break;
	default:
		return -EINVAL;
	}
	if (chg_config) { /* Enabling the charger */
		ret = hl7015_write_mask(bdi, HL7015_REG_CCC,
				HL7015_REG_CCC_FORCE_20PCT_MASK,
				HL7015_REG_CCC_FORCE_20PCT_SHIFT,
				force_20pct);
		if (ret < 0)
			return ret;
		ret = hl7015_write_mask(bdi, HL7015_REG_CTTC,
				HL7015_REG_CTTC_EN_TERM_MASK,
				HL7015_REG_CTTC_EN_TERM_SHIFT,
				en_term);
		if (ret < 0)
			return ret;
	}
	return hl7015_write_mask(bdi, HL7015_REG_POC,
			HL7015_REG_POC_CHG_CONFIG_MASK,
			HL7015_REG_POC_CHG_CONFIG_SHIFT, chg_config);
}
static int hl7015_charger_get_health(struct hl7015_dev_info *bdi,
		union power_supply_propval *val)
{
	u8 v;
	int health;
	mutex_lock(&bdi->f_reg_lock);
	v = bdi->f_reg;
	mutex_unlock(&bdi->f_reg_lock);
	if (v & HL7015_REG_F_NTC_FAULT_MASK) {
		switch (v >> HL7015_REG_F_NTC_FAULT_SHIFT & 0x7) {
		case 0x1: /* TS1  Cold */
		case 0x3: /* TS2  Cold */
		case 0x5: /* Both Cold */
			health = POWER_SUPPLY_HEALTH_COLD;
			break;
		case 0x2: /* TS1  Hot */
		case 0x4: /* TS2  Hot */
		case 0x6: /* Both Hot */
			health = POWER_SUPPLY_HEALTH_OVERHEAT;
			break;
		default:
			health = POWER_SUPPLY_HEALTH_UNKNOWN;
		}
	} else if (v & HL7015_REG_F_BAT_FAULT_MASK) {
		health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	} else if (v & HL7015_REG_F_CHRG_FAULT_MASK) {
		switch (v >> HL7015_REG_F_CHRG_FAULT_SHIFT & 0x3) {
		case 0x1: /* Input Fault (VBUS OVP or VBAT<VBUS<3.8V) */
			/*
			 * This could be over-voltage or under-voltage
			 * and there's no way to tell which.  Instead
			 * of looking foolish and returning 'OVERVOLTAGE'
			 * when its really under-voltage, just return
			 * 'UNSPEC_FAILURE'.
			 */
			health = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
			break;
		case 0x2: /* Thermal Shutdown */
			health = POWER_SUPPLY_HEALTH_OVERHEAT;
			break;
		case 0x3: /* Charge Safety Timer Expiration */
			health = POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
			break;
		default:  /* prevent compiler warning */
			health = -1;
		}
	} else if (v & HL7015_REG_F_BOOST_FAULT_MASK) {
		/*
		 * This could be over-current or over-voltage but there's
		 * no way to tell which.  Return 'OVERVOLTAGE' since there
		 * isn't an 'OVERCURRENT' value defined that we can return
		 * even if it was over-current.
		 */
		health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	} else {
		health = POWER_SUPPLY_HEALTH_GOOD;
	}
	val->intval = health;
	return 0;
}

static int hl7015_charger_get_online(struct hl7015_dev_info *bdi,
		union power_supply_propval *val)
{
	u32 vbus = 0;
	int ret;

	if (is_otg_mode)
		val->intval = 0;
	else {
		ret = hl7015_get_vbus(bdi->chg_dev, &vbus);
		pr_err("vbus = %d, ret = %d\n", vbus, ret);
		if (ret < 0) {
			val->intval = 0;
			return ret;
		} else {
			if (vbus > VBUS_MIN_UV && atomic_read(&bdi->attached))
				val->intval = 1;
			else
				val->intval = 0;
		}
	}

	return 0;
}

static int hl7015_charger_get_precharge(struct hl7015_dev_info *bdi,
		union power_supply_propval *val)
{
	u8 v;
	int ret;
	ret = hl7015_read_mask(bdi, HL7015_REG_PCTCC,
			HL7015_REG_PCTCC_IPRECHG_MASK,
			HL7015_REG_PCTCC_IPRECHG_SHIFT, &v);
	if (ret < 0)
		return ret;
	val->intval = ++v * 128 * 1000;
	return 0;
}
static int hl7015_charger_get_charge_term(struct hl7015_dev_info *bdi,
		union power_supply_propval *val)
{
	u8 v;
	int ret;
	ret = hl7015_read_mask(bdi, HL7015_REG_PCTCC,
			HL7015_REG_PCTCC_ITERM_MASK,
			HL7015_REG_PCTCC_ITERM_SHIFT, &v);
	if (ret < 0)
		return ret;
	val->intval = ++v * 128 * 1000;
	return 0;
}
static int hl7015_charger_get_current(struct hl7015_dev_info *bdi,
		union power_supply_propval *val)
{
	u8 v;
	int curr, ret;
	ret = hl7015_get_field_val(bdi, HL7015_REG_CCC,
			HL7015_REG_CCC_ICHG_MASK, HL7015_REG_CCC_ICHG_SHIFT,
			CSTH,
			ARRAY_SIZE(CSTH), &curr);
	if (ret < 0)
		return ret;
	ret = hl7015_read_mask(bdi, HL7015_REG_CCC,
			HL7015_REG_CCC_FORCE_20PCT_MASK,
			HL7015_REG_CCC_FORCE_20PCT_SHIFT, &v);
	if (ret < 0)
		return ret;
	/* If FORCE_20PCT is enabled, then current is 20% of ICHG value */
	if (v)
		curr /= 5;
	val->intval = curr;
	return 0;
}
static int hl7015_charger_get_current_max(struct hl7015_dev_info *bdi,
		union power_supply_propval *val)
{
	int idx = ARRAY_SIZE(CSTH) - 1;
	val->intval = CSTH[idx];
	return 0;
}
static int hl7015_charger_set_current(struct hl7015_dev_info *bdi,
		const union power_supply_propval *val)
{
	u8 v;
	int ret, curr = val->intval;
	ret = hl7015_read_mask(bdi, HL7015_REG_CCC,
			HL7015_REG_CCC_FORCE_20PCT_MASK,
			HL7015_REG_CCC_FORCE_20PCT_SHIFT, &v);
	if (ret < 0)
		return ret;
	/* If FORCE_20PCT is enabled, have to multiply value passed in by 5 */
	if (v)
		curr *= 5;
	return hl7015_set_field_val(bdi, HL7015_REG_CCC,
			HL7015_REG_CCC_ICHG_MASK, HL7015_REG_CCC_ICHG_SHIFT,
			CSTH,
			ARRAY_SIZE(CSTH), curr);
}
static int hl7015_charger_get_voltage(struct hl7015_dev_info *bdi,
		union power_supply_propval *val)
{
	int voltage, ret;
	ret = hl7015_get_field_val(bdi, HL7015_REG_CVC,
			HL7015_REG_CVC_VREG_MASK, HL7015_REG_CVC_VREG_SHIFT,
			VBAT_CVTH,
			ARRAY_SIZE(VBAT_CVTH), &voltage);
	if (ret < 0)
		return ret;
	val->intval = voltage;
	return 0;
}
static int hl7015_charger_get_voltage_max(struct hl7015_dev_info *bdi,
		union power_supply_propval *val)
{
	int idx = ARRAY_SIZE(VBAT_CVTH) - 1;
	val->intval = VBAT_CVTH[idx];
	return 0;
}
static int hl7015_charger_set_voltage(struct hl7015_dev_info *bdi,
		const union power_supply_propval *val)
{
	printk("%s: val=%d\n", __func__, val->intval);
	return hl7015_set_field_val(bdi, HL7015_REG_CVC,
			HL7015_REG_CVC_VREG_MASK, HL7015_REG_CVC_VREG_SHIFT,
			VBAT_CVTH,
			ARRAY_SIZE(VBAT_CVTH), val->intval);
}
static int hl7015_charger_get_iinlimit(struct hl7015_dev_info *bdi,
		union power_supply_propval *val)
{
	int iinlimit, ret;
	printk("%s: HL7015_REG_ISC \n", __func__);
	ret = hl7015_get_field_val(bdi, HL7015_REG_ISC,
			HL7015_REG_ISC_IINLIM_MASK,
			HL7015_REG_ISC_IINLIM_SHIFT,
			INPUT_CSTH,
			ARRAY_SIZE(INPUT_CSTH), &iinlimit);
	if (ret < 0)
		return ret;
	val->intval = iinlimit;
	return 0;
}
static int hl7015_charger_set_iinlimit(struct hl7015_dev_info *bdi,
		const union power_supply_propval *val)
{
	printk("%s: HL7015_REG_ISC \n", __func__);
	return hl7015_set_field_val(bdi, HL7015_REG_ISC,
			HL7015_REG_ISC_IINLIM_MASK,
			HL7015_REG_ISC_IINLIM_SHIFT,
			INPUT_CSTH,
			ARRAY_SIZE(INPUT_CSTH), val->intval);
}
static int hl7015_charger_get_property(struct power_supply *psy,
		enum power_supply_property psp, union power_supply_propval *val)
{
	struct hl7015_dev_info *bdi = power_supply_get_drvdata(psy);
	int ret = 0, value;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		ret = hl7015_charger_get_charge_type(bdi, val);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = hl7015_charger_get_health(bdi, val);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		//dump_stack();
		ret = hl7015_charger_get_online(bdi, val);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		break;
	case POWER_SUPPLY_PROP_TEMP_ALERT_MAX:
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = iio_read_channel_processed(bdi->vbus, &value);
		if (ret < 0) {
			dev_err(bdi->dev, "get vbus voltage failed");
			return -EINVAL;
		}
		val->intval = value + R_VBUS_CHARGER_1 * value / R_VBUS_CHARGER_2;
		dev_info(bdi->dev, "vbus voltage: %d", val->intval);
		break;
	case POWER_SUPPLY_PROP_PRECHARGE_CURRENT:
		ret = hl7015_charger_get_precharge(bdi, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		ret = hl7015_charger_get_charge_term(bdi, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = hl7015_charger_get_current(bdi, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		ret = hl7015_charger_get_current_max(bdi, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = hl7015_charger_get_voltage(bdi, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		ret = hl7015_charger_get_voltage_max(bdi, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = hl7015_charger_get_iinlimit(bdi, val);
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = bdi->model_name;
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = HL7015_MANUFACTURER;
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = bdi->psy_usb_type;
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = bdi->chg_type;
		//drv_fix
    	power_supply_set_property(bdi->charger,POWER_SUPPLY_PROP_TYPE, val);
		//drv_fix
		break;
	default:
		ret = -ENODATA;
	}
	return ret;
}

static void wait_and_check_detach(struct hl7015_dev_info *bdi, int count)
{
	while (count > 0) {
		msleep(100);
		--count;
		if (!atomic_read(&bdi->attached))
			return;
	}
}

static int hl7015db_hvdcp_detection(struct hl7015_dev_info *bdi)
{
	int ret;
	unsigned char reg_val;

	hl7015_start_hvdcp(bdi->chg_dev);
	wait_and_check_detach(bdi, 20);
	ret = hl7015_read_interface(bdi->chg_dev, HL7015_CON10, &reg_val, CON10_DM_STAT_MASK,
					CON10_DM_STAT_SHIFT);
	hl7015_set_en_hvdcp(bdi->chg_dev, false);
	if (ret) {
		dev_err(bdi->dev, "%s: read dm stat failed, ret = %d\n", __func__, ret);
		return -1;
	}
	if (!reg_val) {
		dev_err(bdi->dev, "%s, HVDCP found\n", __func__);
		hl7015_adjust_voltage(bdi->chg_dev, CHARGER_HVDCP_VOLTAGE_9V);
		bdi->is_hvdcp = 1;
		return 0;
	} else {
		hl7015_set_dp_voltage(bdi->chg_dev, HL7015_DPDM_HIZ);
		hl7015_set_dm_voltage(bdi->chg_dev, HL7015_DPDM_HIZ);
		return -1;
	}
}

static void hl7015db_get_divider_type(struct hl7015_dev_info *bdi)
{
	int ret;
	u32 cur = 0;

	ret = hl7015_get_input_current(bdi->chg_dev, &cur);
	if (ret)
		bdi->chg_type = POWER_SUPPLY_TYPE_USB_DIVIDER3;
	else {
		if (cur == DIVIDER2_IINDPM_UA)
			bdi->chg_type = POWER_SUPPLY_TYPE_USB_DIVIDER2;
		else
			bdi->chg_type = POWER_SUPPLY_TYPE_USB_DIVIDER3;
	}
}

static void check_secondary_detection_done(struct hl7015_dev_info *bdi)
{
	int ret;
	unsigned char vin_stat1;
	int retry = 30;

	do {
		msleep(100);
		ret = hl7015_read_interface(bdi->chg_dev, HL7015_CON10, &vin_stat1, CON10_VIN_STAT1_MASK,
						CON10_VIN_STAT1_SHIFT);
		--retry;
		if (!atomic_read(&bdi->attached)) {
			dev_err(bdi->dev, "%s: detached, stop wait!\n", __func__);
			break;
		}
		if (ret)
			continue;
	} while (vin_stat1 == VIN_STAT1_UNKNOWN && retry > 0);
	dev_err(bdi->dev, "%s: used %dms\n", __func__, (30 - retry) * 100);
}

static void hl7015db_get_charger_type(struct hl7015_dev_info *bdi)
{
	int ret;
	unsigned char vin_stat, vin_stat1;

	bdi->is_hvdcp = 0;
	ret = hl7015_read_interface(bdi->chg_dev, HL7015_CON8, &vin_stat, CON8_VIN_STAT_MASK, CON8_VIN_STAT_SHIFT);
	if (ret) {
		dev_err(bdi->dev, "%s: read vin stat failed, ret = %d\n", __func__, ret);
		return;
	}
	dev_err(bdi->dev, "%s: vin stat = %d\n", __func__, vin_stat);
	if (vin_stat == VIN_STAT_USB_HOST) {
		bdi->psy_usb_type = POWER_SUPPLY_USB_TYPE_SDP;
		bdi->chg_type = POWER_SUPPLY_TYPE_USB;
		bdi->psy_desc.type = POWER_SUPPLY_TYPE_USB;
	} else if (vin_stat == VIN_STAT_ADAPTER_PORT) {
		check_secondary_detection_done(bdi);
		ret = hl7015_read_interface(bdi->chg_dev, HL7015_CON10, &vin_stat1, CON10_VIN_STAT1_MASK,
						CON10_VIN_STAT1_SHIFT);
		if (ret) {
			dev_err(bdi->dev, "%s: read vin stat1 failed, ret = %d\n", __func__, ret);
			bdi->psy_usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
			bdi->chg_type = POWER_SUPPLY_TYPE_USB_OTHER;
			bdi->psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
			return;
		}
		dev_err(bdi->dev, "%s: vin stat1 = %d\n", __func__, vin_stat1);
		if (vin_stat1 == VIN_STAT1_USB_CDP) {
			bdi->psy_usb_type = POWER_SUPPLY_USB_TYPE_CDP;
			bdi->chg_type = POWER_SUPPLY_TYPE_USB_CDP;
			bdi->psy_desc.type = POWER_SUPPLY_TYPE_USB_CDP;
		} else if (vin_stat1 == VIN_STAT1_NON_STD) {
			hl7015db_get_divider_type(bdi);
			bdi->psy_usb_type = POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID;
			bdi->psy_desc.type = POWER_SUPPLY_TYPE_APPLE_BRICK_ID;
		} else if (vin_stat1 == VIN_STAT1_USB_DCP) {
			bdi->psy_usb_type = POWER_SUPPLY_USB_TYPE_DCP;
			bdi->psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
			if (atomic_read(&bdi->attached))
				power_supply_changed(bdi->charger);
			if (hl7015db_hvdcp_detection(bdi))
				bdi->chg_type = POWER_SUPPLY_TYPE_USB_DCP;
			else
				bdi->chg_type = POWER_SUPPLY_TYPE_USB_HVDCP;
		} else {
			bdi->psy_usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
			bdi->chg_type = POWER_SUPPLY_TYPE_USB_OTHER;
			bdi->psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
		}
	} else if (vin_stat == VIN_STAT_UNKNOWN) {
		bdi->psy_usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		bdi->chg_type = POWER_SUPPLY_TYPE_USB_OTHER;
		bdi->psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
	}
}

static void wait_type_detection_done(struct hl7015_dev_info *bdi)
{
	int ret;
	unsigned char vin_stat;
	int retry = 30;

	do {
		msleep(100);
		ret = hl7015_read_interface(bdi->chg_dev, HL7015_CON8, &vin_stat, CON8_VIN_STAT_MASK, CON8_VIN_STAT_SHIFT);
		--retry;
		if (!atomic_read(&bdi->attached)) {
			dev_err(bdi->dev, "%s: detached, stop wait!\n", __func__);
			break;
		}
		if (retry == 25) {
			bdi->psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
			power_supply_changed(bdi->charger);
		}
		if (ret)
			continue;
	} while (!vin_stat && retry > 0);
	dev_err(bdi->dev, "%s: used %dms\n", __func__, (30 - retry) * 100);
}

static void hl7015db_usb_type_detection(struct hl7015_dev_info *bdi)
{
	int ret;
	unsigned char vin_stat;

	Charger_Detect_Init();
	ret = hl7015_set_dpdm_en(bdi->chg_dev, 1);
	if (ret) {
		dev_err(bdi->dev, "%s: force dpdm detection failed\n", __func__);
		goto out;
	}
	wait_type_detection_done(bdi);
	if (!atomic_read(&bdi->attached))
		goto out;
	ret = hl7015_read_interface(bdi->chg_dev, HL7015_CON8, &vin_stat, CON8_VIN_STAT_MASK, CON8_VIN_STAT_SHIFT);
	if (ret) {
		dev_err(bdi->dev, "%s: read vin stat failed, ret = %d\n", __func__, ret);
		goto out;
	}
	if (vin_stat == VIN_STAT_USB_HOST) {
		ret = hl7015_set_dpdm_en(bdi->chg_dev, 1);
		if (ret) {
			dev_err(bdi->dev, "%s: force dpdm detection failed\n", __func__);
			goto out;
		}
		wait_type_detection_done(bdi);
		if (!atomic_read(&bdi->attached))
			goto out;
	}
	hl7015db_get_charger_type(bdi);
	if (!atomic_read(&bdi->attached)) {
		bdi->is_hvdcp = 0;
		bdi->psy_usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		bdi->chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
		bdi->psy_desc.type = POWER_SUPPLY_TYPE_UNKNOWN;
	}
	else
		power_supply_changed(bdi->charger);

out:
	Charger_Detect_Release();
}

static void hl7015db_force_detection_dwork_handler(struct work_struct *work)
{
	struct hl7015_dev_info *bdi = container_of(work, struct hl7015_dev_info, force_detect_dwork.work);

	hl7015db_usb_type_detection(bdi);
}

static int hl7015_charger_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct hl7015_dev_info *bdi = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		dev_err(bdi->dev, "%s: %d, attach notified\n", __func__, val->intval);
		if (val->intval == ATTACH_TYPE_TYPEC) {
			atomic_set(&bdi->attached, 1);
			schedule_delayed_work(&bdi->force_detect_dwork, msecs_to_jiffies(0));
		} else if (val->intval == ATTACH_TYPE_NONE) {
			atomic_set(&bdi->attached, 0);
			if(!strcmp(bdi->chg_dev_name, "primary_chg"))
				hl7015_set_en_hiz(bdi->chg_dev,false);
			bdi->is_hvdcp = 0;
			bdi->psy_usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
			bdi->chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
			bdi->psy_desc.type = POWER_SUPPLY_TYPE_UNKNOWN;
			power_supply_changed(bdi->charger);
		} else {
			atomic_set(&bdi->attached, 1);
		}
		break;
	case POWER_SUPPLY_PROP_TEMP_ALERT_MAX:
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		ret = hl7015_charger_set_charge_type(bdi, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = hl7015_charger_set_current(bdi, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = hl7015_charger_set_voltage(bdi, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = hl7015_charger_set_iinlimit(bdi, val);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

static int hl7015_charger_property_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_TEMP_ALERT_MAX:
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return 1;
	default:
		return 0;
	}
}
static enum power_supply_property hl7015_charger_properties[] = {
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TEMP_ALERT_MAX,
	POWER_SUPPLY_PROP_PRECHARGE_CURRENT,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static enum power_supply_usb_type hl7015_chg_psy_usb_types[] = {
	POWER_SUPPLY_USB_TYPE_UNKNOWN,
	POWER_SUPPLY_USB_TYPE_SDP,
	POWER_SUPPLY_USB_TYPE_CDP,
	POWER_SUPPLY_USB_TYPE_DCP,
	POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID,
};

static char *hl7015_charger_supplied_to[] = {
	"battery",
	"mtk-master-charger",
};

static const struct power_supply_desc hl7015_charger_desc = {
	.name                  = "ext_charger_type",
	.type                  = POWER_SUPPLY_TYPE_MAINS,
	.usb_types             = hl7015_chg_psy_usb_types,
	.num_usb_types         = ARRAY_SIZE(hl7015_chg_psy_usb_types),
	.properties            = hl7015_charger_properties,
	.num_properties        = ARRAY_SIZE(hl7015_charger_properties),
	.get_property          = hl7015_charger_get_property,
	.set_property          = hl7015_charger_set_property,
	.property_is_writeable = hl7015_charger_property_is_writeable,
};

static irqreturn_t hl7015_irq_handler_thread(int irq, void *data)
{
	struct hl7015_dev_info *bdi = data;
	int error;
	bdi->irq_event = true;
	error = pm_runtime_get_sync(bdi->dev);
	printk("%s: ret = %d \n", __func__, error);
	if (error < 0) {
		dev_warn(bdi->dev, "pm_runtime_get failed: %i\n", error);
		pm_runtime_put_noidle(bdi->dev);
		return IRQ_NONE;
	}
	pm_runtime_mark_last_busy(bdi->dev);
	pm_runtime_put_autosuspend(bdi->dev);
	bdi->irq_event = false;
//	power_supply_changed(bdi->charger);
	return IRQ_HANDLED;
}
static int hl7015_parse_dt(struct hl7015_dev_info *bdi)
{
	int ret, i;
	int irq_gpio = 0, irqn = 0;
	struct {
		char *name;
		int *conv_data;
		int *default_data;
	} props[] = {
		{"hl7015db,wd-time", &(bdi->cfg.wd_time), &(hl7015_default_cfg.wd_time)},
		{"hl7015db,vbat-cv", &(bdi->cfg.vbat_cv), &(hl7015_default_cfg.vbat_cv)},
		{"hl7015db,iterm", &(bdi->cfg.iterm), &(hl7015_default_cfg.iterm)},
		{"hl7015db,vrechg", &(bdi->cfg.vrechg), &(hl7015_default_cfg.vrechg)},
	};

	irq_gpio = of_get_named_gpio(bdi->dev->of_node, "irq-gpio", 0);
	if (!gpio_is_valid(irq_gpio))
	{
		dev_err(bdi->dev, "%s: %d gpio get failed\n", __func__, irq_gpio);
		return -EINVAL;
	}
	ret = gpio_request(irq_gpio, "hl7015 irq pin");
	if (ret) {
		dev_err(bdi->dev, "%s: %d gpio request failed\n", __func__, irq_gpio);
		return ret;
	}
	gpio_direction_input(irq_gpio);
	irqn = gpio_to_irq(irq_gpio);
	if (irqn < 0) {
		dev_err(bdi->dev, "%s:%d gpio_to_irq failed\n", __func__, irqn);
		return irqn;
	}
	bdi->client->irq = irqn;
	pr_info("[%s] irq = %d  \n", __func__, bdi->client->irq);

	if (of_property_read_string(bdi->dev->of_node, "alias_name", &(bdi->chg_props.alias_name)) < 0) {
		bdi->chg_props.alias_name = "hl7015";
		dev_err(bdi->dev, "[hl7015] %s: no alias name\n", __func__);
	}

	if (of_property_read_string(bdi->dev->of_node, "chg-name", &bdi->chg_dev_name) < 0) {
		dev_err(bdi->dev, "[hl7015] %s: no chg name\n", __func__);
		return -EOPNOTSUPP;
	}
	printk("%s: chg-name = %s\n", __func__,bdi->chg_dev_name);

	/* initialize data for optional properties */
	for (i = 0; i < ARRAY_SIZE(props); i++) {
		ret = of_property_read_u32(bdi->dev->of_node, props[i].name, props[i].conv_data);
		if (ret < 0) {
			dev_err(bdi->dev, "%s not find, use default value\n", props[i].name);
			*(props[i].conv_data) = *(props[i].default_data);
			continue;
		}
	}
	pr_info("[%s] end \n", __func__);

	return 0;
}

static void hl7015_init_device(struct hl7015_dev_info *bdi)
{
	hl7015_set_i2cwatchdog(bdi->chg_dev, bdi->cfg.wd_time);
	hl7015_set_cv_voltage(bdi->chg_dev, bdi->cfg.vbat_cv);
	hl7015_set_termination_curr(bdi->chg_dev, bdi->cfg.iterm);
	hl7015_set_vrechg(bdi->chg_dev, bdi->cfg.vrechg);
}

static int hl7015_get_config(struct hl7015_dev_info *bdi)
{
	const char * const s = "ti,system-minimum-microvolt";
	struct power_supply_battery_info info = {};
	int v;
	if (device_property_read_u32(bdi->dev, s, &v) == 0) {
		v /= 1000;
		if (v >= HL7015_REG_POC_SYS_MIN_MIN
		 && v <= HL7015_REG_POC_SYS_MIN_MAX)
			bdi->sys_min = v;
		else
			dev_warn(bdi->dev, "invalid value for %s: %u\n", s, v);
	}
	if (bdi->dev->of_node &&
	    !power_supply_get_battery_info(bdi->charger, &info)) {
		v = info.precharge_current_ua / 1000;
		if (v >= HL7015_REG_PCTCC_IPRECHG_MIN
		 && v <= HL7015_REG_PCTCC_IPRECHG_MAX)
			bdi->iprechg = v;
		else
			dev_warn(bdi->dev, "invalid value for battery:precharge-current-microamp: %d\n",
				 v);
		v = info.charge_term_current_ua / 1000;
		if (v >= HL7015_REG_PCTCC_ITERM_MIN
		 && v <= HL7015_REG_PCTCC_ITERM_MAX)
			bdi->iterm = v;
		else
			dev_warn(bdi->dev, "invalid value for battery:charge-term-current-microamp: %d\n",
				 v);
	}
	return 0;
}
static struct charger_ops hl7015_chg_ops = {
	/* enable charging */
	.enable = hl7015_enable_charging,
	/* charge current stuff */
	.get_charging_current = hl7015_get_current,
	.set_charging_current = hl7015_set_current,
	/* charge voltage stuff */
	.get_constant_voltage = hl7015_get_cv_voltage,
	.set_constant_voltage = hl7015_set_cv_voltage,
	/* input charge current stuff */
	.get_input_current = hl7015_get_input_current,
	.set_input_current = hl7015_set_input_current,
	/* termination current stuff */
	.get_eoc_current = hl7015_get_termination_curr,
	.set_eoc_current = hl7015_set_termination_curr,
	/* ic watch dog */
	.kick_wdt = hl7015_reset_watch_dog_timer,
	.event = hl7015_do_event,
	/* pe/pe+ */
	.send_ta_current_pattern = hl7015_set_ta_current_pattern,
	/* powerpath stuff */
	.is_powerpath_enabled = hl7015_is_powerpath_enabled,
	.enable_powerpath = hl7015_enable_powerpath,     //drv_fix   2_reboot
	/* vindpm stuff */
	.get_mivr_state = hl7015_get_vindpm_voltage,
	.set_mivr = hl7015_set_vindpm_voltage,
	/* safety timer stuff */
	.is_safety_timer_enabled = hl7015_is_safety_timer_enabled,
	.enable_safety_timer = hl7015_enable_safety_timer,
	/* OTG */
	.enable_otg = hl7015_charger_enable_otg,
	.set_boost_current_limit = hl7015_set_boost_current_limit,
	/* charging over */
	.is_charging_done = hl7015_get_charging_status,
	/* dump info */
	.dump_registers = hl7015_dump_register,
//Antaiui <AI_BSP_CHG> <hehl> <2021-05-11> add reset ta begin
	.reset_ta = hl7015_reset_ta,
//Antaiui <AI_BSP_CHG> <hehl> <2021-05-11> add reset ta end
	.enable_hz = hl7015_set_en_hiz,
	.get_adc = hl7015_get_adc,
	.get_vbus_adc = hl7015_get_vbus,
	.request_hvdcp_voltage = hl7015_adjust_voltage,
	.is_hvdcp = hl7015_is_hvdcp,
	.ship_enable = hl7015_ship_enable,
};

//reigster
static ssize_t hl7015_show_registers(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct hl7015_dev_info *bdi = dev_get_drvdata(dev);
	uint8_t addr;
	uint8_t val;
	uint8_t tmpbuf[300];
	int len;
	int idx = 0;
	int ret;

	idx = snprintf(buf, PAGE_SIZE, "%s:\n", "hl7015");
	for (addr = 0x0; addr <= 0x10; addr++) {
		ret = hl7015_read(bdi, addr, &val);
		if (ret == 0) {
			len = snprintf(tmpbuf, PAGE_SIZE - idx,
				"Reg[%.2X] = 0x%.2x\n", addr, val);
			memcpy(&buf[idx], tmpbuf, len);
			idx += len;
		}
	}

	return idx;
}

static ssize_t hl7015_store_register(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct hl7015_dev_info *bdi = dev_get_drvdata(dev);
	int ret;
	unsigned int val;
	unsigned int reg;
	ret = sscanf(buf, "%x %x", &reg, &val);
	if (ret == 2 && reg <= 0x10)
		hl7015_write(bdi, (unsigned char)reg, (unsigned char)val);

	return count;
}

static DEVICE_ATTR(registers, 0660, hl7015_show_registers, hl7015_store_register);

static int hl7015_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = client->adapter;
	struct device *dev = &client->dev;
	struct power_supply_config charger_cfg = {};
	struct hl7015_dev_info *bdi;
	int ret;
	printk("%s: start\n", __func__);
	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(dev, "No support for SMBUS_BYTE_DATA\n");
		return -ENODEV;
	}

        ret = i2c_smbus_read_byte_data(client, 0x00);
	if (ret < 0)
	{
	        dev_err(dev, "hl7015_probe no dev\n");
		return -ENODEV;
        }
	bdi = devm_kzalloc(dev, sizeof(*bdi), GFP_KERNEL);
	if (!bdi) {
		dev_err(dev, "Can't alloc bdi struct\n");
		return -ENOMEM;
	}
	bdi->is_hvdcp = 0;
	bdi->client = client;
	bdi->dev = dev;
	strncpy(bdi->model_name, id->name, sizeof(bdi->model_name));
	mutex_init(&bdi->f_reg_lock);
	bdi->f_reg = 0;
	bdi->ss_reg = HL7015_REG_SS_VBUS_STAT_MASK; /* impossible state */
	i2c_set_clientdata(client, bdi);

	bdi->vbus = devm_iio_channel_get(bdi->dev, "pmic_vbus");
	if (IS_ERR_OR_NULL(bdi->vbus)) {
		dev_err(bdi->dev, "hl7015 get vbus failed\n");
		return -EPROBE_DEFER;
	}
	ret = hl7015_parse_dt(bdi);
	if (ret)
		return ret;
	new_client = client;
	if (bdi->client->irq <= 0) {
		dev_err(dev, "Can't get irq info\n");
		return -EINVAL;
	}
	if (!strcmp(bdi->chg_dev_name, "primary_chg"))
		hl7015_set_en_hiz(bdi->chg_dev, false);
	pm_runtime_enable(dev);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 600);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get failed: %i\n", ret);
		goto out_pmrt;
	}
	memcpy(&bdi->psy_desc, &hl7015_charger_desc, sizeof(bdi->psy_desc));
	if (!strcmp(bdi->chg_dev_name, "sub_chg1")) {
		bdi->psy_desc.name = "sub_chg1_psy";
		bdi->psy_desc.type = POWER_SUPPLY_TYPE_UNKNOWN;
	} else if (!strcmp(bdi->chg_dev_name, "sub_chg2")) {
		bdi->psy_desc.name = "sub_chg2_psy";
		bdi->psy_desc.type = POWER_SUPPLY_TYPE_UNKNOWN;
	}
#ifdef CONFIG_SYSFS
	hl7015_sysfs_init_attrs();
	charger_cfg.attr_grp = hl7015_sysfs_groups;
#endif
	charger_cfg.drv_data = bdi;
	charger_cfg.of_node = dev->of_node;
	charger_cfg.supplied_to = hl7015_charger_supplied_to;
	charger_cfg.num_supplicants = ARRAY_SIZE(hl7015_charger_supplied_to);
	bdi->charger = power_supply_register(dev, &bdi->psy_desc, &charger_cfg);
	if (IS_ERR(bdi->charger)) {
		dev_err(dev, "Can't register charger\n");
		ret = PTR_ERR(bdi->charger);
		goto out_pmrt;
	}
	ret = hl7015_get_config(bdi);
	if (ret < 0) {
		dev_err(dev, "Can't get devicetree config\n");
		goto out_charger;
	}
	/* Register charger device */
	bdi->chg_dev = charger_device_register(bdi->chg_dev_name,
		&client->dev, bdi, &hl7015_chg_ops, &bdi->chg_props);
        if(!strcmp(bdi->chg_dev_name, "primary_chg"))
        {
		ret = devm_request_threaded_irq(dev, bdi->client->irq, NULL,
			hl7015_irq_handler_thread,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			bdi->chg_dev_name, bdi);
		if (ret < 0) {
			dev_err(dev, "Can't set up irq handler\n");
			goto out_charger;
		}
		enable_irq_wake(bdi->client->irq);
	} else {
		hl7015_enable_charging(bdi->chg_dev, false);
		hl7015_set_en_hiz(bdi->chg_dev, true);
	}
	INIT_DELAYED_WORK(&bdi->force_detect_dwork, hl7015db_force_detection_dwork_handler);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	hl7015_init_device(bdi);
	device_create_file(dev, &dev_attr_registers);
	printk("%s: success\n", __func__);
	hl7015_dump_register(bdi->chg_dev);
	bdi->initialized = true;
	return 0;
out_charger:
        if(!strcmp(bdi->chg_dev_name, "primary_chg"))
        {
	    power_supply_unregister(bdi->charger);
	}
out_pmrt:
	pm_runtime_put_sync(dev);
	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);
	return ret;
}
static int hl7015_remove(struct i2c_client *client)
{
	struct hl7015_dev_info *bdi = i2c_get_clientdata(client);
	int error;
	error = pm_runtime_get_sync(bdi->dev);
	if (error < 0) {
		dev_warn(bdi->dev, "pm_runtime_get failed: %i\n", error);
		pm_runtime_put_noidle(bdi->dev);
	}
	hl7015_register_reset(bdi);
        if(!strcmp(bdi->chg_dev_name, "primary_chg"))
        {
	    power_supply_unregister(bdi->charger);
	}
	if (error >= 0)
		pm_runtime_put_sync(bdi->dev);
	pm_runtime_dont_use_autosuspend(bdi->dev);
	pm_runtime_disable(bdi->dev);
	return 0;
}
static __maybe_unused int hl7015_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct hl7015_dev_info *bdi = i2c_get_clientdata(client);
	if (!bdi->initialized)
		return 0;
	dev_dbg(bdi->dev, "%s\n", __func__);
	return 0;
}
static __maybe_unused int hl7015_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct hl7015_dev_info *bdi = i2c_get_clientdata(client);
	if (!bdi->initialized)
		return 0;
	if (!bdi->irq_event && !strcmp(bdi->chg_dev_name, "primary_chg")) {
		dev_dbg(bdi->dev, "checking events on possible wakeirq\n");
	}
	return 0;
}
static __maybe_unused int hl7015_pm_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct hl7015_dev_info *bdi = i2c_get_clientdata(client);
	int error;
	error = pm_runtime_get_sync(bdi->dev);
	if (error < 0) {
		dev_warn(bdi->dev, "pm_runtime_get failed: %i\n", error);
		pm_runtime_put_noidle(bdi->dev);
	}
//	hl7015_register_reset(bdi);
	if (error >= 0) {
		pm_runtime_mark_last_busy(bdi->dev);
		pm_runtime_put_autosuspend(bdi->dev);
	}
	return 0;
}
static __maybe_unused int hl7015_pm_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct hl7015_dev_info *bdi = i2c_get_clientdata(client);
	int error;
	bdi->f_reg = 0;
	bdi->ss_reg = HL7015_REG_SS_VBUS_STAT_MASK; /* impossible state */
	error = pm_runtime_get_sync(bdi->dev);
	if (error < 0) {
		dev_warn(bdi->dev, "pm_runtime_get failed: %i\n", error);
		pm_runtime_put_noidle(bdi->dev);
	}
//	hl7015_register_reset(bdi);
//	hl7015_set_config(bdi);
	hl7015_read(bdi, HL7015_REG_SS, &bdi->ss_reg);
	if (error >= 0) {
		pm_runtime_mark_last_busy(bdi->dev);
		pm_runtime_put_autosuspend(bdi->dev);
	}
	/* Things may have changed while suspended so alert upper layer */
	if (!strcmp(bdi->chg_dev_name, "primary_chg")) {
	        power_supply_changed(bdi->charger);
	}
	return 0;
}
static const struct dev_pm_ops hl7015_pm_ops = {
	SET_RUNTIME_PM_OPS(hl7015_runtime_suspend, hl7015_runtime_resume,
			   NULL)
	SET_SYSTEM_SLEEP_PM_OPS(hl7015_pm_suspend, hl7015_pm_resume)
};
static const struct i2c_device_id hl7015_i2c_ids[] = {
	{ "hl7015" },
	{ },
};
MODULE_DEVICE_TABLE(i2c, hl7015_i2c_ids);
#ifdef CONFIG_OF
static const struct of_device_id hl7015_of_match[] = {
	{ .compatible = "mtk,hl7015", },
	{ },
};
MODULE_DEVICE_TABLE(of, hl7015_of_match);
#else
static const struct of_device_id hl7015_of_match[] = {
	{ },
};
#endif
static struct i2c_driver hl7015_driver = {
	.probe		= hl7015_probe,
	.remove		= hl7015_remove,
	.id_table	= hl7015_i2c_ids,
	.driver = {
		.name		= "hl7015-charger",
		.pm		= &hl7015_pm_ops,
		.of_match_table	= of_match_ptr(hl7015_of_match),
	},
};
module_i2c_driver(hl7015_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mark A. Greer <mgreer@animalcreek.com>");
MODULE_DESCRIPTION("TI HL7015 Charger Driver");

