/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file audio_output.c
 * @brief 音频输出统一抽象层实现。
 *
 * 本文件负责根据当前输出模式，将 PCM 音频数据写入：
 * - 本机音频输出；
 * - 蓝牙 A2DP 输出；
 * - 自动选择的输出目标。
 *
 * 蓝牙输出是否可用由 APP_CONFIG_BT_AUDIO_ENABLE 控制。
 */

#include "audio_output.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_log.h"

/* Project headers ---------------------------------------------------------- */
#include "app_config.h"

#if APP_CONFIG_BT_AUDIO_ENABLE
#include "bt_audio.h"
#endif

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "audio_output";

/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief 当前音频输出模式。
 */
static audio_output_mode_t s_output_mode = AUDIO_OUTPUT_AUTO;

/**
 * @brief 当前蓝牙音频是否已连接。
 */
static bool s_bt_connected = false;

/* -------------------------------------------------------------------------- */
/* External local audio backend                                               */
/* -------------------------------------------------------------------------- */

/*
 * 本机音频输出后端。
 *
 * 当前声明为外部函数，由项目中的本机音频播放模块实现。
 * 通常可能位于 audio_ws.c 或专门的本地音频输出文件中。
 *
 * 如果后续希望彻底解耦，建议将本机输出也整理为独立模块：
 *
 * local_audio_output.c
 * local_audio_output.h
 */
extern esp_err_t local_audio_write_pcm(const uint8_t *pcm_data,
                                       int len,
                                       int sample_rate,
                                       int channels,
                                       int bits_per_sample);

/**
 * @brief 设置本机音频静音状态。
 *
 * @param mute true 表示静音，false 表示取消静音。
 */
extern void local_audio_mute(bool mute);

/* -------------------------------------------------------------------------- */
/* Private helpers                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief 根据编译期配置获取默认输出模式。
 *
 * @return 默认音频输出模式。
 */
static audio_output_mode_t audio_output_get_default_mode(void)
{
#if APP_CONFIG_AUDIO_OUTPUT_DEFAULT == APP_CONFIG_AUDIO_OUTPUT_LOCAL
    return AUDIO_OUTPUT_LOCAL;
#elif APP_CONFIG_AUDIO_OUTPUT_DEFAULT == APP_CONFIG_AUDIO_OUTPUT_BT
    return AUDIO_OUTPUT_BT;
#else
    return AUDIO_OUTPUT_AUTO;
#endif
}

/**
 * @brief 判断输出模式是否合法。
 *
 * @param mode 输出模式。
 *
 * @return true 表示合法。
 */
static bool audio_output_mode_is_valid(audio_output_mode_t mode)
{
    return mode == AUDIO_OUTPUT_LOCAL ||
           mode == AUDIO_OUTPUT_BT ||
           mode == AUDIO_OUTPUT_AUTO;
}

/**
 * @brief 写入本机音频输出。
 */
static esp_err_t audio_output_write_local(const uint8_t *pcm_data,
                                          int len,
                                          int sample_rate,
                                          int channels,
                                          int bits_per_sample)
{
    return local_audio_write_pcm(pcm_data,
                                 len,
                                 sample_rate,
                                 channels,
                                 bits_per_sample);
}

/**
 * @brief 写入蓝牙音频输出。
 */
static esp_err_t audio_output_write_bt(const uint8_t *pcm_data,
                                       int len,
                                       int sample_rate,
                                       int channels,
                                       int bits_per_sample)
{
#if APP_CONFIG_BT_AUDIO_ENABLE
    if (!s_bt_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    return bt_audio_write_pcm(pcm_data,
                              len,
                              sample_rate,
                              channels,
                              bits_per_sample);
#else
    (void)pcm_data;
    (void)len;
    (void)sample_rate;
    (void)channels;
    (void)bits_per_sample;

    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* -------------------------------------------------------------------------- */
/* Public interfaces                                                          */
/* -------------------------------------------------------------------------- */

esp_err_t audio_output_init(void)
{
    s_output_mode = audio_output_get_default_mode();
    s_bt_connected = false;

#if APP_CONFIG_BT_AUDIO_ENABLE
    ESP_LOGI(TAG, "BT audio enabled");

    esp_err_t ret = bt_audio_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "bt_audio_init failed: %s", esp_err_to_name(ret));
        /*
         * 蓝牙初始化失败时不阻止本机播放。
         */
    } else {
        /*
         * 初版：开机后自动扫描并连接配置的蓝牙音箱。
         * 后续可改为设置页手动开启。
         */
        ret = bt_audio_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "bt_audio_start failed: %s", esp_err_to_name(ret));
        }
    }
