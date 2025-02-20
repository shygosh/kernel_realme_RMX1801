/************************************************************************************
** File: - android\kernel\include\soc\oppo\mmkey_log.h
** CONFIG_OPPO_VENDOR_EDIT
** Copyright (C), 2008-2015, OPPO Mobile Comm Corp., Ltd
**
** Description:
**      oppo key log multimedia issue id header file
** Version: 1.0
** --------------------------- Revision History: --------------------------------
** 	<author>	<data>			<desc>
** John.Xu@multimedia.audiodriver 10/23/2015 add for OPPO key log
************************************************************************************/
#ifndef MMKEYLOG_H_
#define MMKEYLOG_H_
//MultiMedia issue type range is 200~399,
//mediaser and surfaceflinger use 200~299
//kernel use 300~399
enum mmkeylog_issue {
	TYPE_SOUND_CARD_REGISTER_FAIL = 300,
	TYPE_ADSP_LOAD_FAIL,
	TYPE_SMART_PA_EXCEPTION,
	TYPE_NO_DATA_TO_SHOW,
	TYPE_KGSL_EXCEPTION,
	TYPE_VSYNC_EXCEPTION,
	TYPE_ESD_EXCEPTION,
	TYPE_GPU_EXCEPTION,
	TYPE_IOMMU_ERROR,
	TYPE_FENCE_TIMEOUT,
	TYPE_BL_EXCEPTION,
	TYPE_ADSP_CLK_OPEN_TIMEOUT,
	TYPE_HP_PA_EXCEPTION,
};

//Yadong.Hu@Prd.Svc.Wifi, 2016/01/04, Add for wifi critical log
enum conkeylog_issue {
	TYPE_SYMBOL_VERSION_DISAGREE = 803,
	TYPE_WDI_EXCEPTION,
};

//Canjie.Zheng@Swdp.Android.OppoDebug.CriticalLog,2016/06/03,add for critical
//record subSystem crash
enum androidlog_issue {
	TYPE_SUBSYSTEM_RESTART = 1001,
};

extern void mm_keylog_write(const char *logmessage, const char *cause, int id);
#endif
