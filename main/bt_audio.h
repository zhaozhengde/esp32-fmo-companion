/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file bt_audio.h
 * @brief 蓝牙 A2DP Source 音频输出接口。
 *
 * 本模块负责 Classic Bluetooth / A2DP Source 初始化、
 * 目标蓝牙音频设备扫描连接，以及 PCM 音频写入。
 *
 * @note
 * 蓝牙音频功能受 app_config.h 中 APP_CONFIG_BT_AUDIO_ENABLE 控制。
 * 当该宏为 0 时，本模块接口仍然存在，但会返回不支持或未连接状态。
 */

#ifndef BT_AUDIO_H
#define BT_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化蓝牙音频模块。
 *
 * 初始化内容包括：
 * - 释放 BLE 内存；
 * - 初始化并启用 Classic BT controller；
 * - 初始化并启用 Bluedroid；
 * - 注册 GAP 回调；
 * - 注册 A2DP Source 回调；
 * - 分配蓝牙 PCM ring buffer。
 *
 * @return
 *      - ESP_OK：初始化成功，或已经初始化
 *      - ESP_ERR_NOT_SUPPORTED：编译时未启用蓝牙音频
 *      - ESP_ERR_NO_MEM：内存不足
 *      - 其他值：ESP-IDF 蓝牙初始化错误码
 */
esp_err_t bt_audio_init(void);

/**
 * @brief 启动蓝牙扫描并尝试连接目标设备。
 *
 * 目标设备名称由 APP_CONFIG_BT_TARGET_NAME 指定。
 *
 * @return
 *      - ESP_OK：启动成功，或已经连接/正在连接
 *      - ESP_ERR_NOT_SUPPORTED：编译时未启用蓝牙音频
 *      - 其他值：ESP-IDF 蓝牙扫描错误码
 */
esp_err_t bt_audio_start(void);

/**
 * @brief 停止蓝牙音频。
 *
 * 如果正在扫描则取消扫描；
 * 如果已经连接则断开 A2DP 连接；
 * 同时清空内部 PCM ring buffer。
 *
 * @return ESP_OK 或底层错误码。
 */
esp_err_t bt_audio_stop(void);

/**
 * @brief 判断蓝牙音频是否已连接。
 *
 * @return true 表示已连接。
 */
bool bt_audio_is_connected(void);

/**
 * @brief 写入 PCM 音频数据到蓝牙输出。
 *
 * 输入 PCM 会被转换为 A2DP Source 常用输出格式：
 * - 44.1kHz；
 * - stereo；
 * - 16-bit。
 *
 * 当前转换策略：
 * - 仅支持 16-bit PCM；
 * - mono 输入复制到 L/R；
 * - stereo 输入取左声道再复制到 L/R；
 * - 使用零阶保持方式进行简单升采样。
 *
 * @param pcm_data PCM 数据指针。
 * @param len PCM 字节长度。
 * @param sample_rate 输入采样率。
 * @param channels 输入声道数，1=mono，2=stereo。
 * @param bits_per_sample 输入位深，目前仅支持 16。
 *
 * @return
 *      - ESP_OK：写入成功
 *      - ESP_ERR_INVALID_ARG：参数非法
 *      - ESP_ERR_INVALID_STATE：蓝牙未连接
 *      - ESP_ERR_NOT_SUPPORTED：格式不支持或蓝牙功能未启用
 */
esp_err_t bt_audio_write_pcm(const uint8_t *pcm_data,
                             int len,
                             int sample_rate,
                             int channels,
                             int bits_per_sample);

#ifdef __cplusplus
}
#endif

#endif /* BT_AUDIO_H */
