/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file app_ui.h
 * @brief 应用主界面与 UI 更新接口。
 *
 * 本模块负责创建 LVGL 主界面、设置页、待机时钟页以及相关弹窗，
 * 并向其他业务模块提供线程安全封装层之外的 UI 更新接口。
 *
 * @note
 * LVGL 通常不是线程安全的。除 LVGL 主线程/定时器上下文外，
 * 其他 FreeRTOS 任务不应直接调用本文件中的 UI 更新函数。
 * 后台任务建议通过 ui_async.c 提供的异步接口切回 LVGL 上下文后再更新 UI。
 */

#ifndef APP_UI_H
#define APP_UI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* UI creation                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 创建应用主界面。
 *
 * 该函数会清空当前 LVGL 屏幕，并创建：
 * - 主界面
 * - 当前呼号显示区
 * - 上次通联与 QSO 数量区域
 * - 当前站点显示条
 * - 底部状态栏与操作按钮
 * - 设置页面
 * - 待机时钟页面
 * - 站点选择弹窗
 * - QSO 同步弹窗
 *
 * @note
 * 应在 LVGL 初始化完成后调用。
 */
void app_ui_create(void);

/* -------------------------------------------------------------------------- */
/* Status and main screen update                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 更新底部状态栏文字。
 *
 * @param text 状态文本；为空时显示默认占位内容。
 */
void app_ui_update_status(const char *text);

/**
 * @brief 更新当前说话人呼号。
 *
 * 兼容旧接口：当 talker 非空时认为处于活跃说话状态；
 * 当 talker 为空时认为未说话。
 *
 * @param talker 当前说话人呼号，可为 NULL。
 */
void app_ui_update_talker(const char *talker);

/**
 * @brief 更新当前说话人呼号和说话状态。
 *
 * @param talker 当前说话人呼号，可为 NULL。
 * @param active true 表示正在说话，false 表示未说话。
 */
void app_ui_update_talker_state(const char *talker, bool active);

/**
 * @brief 更新上次通联呼号。
 *
 * @param callsign 上次通联呼号，可为 NULL。
 */
void app_ui_update_last_call(const char *callsign);

/**
 * @brief 更新当前站点名称。
 *
 * @param station 当前站点名称，可为 NULL。
 */
void app_ui_update_station(const char *station);

/**
 * @brief 更新 QSO 数量显示。
 *
 * @param count QSO 数量。
 */
void app_ui_update_qso_count(uint32_t count);

/**
 * @brief 通知 UI 站点列表缓存已更新。
 *
 * 当 station_parser 或 WebSocket 模块更新站点缓存后，
 * 可调用该函数刷新当前正在显示的站点列表或收藏站点弹窗。
 */
void app_ui_station_list_updated(void);

/* -------------------------------------------------------------------------- */
/* WiFi and battery update                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 更新 WiFi RSSI 显示。
 *
 * UI 会根据 RSSI 值换算信号强度，并使用不同颜色显示 WiFi 图标。
 *
 * @param rssi_dbm WiFi 信号强度，单位 dBm。
 */
void app_ui_update_wifi_rssi(int rssi_dbm);

/**
 * @brief 更新 WiFi 连接状态。
 *
 * 兼容旧接口。连接时使用给定 RSSI 更新图标；
 * 断开时使用极低 RSSI 值显示弱信号/断开状态。
 *
 * @param connected true 表示已连接，false 表示未连接。
 * @param rssi RSSI，单位 dBm。
 */
void app_ui_update_wifi_status(bool connected, int rssi);

/**
 * @brief 更新电池状态显示。
 *
 * @param percent 电量百分比，范围 0~100。
 * @param charging true 表示正在充电，false 表示未充电。
 */
void app_ui_update_battery(uint8_t percent, bool charging);

/* -------------------------------------------------------------------------- */
/* Audio display and volume compatibility                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 更新音频电平显示。
 *
 * 当前版本已取消顶部音频电平条，该接口保留用于兼容旧调用。
 *
 * @param level 音频电平，范围 0~100。
 */
void app_ui_update_voice_level(uint8_t level);

/**
 * @brief 设置待刷新的音频电平。
 *
 * 当前版本已取消顶部音频电平条，该接口保留用于兼容旧调用。
 *
 * @param level 音频电平，范围 0~100。
 */
void app_ui_set_voice_level_pending(uint8_t level);

/**
 * @brief 更新音量显示。
 *
 * 当前主界面不单独显示音量，该接口保留用于兼容旧代码。
 *
 * @param volume 音量百分比，范围 0~100。
 */
void app_ui_update_volume(uint8_t volume);

/* -------------------------------------------------------------------------- */
/* Backlight                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 设置屏幕背光亮度。
 *
 * @param percent 背光百分比，范围 0~100。
 */
void app_ui_set_backlight_percent(uint8_t percent);

/**
 * @brief 设置屏幕背光亮度。
 *
 * 兼容旧接口，内部等价于 app_ui_set_backlight_percent()。
 *
 * @param percent 背光百分比，范围 0~100。
 */
void app_ui_set_backlight(uint8_t percent);

/* -------------------------------------------------------------------------- */
/* Idle clock and power-save clock                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief 从普通待机状态唤醒。
 *
 * 如果当前处于手动省电或低电量省电时钟页，该函数不会退出省电时钟页。
 */
void app_ui_wake_from_idle(void);

/**
 * @brief 进入省电时钟页。
 *
 * 该函数通常由 app_power_save 模块调用，用于手动省电或低电量省电。
 */
void app_ui_enter_power_save_clock(void);

/**
 * @brief 退出省电时钟页。
 *
 * 该函数通常由 app_power_save 模块调用。
 */
void app_ui_exit_power_save_clock(void);

/**
 * @brief 判断当前是否处于省电时钟页。
 *
 * @return
 *      - true：当前处于省电时钟页
 *      - false：当前不处于省电时钟页
 */
bool app_ui_is_power_save_clock(void);

/* -------------------------------------------------------------------------- */
/* QSO sync popup                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief 显示 QSO 同步提示弹窗。
 *
 * @param text 弹窗提示文本。
 * @param auto_close_ms 自动关闭时间，单位 ms。
 *                      为 0 时不自动关闭。
 */
void app_ui_qso_sync_popup_show(const char *text, uint32_t auto_close_ms);

/**
 * @brief 关闭 QSO 同步提示弹窗。
 */
void app_ui_qso_sync_popup_close(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_UI_H */
