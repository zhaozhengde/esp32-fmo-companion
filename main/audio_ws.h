/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file audio_ws.h
 * @brief 音频、事件、站点 WebSocket 与 QSO 同步接口。
 *
 * 本模块负责：
 * - 启动和停止 Audio / Event / Station WebSocket；
 * - 管理本地网络音频播放；
 * - 单独开启或关闭 Audio WebSocket；
 * - 设置音量；
 * - 请求和切换站点；
 * - 接收 Event 模块提供的 speaking 状态；
 * - 在低电量场景下禁止本地音频播放；
 * - 通过 Station WebSocket 请求并同步 QSO 数量。
 *
 * 启动流程通常为：
 * 1. WiFi 获取 IP；
 * 2. wifi_manager 调用 audio_ws_start()；
 * 3. 启动 SNTP；
 * 4. 启动 Event WebSocket；
 * 5. 启动 Station WebSocket；
 * 6. 创建音频播放任务；
 * 7. Audio WebSocket 默认保持关闭，等待用户手动开启。
 */

#ifndef AUDIO_WS_H
#define AUDIO_WS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Module lifecycle                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief 启动 WebSocket 与音频播放相关模块。
 *
 * 当前启动内容包括：
 * - 初始化音频 RingBuffer；
 * - 初始化本机 DAC / I2S 输出；
 * - 创建音频播放任务；
 * - 启动 SNTP；
 * - 启动 Event WebSocket；
 * - 启动 Station WebSocket；
 * - 启动站点和 QSO 相关后台任务。
 *
 * @note
 * 当前 Audio WebSocket 开机默认不自动连接，
 * 需要用户通过静音按钮调用 audio_ws_audio_enable() 开启。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t audio_ws_start(void);

/**
 * @brief 停止 WebSocket 与本地音频播放。
 *
 * 会关闭：
 * - Audio WebSocket；
 * - Event WebSocket；
 * - Station WebSocket；
 * - 本地功放输出；
 * - 当前音频缓冲内容。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t audio_ws_stop(void);

/**
 * @brief 判断 audio_ws 模块是否已经启动。
 *
 * @return true 表示已启动。
 */
bool audio_ws_is_started(void);

/* -------------------------------------------------------------------------- */
/* Audio WebSocket control                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 单独开启 Audio WebSocket。
 *
 * 不影响 Event WebSocket 和 Station WebSocket。
 *
 * @return
 *      - ESP_OK：启动成功或已经连接
 *      - ESP_ERR_INVALID_STATE：模块未启动或 WiFi 未连接
 *      - ESP_ERR_NOT_SUPPORTED：编译时未启用 Audio WebSocket
 *      - 其他值：WebSocket 启动错误
 */
esp_err_t audio_ws_audio_enable(void);

/**
 * @brief 单独关闭 Audio WebSocket。
 *
 * 不影响 Event WebSocket 和 Station WebSocket。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t audio_ws_audio_disable(void);

/**
 * @brief 判断 Audio WebSocket 是否被用户开启。
 *
 * @return true 表示用户已开启音频功能。
 */
bool audio_ws_audio_is_enabled(void);

/**
 * @brief 判断 Audio WebSocket 当前是否已连接。
 *
 * @return true 表示当前已连接。
 */
bool audio_ws_audio_is_connected(void);

/* -------------------------------------------------------------------------- */
/* Audio playback control                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 设置当前音量。
 *
 * @param vol 音量百分比，范围 0~100，超过 100 会被限制为 100。
 *
 * @return ESP_OK。
 */
esp_err_t audio_set_volume(uint8_t vol);

/**
 * @brief 获取当前音量。
 *
 * @return 当前音量百分比，范围 0~100。
 */
uint8_t audio_get_volume(void);

/**
 * @brief 设置当前通联说话状态。
 *
 * 该接口通常由 event_parser 在收到 callsign / isSpeaking 事件后调用。
 *
 * @param speaking true 表示当前处于说话状态。
 *
 * @note
 * speaking=true 只表示允许播放，不应直接打开功放。
 * 功放应由播放任务在缓冲达到启动阈值后打开。
 */
void audio_ws_set_speaking(bool speaking);

/**
 * @brief 设置低电量音频禁用状态。
 *
 * @param disabled true 表示禁止本地播放。
 *
 * 当 disabled=true 时：
 * - 禁止本地播放；
 * - 清空音频缓冲；
 * - 关闭功放。
 *
 * @note
 * 该接口不主动关闭 WebSocket，只控制本地播放权限。
 */
void audio_ws_set_low_power_disabled(bool disabled);

/* -------------------------------------------------------------------------- */
/* Station WebSocket control                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 判断 Station WebSocket 是否已连接。
 *
 * @return true 表示已连接。
 */
bool audio_ws_station_is_connected(void);

/**
 * @brief 请求当前站点。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t audio_ws_station_get_current(void);

/**
 * @brief 请求指定范围的普通站点列表。
 *
 * @param start 起始位置。
 * @param count 请求数量。
 *
 * @return ESP_OK 或错误码。
 *
 * @note
 * 该接口为原始范围请求接口，保留用于兼容现有代码。
 */
esp_err_t audio_ws_station_get_list_range(int start, int count);

/**
 * @brief 请求普通站点列表。
 *
 * @param start 起始位置，小于 0 时按 0 处理。
 * @param count 请求数量，小于等于 0 时按 1 处理。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t audio_ws_station_get_list(int start, int count);

/**
 * @brief 请求收藏/置顶站点列表。
 *
 * @param start 起始位置，小于 0 时按 0 处理。
 * @param count 请求数量，小于等于 0 时按 1 处理。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t audio_ws_station_get_pinned_list(int start, int count);

/**
 * @brief 设置当前站点。
 *
 * @param uid 目标站点 UID。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t audio_ws_station_set_current(int uid);

/* -------------------------------------------------------------------------- */
/* QSO synchronization                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief 请求一页 QSO 列表。
 *
 * 该请求通过 Station WebSocket 发送。
 *
 * @param page 页码，从 0 开始。
 * @param page_size 每页数量。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t audio_ws_qso_get_list(int page, int page_size);

/**
 * @brief 启动一次手动 QSO 完整扫描。
 *
 * 设置页点击“QSO同步”时调用。
 * 内部会从第 0 页开始扫描，直到最后一页或达到扫描上限。
 */
void audio_ws_qso_count_manual_full_scan(void);

/**
 * @brief 处理 QSO 列表响应。
 *
 * 由 event_parser 收到 qso/getListResponse 后调用。
 *
 * @param page 响应页码。
 * @param page_size 响应每页数量。
 * @param count 当前页记录数量。
 * @param latest_log_id 最新记录 ID。
 * @param log_ids 当前页记录 ID 数组，可为 NULL。
 * @param log_id_count log_ids 数组长度。
 */
void audio_ws_qso_count_handle_response(int page,
                                        int page_size,
                                        int count,
                                        int latest_log_id,
                                        const int *log_ids,
                                        int log_id_count);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_WS_H */
