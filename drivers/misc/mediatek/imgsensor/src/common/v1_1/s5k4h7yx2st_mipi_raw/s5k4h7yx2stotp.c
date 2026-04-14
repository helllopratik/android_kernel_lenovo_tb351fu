// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

/*
 * NOTE:
 * The modification is appended to initialization of image sensor.
 * After sensor initialization, use the function
 * bool otp_update_wb(unsigned char golden_rg, unsigned char golden_bg)
 * and
 * bool otp_update_lenc(void)
 * and then the calibration of AWB & LSC & BLC will be applied.
 * After finishing the OTP written, we will provide you the typical
 * value of golden sample.
 */

#include <linux/videodev2.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/slab.h>

//#ifndef VENDOR_EDIT
//#include "kd_camera_hw.h"
/*Caohua.Lin@Camera.Drv, 20180126 remove to adapt with mt6771*/
//#endif
#include "kd_imgsensor.h"
#include "kd_imgsensor_define.h"
#include "kd_imgsensor_errcode.h"
#include "kd_camera_typedef.h"

#include "s5k4h7yx2st_mipi_raw_Sensor.h"
#include "s5k4h7yx2stotp.h"
#if IS_ENABLED(CONFIG_TINNO_DEVINFO)
#include "../../../../../../../oem/dev_info/dev_info.h"
#endif

