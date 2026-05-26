/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file audio_output.h
 * @brief 音频输出统一抽象接口。
 *
 * 本模块用于屏蔽具体音频输出方式，为上层音频播放逻辑提供统一接口。
 *
 * 当前支持：
 * - 本机音频输出；
 * - 蓝牙 A2DP 输出；
 * - 自动模式：蓝牙已连接时走蓝牙，否则走本机输出。
 *
 * @note
 * 蓝牙音频功能受 app_config.h 中 APP_CONFIG_BT_AUDIO_ENABLE 控制。
 */

#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Public types                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 音频输出模式。
 */
typedef enum {
    /**
     * @brief 本机音频输出。
     */
    AUDIO_OUTPUT_LOCAL = 0,

    /**
     * @brief 蓝牙音频输出。
     *
     * 如果蓝牙未连接，写入音频时会返回 ESP_ERR_INVALID_STATE。
     */
    AUDIO_OUTPUT_BT = 1,

    /**
     * @brief 自动输出模式。
     *
     * 蓝牙已连接时优先走蓝牙输出；
     * 蓝牙未连接时回退到本机输出。
     */
    AUDIO_OUTPUT_AUTO = 2,
} audio_output_mode_t;

/* -------------------------------------------------------------------------- */
/* Initialization and mode control                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化音频输出模块。
 *
 * 初始化时会根据 APP_CONFIG_AUDIO_OUTPUT_DEFAULT 设置默认输出模式。
 * 如果启用了蓝牙音频，还会初始化并启动蓝牙音频模块。
 *
 * @return
 *      - ESP_OK：初始化成功
 *      - 其他值：底层初始化失败
 */
esp_err_t audio_output_init(void);

/**
 * @brief 设置音频输出模式。
 *
 * @param mode 输出模式。
 *
 * @return
 *      - ESP_OK：设置成功
 *      - ESP_ERR_INVALID_ARG：mode 非法
 */
esp_err_t audio_output_set_mode(audio_output_mode_t mode);

/**
 * @brief 获取当前音频输出模式。
 *
 * @return 当前输出模式。
 */
audio_output_mode_t audio_output_get_mode(void);

/* -------------------------------------------------------------------------- */
/* Bluetooth status                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief 判断蓝牙音频功能是否启用。
 *
 * @return
 *      - true：编译时启用了蓝牙音频功能
 *      - false：编译时未启用蓝牙音频功能
 */
bool audio_output_bt_is_enabled(void);

/**
 * @brief 判断蓝牙音频是否已连接。
 *
 * @return
 *      - true：蓝牙音频已连接
 *      - false：蓝牙音频未连接
 */
bool audio_output_bt_is_connected(void);

/**
 * @brief 蓝牙连接成功通知入口。
 *
 * 由 bt_audio.c 在蓝牙设备连接成功后调用。
 */
void audio_output_on_bt_connected(void);

/**
 * @brief 蓝牙断开通知入口。
 *
 * 由 bt_audio.c 在蓝牙设备断开后调用。
 */
void audio_output_on_bt_disconnected(void);

/* -------------------------------------------------------------------------- */
/* PCM output                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief 写入 PCM 音频数据。
 *
 * @param pcm_data PCM 数据指针，不能为空。
 * @param len PCM 数据长度，单位字节，必须大于 0。
 * @param sample_rate 采样率，例如 8000、16000、44100、48000。
 * @param channels 声道数，1 表示 mono，2 表示 stereo。
 * @param bits_per_sample 位深，通常为 16。
 *
 * @return
 *      - ESP_OK：写入成功
 *      - ESP_ERR_INVALID_ARG：参数非法
 *      - ESP_ERR_INVALID_STATE：当前输出模式不可用
 *      - 其他值：底层输出错误
 */
esp_err_t audio_output_write_pcm(const uint8_t *pcm_data,
                                 int len,
                                 int sample_rate,
                                 int channels,
                                 int bits_per_sample);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_OUTPUT_H */
