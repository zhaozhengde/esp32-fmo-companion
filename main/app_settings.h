/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file app_settings.h
 * @brief 应用配置管理接口。
 *
 * 本模块负责应用运行配置的默认值加载、NVS 持久化保存、
 * 配置版本校验以及运行期配置更新。
 *
 * 当前管理的配置包括：
 * - WiFi SSID / 密码；
 * - FMO Host；
 * - WebSocket URL；
 * - 音量；
 * - 背光；
 * - 待机时钟；
 * - 电池校准；
 * - 本机呼号；
 * - QSO 同步状态；
 * - 屏幕旋转状态。
 *
 * @note
 * 修改 app_settings_t 结构体字段后，需要提升 APP_SETTINGS_VERSION。
 * 旧 NVS blob 会被判定无效，然后自动写入默认配置。
 */

#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Version                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 配置结构体版本号。
 *
 * 修改 app_settings_t 的字段布局、字段大小或字段含义后，
 * 应提升该版本号，避免旧 NVS blob 被错误解释。
 */
#define APP_SETTINGS_VERSION  9

/* -------------------------------------------------------------------------- */
/* Public types                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 应用配置结构体。
 *
 * 该结构体整体以 blob 形式保存到 NVS。
 *
 * @note
 * 修改该结构体后，请同步提升 APP_SETTINGS_VERSION。
 */
typedef struct {
    /**
     * @brief 配置版本号。
     */
    uint32_t version;

    /**
     * @brief 屏幕背光百分比，范围 0~100。
     */
    uint8_t backlight_percent;

    /**
     * @brief 音频音量百分比，范围 0~100。
     */
    uint8_t audio_volume;

    /**
     * @brief 是否启用待机时钟页。
     *
     * 0：禁用
     * 1：启用
     */
    uint8_t idle_image_enabled;

    /**
     * @brief WiFi SSID。
     *
     * 802.11 SSID 最大长度为 32 字节。
     * 这里使用 33 字节用于保存结尾 '\0'。
     *
     * @note
     * 这里限制的是字节数，不是字符数。
     * 中文 UTF-8 SSID 中，一个汉字通常占 3 字节。
     */
    char wifi_ssid[33];

    /**
     * @brief WiFi 密码。
     *
     * WPA/WPA2 密码最长 63 字节，额外 1 字节保存 '\0'。
     */
    char wifi_password[64];

    /**
     * @brief FMO 服务 Host。
     *
     * 用户设置页只填写该字段，例如：
     *
     * @code
     * 192.168.3.165
     * example.com:8080
     * @endcode
     *
     * 保存时会自动生成：
     *
     * @code
     * ws_audio_url   = ws://<fmo_host>/audio
     * ws_event_url   = ws://<fmo_host>/events
     * ws_station_url = ws://<fmo_host>/ws
     * @endcode
     */
    char fmo_host[64];

    /**
     * @brief 音频 WebSocket URL。
     */
    char ws_audio_url[128];

    /**
     * @brief 事件 WebSocket URL。
     */
    char ws_event_url[128];

    /**
     * @brief 站点 WebSocket URL。
     */
    char ws_station_url[128];

    /**
     * @brief 当前站点刷新间隔，单位 ms。
     */
    uint32_t ws_station_current_refresh_ms;

    /**
     * @brief 站点列表刷新间隔，单位 ms。
     */
    uint32_t ws_station_list_refresh_ms;

    /**
     * @brief 待机时钟超时时间，单位 ms。
     */
    uint32_t idle_image_timeout_ms;

    /**
     * @brief 电池空电电压，单位 mV。
     */
    uint16_t battery_empty_mv;

    /**
     * @brief 电池满电电压，单位 mV。
     */
    uint16_t battery_full_mv;

    /**
     * @brief 电池采样偏移校准值，单位 mV。
     */
    int16_t battery_offset_mv;

    /**
     * @brief 本机呼号。
     */
    char owner_callsign[16];

    /**
     * @brief 已同步的 QSO 数量。
     */
    uint32_t qso_count;

    /**
     * @brief 最近一次已同步的 log id。
     */
    int32_t qso_latest_log_id;

    /**
     * @brief QSO 数量是否有效。
     */
    bool qso_count_valid;

    /**
     * @brief 屏幕是否旋转 180 度。
     */
    bool screen_rotate_180;

} app_settings_t;