/******************Modify Following Strings for Debug*******************/
#define PFX "S5K4H7YX2STOTP"
#define LOG_1 SENSORDB("S5K4H7YX2ST,MIPI CAM\n")
#define LOG_INF(format, args...) \
	pr_debug(PFX "[%s] " format, __func__, ##args)
/*********************   Modify end    *********************************/

#define USHORT        unsigned short
#define BYTE          unsigned char
#define I2C_ID          0x20
#define S5K4H7YX2ST_MIFAWB_GROUP_CNT 3
#define S5K4H7YX2ST_LSC_GROUP_CNT 2
char s5k4h7yx2st_cameraSn[24] = {0};

unsigned char ucMIFAWB_data[64] = {0x00};
static kal_uint16 read_cmos_sensor_8(kal_uint16 addr)
{
	kal_uint16 get_byte = 0;
	char pusendcmd[2] = { (char)(addr >> 8), (char)(addr & 0xFF) };

	iReadRegI2C(pusendcmd, 2, (u8 *) &get_byte, 1, I2C_ID);
	return get_byte;
}


static void write_cmos_sensor_8(kal_uint16 addr, kal_uint8 para)
{
	char pusendcmd[4] = {
		(char)(addr >> 8),
		(char)(addr & 0xFF),
		(char)(para & 0xFF) };

	iWriteRegI2C(pusendcmd, 3, I2C_ID);
}

/*************************************************************************
 * Function    :  wb_gain_set
 * Description :  Set WB ratio to register gain setting  512x
 * Parameters  :  [int] r_ratio : R ratio data compared with golden module R
 *                b_ratio : B ratio data compared with golden module B
 * Return      :  [bool] 0 : set wb fail 1 : WB set success
 *************************************************************************/
bool wb_gain_set(kal_uint32 r_ratio, kal_uint32 b_ratio)
{

	kal_uint32 R_GAIN = 0;
	kal_uint32 B_GAIN = 0;
	kal_uint32 Gr_GAIN = 0;
	kal_uint32 Gb_GAIN = 0;
	kal_uint32 G_GAIN = 0;
	kal_uint32 GAIN_DEFAULT = 0x0100;

	if (!r_ratio || !b_ratio) {
		LOG_INF(" OTP WB ratio Data Err!");
		return 0;
	}
	if (r_ratio >= 512) {
		if (b_ratio >= 512) {
			R_GAIN = (USHORT) (GAIN_DEFAULT * r_ratio / 512);
			G_GAIN = GAIN_DEFAULT;
			B_GAIN = (USHORT) (GAIN_DEFAULT * b_ratio / 512);
		} else {
			R_GAIN = (USHORT) (GAIN_DEFAULT * r_ratio / b_ratio);
			G_GAIN = (USHORT) (GAIN_DEFAULT * 512 / b_ratio);
			B_GAIN = GAIN_DEFAULT;
		}
	} else {
		if (b_ratio >= 512) {
			R_GAIN = GAIN_DEFAULT;
			G_GAIN = (USHORT) (GAIN_DEFAULT * 512 / r_ratio);
			B_GAIN = (USHORT) (GAIN_DEFAULT * b_ratio / r_ratio);
		} else {
			Gr_GAIN = (USHORT) (GAIN_DEFAULT * 512 / r_ratio);
			Gb_GAIN = (USHORT) (GAIN_DEFAULT * 512 / b_ratio);
			if (Gr_GAIN >= Gb_GAIN) {
				R_GAIN = GAIN_DEFAULT;
				G_GAIN = (USHORT)
					(GAIN_DEFAULT * 512 / r_ratio);
				B_GAIN = (USHORT)
					(GAIN_DEFAULT * b_ratio / r_ratio);
			} else {
				R_GAIN = (USHORT)
					(GAIN_DEFAULT * r_ratio / b_ratio);
				G_GAIN = (USHORT)
					(GAIN_DEFAULT * 512 / b_ratio);
				B_GAIN = GAIN_DEFAULT;
			}
		}
	}

	write_cmos_sensor_8(0x3C0F, 0x01);
	if (R_GAIN > GAIN_DEFAULT) {
		write_cmos_sensor_8(0x0210, (R_GAIN >> 8) & 0x0F);
		write_cmos_sensor_8(0x0211, R_GAIN & 0xFF);
	}
	if (B_GAIN > GAIN_DEFAULT) {
		write_cmos_sensor_8(0x0212, (B_GAIN >> 8) & 0x0F);
		write_cmos_sensor_8(0x0213, B_GAIN & 0xFF);
	}
	if (G_GAIN > GAIN_DEFAULT) {
		write_cmos_sensor_8(0x020E, (G_GAIN >> 8) & 0x0F);
		write_cmos_sensor_8(0x020F, G_GAIN & 0xFF);
		write_cmos_sensor_8(0x0214, (G_GAIN >> 8) & 0x0F);
		write_cmos_sensor_8(0x0215, G_GAIN & 0xFF);
	}
	LOG_INF("OTP enable awb success\n");
#if 1
	LOG_INF("r_gain = %d , g_gain = %d , b_gain = %d \n",
	read_cmos_sensor_8(0x210) << 8 | read_cmos_sensor_8(0x211),
	read_cmos_sensor_8(0x20E) << 8 | read_cmos_sensor_8(0x20F),
	read_cmos_sensor_8(0x212) << 8 | read_cmos_sensor_8(0x213)
	);
#endif
	return 1;
}

/*********************************************************
 *s5k4h7yx2st_apply_otp_lsc
 * ******************************************************/
void s5k4h7yx2st_apply_otp_lsc(unsigned int group)
{
		if (1 == group){
			write_cmos_sensor_8(0x0A3D, 0x01);
		} else {
			write_cmos_sensor_8(0x0A3D, 0x03);
		}
        write_cmos_sensor_8(0x3400, 0x00);
        write_cmos_sensor_8(0x0B00, 0x01);
		LOG_INF("OTP enable lsc success\n");
}

bool S5K4H7YX2ST_update_awb(unsigned char page)
{
	int i = 0;
	unsigned int uiChecksum_cal = 0;
	unsigned char uiChecksum_read = 0;
	unsigned char moduleData[28] = {0x00};

	write_cmos_sensor_8(0x0A02, page);
	write_cmos_sensor_8(0x0A00, 0x01);
	mDELAY(10);
	for(i = 0; i < 28; i++)
	{
		moduleData[i] = read_cmos_sensor_8(0x0A24 + i);
		uiChecksum_cal += moduleData[i];
	}
	uiChecksum_cal = (char)(uiChecksum_cal%255 + 1);
	uiChecksum_read = read_cmos_sensor_8(0x0A40);

	if (uiChecksum_cal == uiChecksum_read) {
		kal_uint32 r_ratio;
		kal_uint32 b_ratio;

		kal_uint32 unit_rg = moduleData[1] | moduleData[0] << 8;
		kal_uint32 unit_bg = moduleData[3] | moduleData[2] << 8;
		kal_uint32 unit_GrGb = moduleData[5] | moduleData[4] << 8;

		kal_uint32 golden_rg = moduleData[7] | moduleData[6] << 8;
		kal_uint32 golden_bg = moduleData[9] | moduleData[8] << 8;
		kal_uint32 golden_GrGb = moduleData[11] | moduleData[10] << 8;


		kal_uint32 unit_r = moduleData[13] | moduleData[12] << 8;
		kal_uint32 unit_Gr = moduleData[15] | moduleData[14] << 8;
		kal_uint32 unit_Gb = moduleData[17] | moduleData[16] << 8;
		kal_uint32 unit_b = moduleData[19] | moduleData[18] << 8;

		kal_uint32 golden_r = moduleData[21] | moduleData[20] << 8;
		kal_uint32 golden_Gr = moduleData[23] | moduleData[22] << 8;
		kal_uint32 golden_Gb = moduleData[25] | moduleData[24] << 8;
		kal_uint32 golden_b = moduleData[27] | moduleData[26] << 8;

		LOG_INF(
			"updata wb golden_rg=0x%02x golden_bg=0x%02x golden_GrGb=0x%02x unit_rg=0x%02x unit_bg =0x%02x unit_GrGb =0x%02x\n",
			golden_rg, golden_bg, golden_GrGb, unit_rg, unit_bg, unit_GrGb);

		LOG_INF(
			"updata wb golden_r=0x%02x golden_Gr=0x%02x golden_Gb=0x%02x golden_b=0x%02x unit_r=0x%02x unit_Gr =0x%02x unit_Gb =0x%02x unit_b =0x%02x\n",
			golden_r, golden_Gr, golden_Gb, golden_b, unit_r, unit_Gr, unit_Gb, unit_b);

		if (!golden_rg || !golden_bg || !unit_rg || !unit_bg) {
			LOG_INF("updata wb err");
			return 0;
		}
		r_ratio = 1023 * (golden_rg) / (unit_rg);
		b_ratio = 1023 * (golden_bg) / (unit_bg);
		wb_gain_set(r_ratio, b_ratio);
	} else {
		LOG_INF("awb info read checksum 0x%02x not match calculated checksum 0x%02x!", uiChecksum_read, uiChecksum_cal);
	}
	return 1;
}


void s5k4h7yx2st_get_module_info(unsigned char page)
{
	int i = 0;
	unsigned int uiChecksum_cal = 0;
	unsigned char uiChecksum_read = 0;
	unsigned char moduleData[29] = {0x00};

	write_cmos_sensor_8(0x0A02, page);
	write_cmos_sensor_8(0x0A00, 0x01);
	mDELAY(10);
	for(i = 0; i < 29; i++)
	{
		moduleData[i] = read_cmos_sensor_8(0x0A05 + i);
		uiChecksum_cal += moduleData[i];
	}
	uiChecksum_cal = (char)(uiChecksum_cal%255 + 1);
	uiChecksum_read = read_cmos_sensor_8(0x0A22);

	if (uiChecksum_cal == uiChecksum_read) {
		LOG_INF("MID = 0x%02x",moduleData[0]);
		LOG_INF("Lenovo PN[0] = 0x%02x",moduleData[1]);
		LOG_INF("Lenovo PN[1] = 0x%02x",moduleData[2]);
		LOG_INF("Lenovo PN[2] = 0x%02x",moduleData[3]);
		LOG_INF("Sensor ID = 0x%02x",moduleData[4]);
		LOG_INF("Lens ID = 0x%02x",moduleData[21]);
		LOG_INF("VCM ID = 0x%02x",moduleData[22]);
		LOG_INF("Year = %d",moduleData[24]+2000);
		LOG_INF("Month = %d",moduleData[25]);
		LOG_INF("Phase = 0x%02x",moduleData[26]);
		LOG_INF("Mirror/Flip Status = 0x%02x",moduleData[27]);
		LOG_INF("IR filter ID = 0x%02x",moduleData[28]);
		sprintf(s5k4h7yx2st_cameraSn, "8SSC28%02X%02X%02X0000%01d%01X0%02X%02X",
			moduleData[1],moduleData[2],
			moduleData[3], (moduleData[24] % 10),
			moduleData[25], moduleData[4], moduleData[21]);
#if IS_ENABLED(CONFIG_TINNO_DEVINFO)
		FULL_PRODUCT_DEVICE_INFO(ID_FRONT1_CAM_SN, s5k4h7yx2st_cameraSn);
#endif
	} else {
		LOG_INF("module info read checksum 0x%02x not match calculated checksum 0x%02x!", uiChecksum_read, uiChecksum_cal);
	}

}

bool s5k4h7yx2st_mifawb_datacheck(unsigned char * ucMIFAWB_data)
{

	unsigned int uiChecksum_cal = 0;
	unsigned int uiChecksum_read = 0;
	int i = 0;
	for(i = 1; i < 41; i++)
	{
		uiChecksum_cal += ucMIFAWB_data[i];
		LOG_INF("MIF&AWB data[%d] = 0x%02x\n",i,ucMIFAWB_data[i]);
	}
	uiChecksum_cal = uiChecksum_cal % 255 + 1;
	uiChecksum_read = ucMIFAWB_data[41];

	LOG_INF("MIF&AWB calculate checksum = 0x%02x\n",uiChecksum_cal);
	LOG_INF("MIF&AWB read checksum = 0x%02x\n",uiChecksum_read);
	if(uiChecksum_cal != uiChecksum_read)
	{
		LOG_INF("MIF&AWB read checksum not match calculated checksum!");
		return 0;
	}
	return 1;

}

bool s5k4h7yx2st_lsc_datacheck(unsigned int group)
{
	unsigned char ucLSCData[1024] = {0x00};
	unsigned int uiBatchData = 0;
	unsigned int uiChecksum_cal = 0;
	unsigned int uiChecksum_read = 0;
	int i = 0;
	int page = 0;
	for(page = 1; page < 13; page++ )
	{
		write_cmos_sensor_8(0x0A02, page);
		write_cmos_sensor_8(0x0A00, 0x01);
		mDELAY(10);
		for(i = 0; i < 64; i++)
		{
			ucLSCData[uiBatchData++] = read_cmos_sensor_8(0x0A04 + i);
		}
	}

	write_cmos_sensor_8(0x0A02, 0x1b);
	write_cmos_sensor_8(0x0A00, 0x01);
	mDELAY(10);
	if(1 == group)
	{
		LOG_INF("LSC group 1 valied!\n");
		uiChecksum_read = read_cmos_sensor_8(0x0A06);
		for(i = 0; i < 360; i++)
		{
			uiChecksum_cal += ucLSCData[i];
			//LOG_INF("LSC data[%d] = 0x%02x\n",i,ucLSCData[i]);
		}
		uiChecksum_cal = uiChecksum_cal % 255 + 1;
	}
	else
	{
		LOG_INF("LSC group 2 valied!\n");
		uiChecksum_read = read_cmos_sensor_8(0x0A07);
		for(i = 360; i < 720; i++)
		{
			uiChecksum_cal += ucLSCData[i];
			//LOG_INF("LSC data[%d] = 0x%02x\n",i,ucLSCData[i]);
		}
		uiChecksum_cal = uiChecksum_cal % 255 + 1;
	}
	LOG_INF("LSC calculate checksum = 0x%02x\n",uiChecksum_cal);
	LOG_INF("LSC read checksum = 0x%02x\n",uiChecksum_read);
	if(uiChecksum_cal != uiChecksum_read)
	{
		LOG_INF("LSC read checksum not match calculated checksum!");
		return 0;
	}
	else
	{
		s5k4h7yx2st_apply_otp_lsc(group);
	}
	return 1;
}

bool s5k4h7yx2st_get_module_lsc(void)
{
	unsigned char flag[2] = {0x00};
	unsigned char page = 0x1b;
	unsigned int group = 0;
	write_cmos_sensor_8(0x0A02, page);
	write_cmos_sensor_8(0x0A00, 0x01);
	mDELAY(10);
	flag[0] = read_cmos_sensor_8(0x0A04);
	flag[1] = read_cmos_sensor_8(0x0A05);
	if(0x00 == flag[0]&&(0x00 == flag[1]))
	{
		LOG_INF("group 1 and group 2 are both enpty!\n");
		return 0;
	}
	else if((0x40 == flag[0])&&(0x00 == flag[1]))
	{
		LOG_INF("group 1 is valied!\n");
		group = 1;
	}
	else if((0xC0 == flag[0])&&(0x40 == flag[1]))
	{
		LOG_INF("group 2 is valied!\n");
		group = 2;
	}
	else if((0xC0 == flag[0])&&(0xC0 == flag[1]))
	{
		LOG_INF("group 2 is invalied,there are only two groups of space in the LSC!\n");
		return 0;
	}

	return s5k4h7yx2st_lsc_datacheck(group);
}

void s5k4h7yx2st_get_chip_id(void)
{
	char chip_id[8] = {0x00};
	int i = 0;

	write_cmos_sensor_8(0x0136, 0x18);	// 24MHz
	write_cmos_sensor_8(0x0137, 0x00);
	write_cmos_sensor_8(0x0305, 0x06);	// PLL pre div
	write_cmos_sensor_8(0x0306, 0x00);	//PLL multiplier
	write_cmos_sensor_8(0x0307, 0x8C);

	write_cmos_sensor_8(0x030D, 0x06);	// second_pre_pll_clk_div

	write_cmos_sensor_8(0x030E, 0x00);	// second_pll_multiplier
	write_cmos_sensor_8(0x030F, 0xAF);	// second_pll_multiplier
	write_cmos_sensor_8(0x0301, 0x04);	// vt_pix_clk_div

	//Streaming ON
	write_cmos_sensor_8(0x0100, 0x01);	// Streaming ON
	mDELAY(10);

	write_cmos_sensor_8(0x0A02, 0x15);	// page 21
	write_cmos_sensor_8(0x0A00, 0x01);
	mDELAY(10);

	LOG_INF("S5K4H7YX2ST chip_id:");
	for(i = 0; i < 8; i++)
	{
		chip_id[i] = read_cmos_sensor_8(0x0A04 + i);
		LOG_INF("%02x",chip_id[i]);
	}
	LOG_INF("\n");
}
/***************************************************************************
 * Function    :  otp_update_wb
 * Description :  Update white balance settings from OTP
 * Parameters  :  [in] golden_rg : R/G of golden camera module
 [in] golden_bg : B/G of golden camera module
 * Return      :  1, success; 0, fail
 ***************************************************************************/
bool S5K4H7YX2ST_otp_update(void)
{
	unsigned int group = 0;
	unsigned char page = 0;
	unsigned char moduleFlag[3] = {0x00};
	unsigned char awbFlag[3] = {0x00};

	write_cmos_sensor_8(0x0136, 0x18);	// 24MHz
	write_cmos_sensor_8(0x0137, 0x00);
	write_cmos_sensor_8(0x0305, 0x06);	// PLL pre div
	write_cmos_sensor_8(0x0306, 0x00);	//PLL multiplier
	write_cmos_sensor_8(0x0307, 0x8C);

	write_cmos_sensor_8(0x030D, 0x06);	// second_pre_pll_clk_div

	write_cmos_sensor_8(0x030E, 0x00);	// second_pll_multiplier
	write_cmos_sensor_8(0x030F, 0xAF);	// second_pll_multiplier
	write_cmos_sensor_8(0x0301, 0x04);	// vt_pix_clk_div

	//Streaming ON
	write_cmos_sensor_8(0x0100, 0x01);	// Streaming ON
	mDELAY(50);

	page = 0x15;
	for (group = 0;group < 3; group++)	{
		page += group * 2;
		write_cmos_sensor_8(0x0A02, page);
		write_cmos_sensor_8(0x0A00, 0x01);
		mDELAY(10);
		moduleFlag[group] = read_cmos_sensor_8(0x0A04);
		awbFlag[group] = read_cmos_sensor_8(0x0A23);
		LOG_INF("group %d moduleFlag 0x%x awbFlag 0x%x!\n", group, moduleFlag[group], awbFlag[group]);
	}

	if(0x40 == moduleFlag[0]) {
		LOG_INF("group 1 is valied!\n");
		group = 1;
	} else if (0x40 == moduleFlag[1]) {
		LOG_INF("group 2 is valied!\n");
		group = 2;
	} else if (0x40 == moduleFlag[2]) {
		LOG_INF("group 3 is valied!\n");
		group = 3;
	} else {
		LOG_INF("all module group are invalied!\n");
	}

	page = 0x15 + (group - 1)*2;

	// read module process
    s5k4h7yx2st_get_module_info(page);

    // read awb process
	if(0x40 == awbFlag[0]) {
		LOG_INF("group 1 is valied!\n");
		group = 1;
	} else if (0x40 == awbFlag[1]) {
		LOG_INF("group 2 is valied!\n");
		group = 2;
	} else if (0x40 == awbFlag[2]) {
		LOG_INF("group 3 is valied!\n");
		group = 3;
	} else {
		LOG_INF("all module group are invalied!\n");
	}

	page = 0x15 + (group - 1)*2;
    S5K4H7YX2ST_update_awb(page);

    // read lsc process
	s5k4h7yx2st_get_module_lsc();

    //stop read process
    write_cmos_sensor_8(0x0A00, 0x04);
    write_cmos_sensor_8(0x0A00, 0x00);

	return 0;
}



unsigned char s5k4h7yx2st_get_module_id(void)
{
	unsigned char module_id = 0;

	write_cmos_sensor_8(0x0136, 0x18);	// 24MHz
	write_cmos_sensor_8(0x0137, 0x00);
	write_cmos_sensor_8(0x0305, 0x06);	// PLL pre div
	write_cmos_sensor_8(0x0306, 0x00);	//PLL multiplier
	write_cmos_sensor_8(0x0307, 0x8C);

	write_cmos_sensor_8(0x030D, 0x06);	// second_pre_pll_clk_div

	write_cmos_sensor_8(0x030E, 0x00);	// second_pll_multiplier
	write_cmos_sensor_8(0x030F, 0xAF);	// second_pll_multiplier
	write_cmos_sensor_8(0x0301, 0x04);	// vt_pix_clk_div

	//Streaming ON
	write_cmos_sensor_8(0x0100, 0x01);	// Streaming ON
	mDELAY(10);

	write_cmos_sensor_8(0x0A02, 0x15);	// page 21
	write_cmos_sensor_8(0x0A00, 0x01);
	mDELAY(10);

	module_id = read_cmos_sensor_8(0x0A04);
	LOG_INF("S5K4H7YX2ST module id = 0x%02x\n", module_id);
	return module_id;
}

unsigned int s5k4h7yx2st_read_otpdata(struct i2c_client *client, unsigned int addr, unsigned char *data, unsigned int size)
{
	int i = 0;
	LOG_INF("otpdata addr %d, size_input %d",addr, size);
	if(size > 42)
	{
		size = 42;
	}
	LOG_INF("otpdata addr %d, size %d",addr, size);
	for(i = 0; i < size; i++)
	{
		data[i] = ucMIFAWB_data[addr + i];
		LOG_INF("otpdata[%d] otp_info 0x%02x,data 0x%02x",i, data[i], ucMIFAWB_data[addr + i]);
	}
	return size;
}
EXPORT_SYMBOL(s5k4h7yx2st_read_otpdata);
