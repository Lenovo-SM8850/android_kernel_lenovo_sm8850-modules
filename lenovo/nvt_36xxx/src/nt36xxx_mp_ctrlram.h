/*
 * Copyright (C) 2024 Novatek, Inc.
 *
 * $Revision: 132694 $
 * $Date: 2024-01-16 08:16:46 +0800 (¶g¤G, 16 ¤@¤ë 2024) $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#if NVT_TOUCH_MP
#define NVT_SELFTEST "nvt_selftest"
#define nvt_mp_printf(fmt, args...) do { \
	if (!nvt_mp_test_result_printed) \
		printk(fmt, ##args); \
} while (0)

/* TB323FU stock fallback criteria; DT overrides these for the detected PID. */
#define X_Y_DIMENSION_MAX (72 * 72)
#define PEN_X_Y_DIMENSION_MAX 1024
static uint32_t IC_KEY_CFG_SIZE = 4;
static uint32_t Key_Channel = TOUCH_KEY_NUM;
static uint32_t IC_X_CFG_SIZE = 32;
static uint32_t IC_Y_CFG_SIZE = 48;
static uint32_t X_Channel = 32;
static uint32_t Y_Channel = 48;
static int32_t PS_Config_Diff_Test_Frame = 50;
static uint8_t AIN_X[64] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
static uint8_t AIN_Y[64] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static int32_t PS_Config_Lmt_Short_Rawdata_P[X_Y_DIMENSION_MAX] = {
	[0 ... 2559] = 14008,
};

static int32_t PS_Config_Lmt_Short_Rawdata_N[X_Y_DIMENSION_MAX] = {
	[0 ... 2559] = 10000,
};

static int32_t PS_Config_Lmt_Open_Rawdata_P[X_Y_DIMENSION_MAX] = {
	[0 ... 2559] = 5120,
};

static int32_t PS_Config_Lmt_Open_Rawdata_N[X_Y_DIMENSION_MAX] = {
	[0 ... 2559] = -511,
};

static int32_t PS_Config_Lmt_FW_Rawdata_P[X_Y_DIMENSION_MAX] = {
	[0 ... 2559] = 3840,
};

static int32_t PS_Config_Lmt_FW_Rawdata_N[X_Y_DIMENSION_MAX] = {
	[0 ... 2559] = 240,
};

static int32_t PS_Config_Lmt_FW_CC_P[X_Y_DIMENSION_MAX] = {
	[0 ... 2559] = 314,
};

static int32_t PS_Config_Lmt_FW_CC_N[X_Y_DIMENSION_MAX] = {
};

static int32_t PS_Config_Lmt_FW_Diff_P[X_Y_DIMENSION_MAX] = {
	[0 ... 2559] = 75,
};

static int32_t PS_Config_Lmt_FW_Diff_N[X_Y_DIMENSION_MAX] = {
	[0 ... 2559] = -75,
};

static int32_t PS_Config_Lmt_PenTipX_FW_Raw_P[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 479] = 1050,
};

static int32_t PS_Config_Lmt_PenTipX_FW_Raw_N[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 479] = -1050,
};

static int32_t PS_Config_Lmt_PenTipY_FW_Raw_P[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 511] = 1050,
};

static int32_t PS_Config_Lmt_PenTipY_FW_Raw_N[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 511] = -1050,
};

static int32_t PS_Config_Lmt_PenRingX_FW_Raw_P[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 479] = 1050,
};

static int32_t PS_Config_Lmt_PenRingX_FW_Raw_N[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 479] = -1050,
};

static int32_t PS_Config_Lmt_PenRingY_FW_Raw_P[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 511] = 1050,
};

static int32_t PS_Config_Lmt_PenRingY_FW_Raw_N[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 511] = -1050,
};

static int32_t PS_Config_Lmt_PenTipX_FW_Diff_P[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 479] = 1050,
};

static int32_t PS_Config_Lmt_PenTipX_FW_Diff_N[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 479] = -1050,
};

static int32_t PS_Config_Lmt_PenTipY_FW_Diff_P[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 511] = 1050,
};

static int32_t PS_Config_Lmt_PenTipY_FW_Diff_N[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 511] = -1050,
};

static int32_t PS_Config_Lmt_PenRingX_FW_Diff_P[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 479] = 1050,
};

static int32_t PS_Config_Lmt_PenRingX_FW_Diff_N[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 479] = -1050,
};

static int32_t PS_Config_Lmt_PenRingY_FW_Diff_P[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 511] = 1050,
};

static int32_t PS_Config_Lmt_PenRingY_FW_Diff_N[PEN_X_Y_DIMENSION_MAX] = {
	[0 ... 511] = -1050,
};

#endif /* NVT_TOUCH_MP */