#else
    ESP_LOGI(TAG, "BT audio disabled");
#endif

#if !APP_CONFIG_BT_AUDIO_ENABLE
    /*
     * 如果编译时未启用蓝牙，但默认配置为蓝牙或自动，
     * 则强制回退本机模式，避免后续误判。
     */
    if (s_output_mode == AUDIO_OUTPUT_BT ||
        s_output_mode == AUDIO_OUTPUT_AUTO) {
        s_output_mode = AUDIO_OUTPUT_LOCAL;
    }
#endif

    ESP_LOGI(TAG, "audio output init, mode=%d", (int)s_output_mode);

    return ESP_OK;
}

esp_err_t audio_output_set_mode(audio_output_mode_t mode)
{
    if (!audio_output_mode_is_valid(mode)) {
        return ESP_ERR_INVALID_ARG;
    }

#if !APP_CONFIG_BT_AUDIO_ENABLE
    if (mode == AUDIO_OUTPUT_BT || mode == AUDIO_OUTPUT_AUTO) {
        ESP_LOGW(TAG, "BT audio disabled, force local mode");
        s_output_mode = AUDIO_OUTPUT_LOCAL;
        return ESP_OK;
    }
#endif

    s_output_mode = mode;

    ESP_LOGI(TAG, "audio output mode set to %d", (int)s_output_mode);

    return ESP_OK;
}

audio_output_mode_t audio_output_get_mode(void)
{
    return s_output_mode;
}

bool audio_output_bt_is_enabled(void)
{
#if APP_CONFIG_BT_AUDIO_ENABLE
    return true;
#else
    return false;
#endif
}

bool audio_output_bt_is_connected(void)
{
    return s_bt_connected;
}

void audio_output_on_bt_connected(void)
{
    s_bt_connected = true;

    ESP_LOGI(TAG, "BT audio connected");

    /*
     * 蓝牙可用时，为避免本机和蓝牙同时播放，
     * 在 BT/AUTO 模式下静音本机输出。
     */
    if (s_output_mode == AUDIO_OUTPUT_BT ||
        s_output_mode == AUDIO_OUTPUT_AUTO) {
        local_audio_mute(true);
    }
}

void audio_output_on_bt_disconnected(void)
{
    s_bt_connected = false;

    ESP_LOGW(TAG, "BT audio disconnected");

    /*
     * 蓝牙断开后恢复本机播放。
     *
     * AUTO 模式下会自动回退本机输出；
     * BT 模式下虽然 write_pcm 会返回 ESP_ERR_INVALID_STATE，
     * 但取消本机静音可以避免后续切换模式时仍保持静音。
     */
    if (s_output_mode == AUDIO_OUTPUT_BT ||
        s_output_mode == AUDIO_OUTPUT_AUTO) {
        local_audio_mute(false);
    }
}

esp_err_t audio_output_write_pcm(const uint8_t *pcm_data,
                                 int len,
                                 int sample_rate,
                                 int channels,
                                 int bits_per_sample)
{
    if (!pcm_data || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sample_rate <= 0 || channels <= 0 || bits_per_sample <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (s_output_mode) {
    case AUDIO_OUTPUT_LOCAL:
        return audio_output_write_local(pcm_data,
                                        len,
                                        sample_rate,
                                        channels,
                                        bits_per_sample);

    case AUDIO_OUTPUT_BT:
        /*
         * 纯蓝牙模式：
         * - 蓝牙已连接：写蓝牙
         * - 蓝牙未连接：返回 ESP_ERR_INVALID_STATE
         */
        return audio_output_write_bt(pcm_data,
                                     len,
                                     sample_rate,
                                     channels,
                                     bits_per_sample);

    case AUDIO_OUTPUT_AUTO:
        /*
         * 自动模式：
         * - 蓝牙功能启用且已连接：优先写蓝牙
         * - 否则回退本机播放
         */
#if APP_CONFIG_BT_AUDIO_ENABLE
        if (s_bt_connected) {
            return audio_output_write_bt(pcm_data,
                                         len,
                                         sample_rate,
                                         channels,
                                         bits_per_sample);
        }
#endif
        return audio_output_write_local(pcm_data,
                                        len,
                                        sample_rate,
                                        channels,
                                        bits_per_sample);

    default:
        return ESP_ERR_INVALID_STATE;
    }
}