/* -------------------------------------------------------------------------- */
/* Initialization and access                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化配置模块。
 *
 * 初始化流程：
 * 1. 先加载默认配置到内存；
 * 2. 尝试从 NVS 读取配置 blob；
 * 3. 校验版本和字段合法性；
 * 4. 如果 NVS 配置有效，则使用 NVS 配置；
 * 5. 如果 NVS 中没有配置或配置无效，则保存默认配置。
 *
 * @return
 *      - ESP_OK：初始化成功
 *      - 其他值：NVS 打开或保存失败
 */
esp_err_t app_settings_init(void);

/**
 * @brief 从当前内存配置复制一份到 cfg。
 *
 * @param cfg 输出配置指针，不能为空。
 *
 * @return
 *      - ESP_OK：复制成功
 *      - ESP_ERR_INVALID_ARG：参数为空
 */
esp_err_t app_settings_load(app_settings_t *cfg);

/**
 * @brief 保存配置到 NVS，并更新内存中的当前配置。
 *
 * 保存前会对配置进行 sanitize，并重新生成 WebSocket URL。
 *
 * @param cfg 待保存配置指针，不能为空。
 *
 * @return
 *      - ESP_OK：保存成功
 *      - ESP_ERR_INVALID_ARG：参数或字段非法
 *      - 其他值：NVS 操作失败
 */
esp_err_t app_settings_save(const app_settings_t *cfg);

/**
 * @brief 获取当前配置指针。
 *
 * @return 当前配置的只读指针。
 *
 * @note
 * 返回的是内部静态对象指针，调用方不应直接修改其内容。
 * 如需修改配置，请复制一份后通过 app_settings_save()
 * 或对应 setter 接口保存。
 */
const app_settings_t *app_settings_get(void);

/**
 * @brief 加载默认配置到 cfg。
 *
 * @param cfg 配置输出指针，不能为空。
 */
void app_settings_load_defaults(app_settings_t *cfg);

/**
 * @brief 根据 cfg->fmo_host 自动生成三个 WebSocket URL。
 *
 * @param cfg 配置指针，不能为空。
 */
void app_settings_build_ws_urls(app_settings_t *cfg);

/* -------------------------------------------------------------------------- */
/* Setters                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 设置 WiFi SSID 和密码。
 *
 * 该函数只修改内存配置并保存到 NVS，不会立即重连 WiFi。
 * 当前 UI 逻辑建议保存后重启生效。
 *
 * @param ssid WiFi SSID，不能为空，最大 32 字节。
 * @param password WiFi 密码，可为空字符串，最大 63 字节。
 *
 * @return
 *      - ESP_OK：保存成功
 *      - ESP_ERR_INVALID_ARG：参数非法
 *      - ESP_ERR_INVALID_SIZE：目标缓存不足
 *      - 其他值：NVS 保存失败
 */
esp_err_t app_settings_set_wifi(const char *ssid, const char *password);

/**
 * @brief 设置 FMO Host。
 *
 * 例如：
 *
 * @code
 * 192.168.3.165
 * example.com:8080
 * @endcode
 *
 * 内部会自动生成：
 *
 * @code
 * ws://<host>/audio
 * ws://<host>/events
 * ws://<host>/ws
 * @endcode
 *
 * @param host FMO Host，不能为空。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_fmo_host(const char *host);

/**
 * @brief 设置音量。
 *
 * @param volume 音量百分比，范围 0~100，超过 100 会被限制为 100。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_audio_volume(uint8_t volume);

/**
 * @brief 设置背光。
 *
 * @param percent 背光百分比，范围 0~100，超过 100 会被限制为 100。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_backlight(uint8_t percent);

/**
 * @brief 设置电池校准参数。
 *
 * @param empty_mv 空电电压，单位 mV。
 * @param full_mv 满电电压，单位 mV。
 * @param offset_mv 电压偏移校准值，单位 mV。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_battery_calibration(uint16_t empty_mv,
                                               uint16_t full_mv,
                                               int16_t offset_mv);

/**
 * @brief 设置本机呼号。
 *
 * 小写字母会自动转换为大写。
 *
 * @param callsign 本机呼号，不能为空。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_owner_callsign(const char *callsign);

/**
 * @brief 设置 QSO 同步状态。
 *
 * @param count 当前 QSO 数量。
 * @param latest_log_id 最近一次同步的 log id。
 * @param valid true 表示 QSO 数量有效。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_qso_state(uint32_t count,
                                     int32_t latest_log_id,
                                     bool valid);

/**
 * @brief 设置屏幕是否旋转 180 度。
 *
 * @param enabled true 表示旋转 180 度。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_screen_rotate_180(bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* APP_SETTINGS_H */
