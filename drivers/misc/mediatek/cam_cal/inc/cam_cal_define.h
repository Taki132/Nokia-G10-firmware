/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */
#ifndef _CAM_CAL_DATA_H
#define _CAM_CAL_DATA_H

#ifdef CONFIG_COMPAT
/* 64 bit */
#include <linux/fs.h>
#include <linux/compat.h>
#endif

struct CAM_CAL_SENSOR_INFO {
	u32 sensor_id;
};

//+bug591033 cdzk, add, 2020/10/27, the otp porting
struct stCAM_CAL_DATAINFO_STRUCT{
	u32 sensorID; // Sensor ID
	u32 deviceID; // MAIN = 0x01, SUB  = 0x02, MAIN_2 = 0x04
	u32 dataLength; //Data len
	u32 sensorVendorid; // Module ID | Pos ID | Vcm ID | Len ID
	u8  vendorByte[4]; // Module ID offset, Pos ID offset, Vcm ID offset,  Len ID offset
	u8  *dataBuffer; //It's need malloc dataLength cache
};

typedef enum{
	MODULE_ITEM = 0,
	AWB_ITEM,
	SEGMENT_ITEM,
	AF_ITEM,
	LSC_ITEM,
	PDAF_ITEM,
	//+bug584789 chenbocheng.wt, add, 2020/10/30, add main camera s5kgm1 and wide camera hi556 otp code
	PDAF_PROC2_ITEM,
	//+bug584789 chenbocheng.wt, add, 2020/11/16, add new otp items(PGC,QGC,XGC) for 2nd supply main camera hi4821q
	HI4821Q_PGC,  //Correct the PD pixel above the green color
	HI4821Q_QGC,  //Grid pattern is improved by reducing the deviation between 2x2 adjacent color
	HI4821Q_XGC,  //Correct XTALK pixels around Paired PD pixel
	//+bug584789 chenbocheng.wt, add, 2020/11/16, add main camera s5kgm1 and wide camera hi556 otp code
	DUALCAM_ITEM,
	TOTAL_ITEM,
	MAX_ITEM,
}stCAM_CAL_CHECKSUM_ITEM;

struct stCAM_CAL_CHECKSUM_STRUCT{
	stCAM_CAL_CHECKSUM_ITEM item;
	u32 flagAdrees;
	u32 startAdress;
	u32 endAdress;
	u32 checksumAdress;
	u8  validFlag;
};
//-bug591033 cdzk, add, 2020/10/27, the otp porting

struct stCAM_CAL_INFO_STRUCT {
	u32 u4Offset;
	u32 u4Length;
	u32 sensorID;
	/*
	 * MAIN = 0x01,
	 * SUB  = 0x02,
	 * MAIN_2 = 0x04,
	 * SUB_2 = 0x08,
	 * MAIN_3 = 0x10,
	 */
	u32 deviceID;
	u8 *pu1Params;
};

#ifdef CONFIG_COMPAT

struct COMPAT_stCAM_CAL_INFO_STRUCT {
	u32 u4Offset;
	u32 u4Length;
	u32 sensorID;
	u32 deviceID;
	compat_uptr_t pu1Params;
};
#endif

#endif/*_CAM_CAL_DATA_H*/
