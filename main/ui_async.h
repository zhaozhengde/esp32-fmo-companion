/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file ui_async.h
 * @brief LVGL UI 异步更新接口。
 *
 * 本模块用于从非 LVGL 上下文安全地请求 UI 更新。
 *
 * 可调用场景包括：
 * - WebSocket 回调
 * - JSON 解析任务
 * - 站点解析模块
 * - 音频播放任务
 * - WiFi RSSI 任务
 * - 电池监测任务
 * - 省电管理任务
 *
 * 内部通过 lv_async_call() 将请求切换回 LVGL 上下文，
 * 再调用 app_ui.c 中真正的 UI 更新函数。
 *
 * @note
 * 业务模块应优先调用本文件中的 ui_async_xxx() 接口，
 * 避免在后台 FreeRTOS 任务中直接操作 LVGL 对象。
 */

#ifndef UI_ASYNC_H
#define UI_ASYNC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Status and main screen updates                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief 异步更新底部状态栏文本。
 *
 * @param text 状态文本，可为 NULL。
 */
void ui_async_update_status(const char *text);

/**
 * @brief 异步更新当前说话人呼号。
 *
 * 兼容旧接口：talker 非空时认为处于说话状态，
 * talker 为空时认为未说话。
 *
 * @param talker 当前说话人呼号，可为 NULL。
 */
void ui_async_update_talker(const char *talker);

/**
 * @brief 异步更新当前说话人呼号和说话状态。
 *
 * @param talker 当前说话人呼号，可为 NULL。
 * @param active true 表示正在说话，false 表示未说话。
 */
void ui_async_update_talker_state(const char *talker, bool active);

/**
 * @brief 异步更新上次通联呼号。
 *
 * @param callsign 上次通联呼号，可为 NULL。
 */
void ui_async_update_last_call(const char *callsign);

/**
 * @brief 异步更新当前站点名称。
 *
 * @param station 当前站点名称，可为 NULL。
 */
void ui_async_update_station(const char *station);

/**
 * @brief 异步更新 QSO 数量。
 *
 * @param count QSO 数量。
 */
void ui_async_update_qso_count(uint32_t count);

/**
 * @brief 通知 UI 站点列表缓存已更新。
 *
 * station_parser 解析完成并更新缓存后，可调用该接口刷新当前页面。
 */
void ui_async_station_list_updated(void);

/* -------------------------------------------------------------------------- */
/* WiFi, battery and audio updates                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief 异步更新 WiFi RSSI 显示。
 *
 * @param rssi_dbm RSSI，单位 dBm。
 */
void ui_async_update_wifi_rssi(int rssi_dbm);

/**
 * @brief 异步更新电池状态显示。
 *
 * @param percent 电量百分比，范围 0~100。
 * @param charging true 表示正在充电。
 */
void ui_async_update_battery(uint8_t percent, bool charging);

/**
 * @brief 更新音频电平。
 *
 * @param level 音频电平，范围 0~100。
 *
 * @note
 * 当前版本主界面音频电平条已取消，app_ui 中对应接口为空实现。
 * 如果后续恢复高频音频电平显示，建议继续避免频繁使用 lv_async_call()，
 * 可采用 pending 变量 + LVGL timer 的方式刷新。
 */
void ui_async_update_voice_level(uint8_t level);

/* -------------------------------------------------------------------------- */
/* Backlight and idle / power-save clock                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief 异步设置屏幕背光百分比。
 *
 * @param percent 背光百分比，范围 0~100。
 */
void ui_async_set_backlight_percent(uint8_t percent);

/**
 * @brief 异步从普通待机状态唤醒。
 */
void ui_async_wake_from_idle(void);

/**
 * @brief 异步进入省电时钟页。
 */
void ui_async_enter_power_save_clock(void);

/**
 * @brief 异步退出省电时钟页。
 */
void ui_async_exit_power_save_clock(void);

/* -------------------------------------------------------------------------- */
/* QSO sync popup                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief 异步显示 QSO 同步提示弹窗。
 *
 * @param text 弹窗文本，可为 NULL。
 * @param auto_close_ms 自动关闭时间，单位 ms。
 *                      0 表示不自动关闭。
 */
void ui_async_qso_sync_popup_show(const char *text, uint32_t auto_close_ms);

/**
 * @brief 异步关闭 QSO 同步提示弹窗。
 */
void ui_async_qso_sync_popup_close(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_ASYNC_H */
