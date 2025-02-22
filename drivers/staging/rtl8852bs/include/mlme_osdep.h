/******************************************************************************
 *
 * Copyright(c) 2007 - 2019 Realtek Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 *****************************************************************************/
#ifndef	__MLME_OSDEP_H_
#define __MLME_OSDEP_H_

extern void rtw_os_indicate_disconnect(_adapter *adapter, u16 reason, u8 locally_generated);
extern void rtw_os_indicate_connect(_adapter *adapter);
void rtw_os_indicate_scan_done(_adapter *padapter, bool aborted);
extern void rtw_report_sec_ie(_adapter *adapter, u8 authmode, u8 *sec_ie);

void rtw_reset_securitypriv(_adapter *adapter);

#ifdef CONFIG_DFS_MASTER
void rtw_os_indicate_radar_detected(struct rf_ctl_t *rfctl, u8 band_idx
	, u8 cch, enum channel_width bw);
void rtw_os_indicate_cac_started(struct rf_ctl_t *rfctl, u8 band_idx
	, u8 ifbmp, u8 cch, enum channel_width bw);
void rtw_os_indicate_cac_finished(struct rf_ctl_t *rfctl, u8 band_idx
	, u8 ifbmp, u8 cch, enum channel_width bw);
void rtw_os_indicate_cac_aborted(struct rf_ctl_t *rfctl, u8 band_idx
	, u8 ifbmp, u8 cch, enum channel_width bw);
void rtw_os_force_cac_finished(struct rf_ctl_t *rfctl, u8 band_idx
	, u8 ifbmp, u8 cch, enum channel_width bw);
void rtw_os_indicate_nop_finished(struct rf_ctl_t *rfctl, u8 band_idx
	, u8 band, u8 cch, enum channel_width bw);
void rtw_os_indicate_nop_started(struct rf_ctl_t *rfctl, u8 band_idx
	, u8 band, u8 cch, enum channel_width bw, bool called_on_cmd_thd);
#endif

#endif /* _MLME_OSDEP_H_ */
