/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file app_settings.c
 * @brief 应用配置管理实现。
 *
 * 本模块负责：
 * - 默认配置生成；
 * - NVS 配置读取与保存；
 * - 配置版本校验；
 * - 字段合法性检查；
 * - WiFi、FMO、音量、背光、电池校准、本机呼号等设置保存；
 * - WebSocket URL 自动生成。
 */

#include "app_settings.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Project headers ---------------------------------------------------------- */
#include "app_config.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "app_settings";

/* -------------------------------------------------------------------------- */
/* Private macros                                                             */
/* -------------------------------------------------------------------------- */

#define NVS_NAMESPACE  "appcfg"
#define NVS_KEY_BLOB   "settings"

#ifndef APP_DEFAULT_OWNER_CALLSIGN
#define APP_DEFAULT_OWNER_CALLSIGN "BI4TKL"
#endif

/*
 * 下面三个 key 当前未在本文件中单独使用。
 * 如果后续 QSO 状态改为单独 NVS key 保存，可以复用。
 */
#define KEY_QSO_COUNT       "qso_count"
#define KEY_QSO_LATEST      "qso_latest"
#define KEY_QSO_VALID       "qso_valid"

/*
 * WiFi SSID 在 802.11 中最大是 32 字节，不是 32 个字符。
 * 中文 UTF-8 一般一个汉字占 3 字节。
 *
 * 为了作为 C 字符串保存，app_settings_t.wifi_ssid 定义为 33 字节：
 * 32 字节 SSID + 1 字节 '\0'。
 */
#define WIFI_SSID_MAX_BYTES      32

/*
 * WPA/WPA2 密码最大 63 字节。
 */
#define WIFI_PASSWORD_MAX_BYTES  63

/*
 * 本机呼号最大长度。
 * app_settings_t.owner_callsign 定义为 16 字节：
 * 15 字节内容 + 1 字节 '\0'。
 */
#define OWNER_CALLSIGN_MAX_LEN   15

/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

static app_settings_t s_settings;

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static void safe_strcpy(char *dst, size_t dst_size, const char *src);
static bool is_non_empty_string(const char *s);
static bool is_c_string_terminated(const char *s, size_t max_size);

static bool owner_callsign_is_valid(const char *callsign);
static esp_err_t copy_owner_callsign(char *dst,
                                     size_t dst_size,
                                     const char *callsign);

static bool wifi_ssid_is_valid(const char *ssid);
static bool wifi_password_is_valid(const char *password);

static esp_err_t copy_wifi_ssid(char *dst,
                                size_t dst_size,
                                const char *ssid);
static esp_err_t copy_wifi_password(char *dst,
                                    size_t dst_size,
                                    const char *password);

static bool app_settings_is_valid(const app_settings_t *cfg);
static void app_settings_sanitize(app_settings_t *cfg);

/* -------------------------------------------------------------------------- */
/* String helpers                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief 安全复制字符串。
 *
 * 与 strncpy 不同，该函数始终保证目标缓冲区以 '\0' 结尾。
 *
 * @param dst 目标缓冲区。
 * @param dst_size 目标缓冲区大小。
 * @param src 源字符串，可为 NULL。
 */
static void safe_strcpy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/**
 * @brief 判断字符串是否非空。
 */
static bool is_non_empty_string(const char *s)
{
    return s && s[0] != '\0';
}

/**
 * @brief 检查字符串在限定范围内是否存在 '\0'。
 *
 * 用于避免从 NVS blob 读取出的字符串没有结束符，
 * 从而导致 strlen 等函数越界读取。
 */
static bool is_c_string_terminated(const char *s, size_t max_size)
{
    if (!s || max_size == 0) {
        return false;
    }

    return memchr(s, '\0', max_size) != NULL;
}

/* -------------------------------------------------------------------------- */
/* Owner callsign validation and copy                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief 本机呼号合法性检查。
 *
 * 允许字符：
 * - A-Z
 * - a-z
 * - 0-9
 * - '-'
 * - '_'
 * - '/'
 * - '.'
 *
 * 保存时会自动转成大写。
 */
static bool owner_callsign_is_valid(const char *callsign)
{
    if (!callsign || callsign[0] == '\0') {
        return false;
    }

    size_t len = strlen(callsign);

    if (len == 0 || len > OWNER_CALLSIGN_MAX_LEN) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)callsign[i];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' ||
            c == '_' ||
            c == '/' ||
            c == '.') {
            continue;
        }

        return false;
    }

    return true;
}

/**
 * @brief 复制并规范化本机呼号。
 *
 * 小写字母会自动转为大写。
 */
static esp_err_t copy_owner_callsign(char *dst,
                                     size_t dst_size,
                                     const char *callsign)
{
    if (!dst || dst_size == 0 || !callsign) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!owner_callsign_is_valid(callsign)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(callsign);

    if (len >= dst_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(dst, 0, dst_size);

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)callsign[i];

        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)toupper(c);
        }

        dst[i] = (char)c;
    }

    dst[len] = '\0';

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* WiFi validation and copy                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi SSID 合法性检查。
 *
 * 注意：
 * 这里检查的是字节长度，不检查 UTF-8 字符完整性。
 * 对 WiFi 驱动来说，SSID 本质是字节序列。
 *
 * 当前规则：
 * - 不能为空；
 * - 长度必须为 1~32 字节。
 */
static bool wifi_ssid_is_valid(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return false;
    }

    size_t len = strlen(ssid);

    if (len == 0 || len > WIFI_SSID_MAX_BYTES) {
        return false;
    }

    return true;
}

/**
 * @brief WiFi 密码合法性检查。
 *
 * 这里不强制 8~63 字节，是为了兼容开放网络空密码。
 */
static bool wifi_password_is_valid(const char *password)
{
    if (!password) {
        return true;
    }

    size_t len = strlen(password);

    if (len > WIFI_PASSWORD_MAX_BYTES) {
        return false;
    }

    return true;
}

/**
 * @brief 复制 WiFi SSID。
 *
 * 不允许截断。
 *
 * 原因：
 * - 中文 SSID 如果被截断，可能破坏 UTF-8 字节序列；
 * - SSID 字节序列被截断后也无法匹配路由器广播的真实 SSID。
 */
static esp_err_t copy_wifi_ssid(char *dst,
                                size_t dst_size,
                                const char *ssid)
{
    if (!dst || dst_size == 0 || !ssid) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(ssid);

    if (len == 0 || len > WIFI_SSID_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * dst 至少需要容纳 32 字节 SSID + '\0'。
     */
    if (dst_size < WIFI_SSID_MAX_BYTES + 1) {
        ESP_LOGE(TAG,
                 "wifi_ssid buffer too small: %u, need at least 33",
                 (unsigned)dst_size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (len >= dst_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(dst, 0, dst_size);
    memcpy(dst, ssid, len);
    dst[len] = '\0';

    return ESP_OK;
}

/**
 * @brief 复制 WiFi 密码。
 *
 * 密码数组通常是 64 字节，最大密码 63 字节 + '\0'。
 */
static esp_err_t copy_wifi_password(char *dst,
                                    size_t dst_size,
                                    const char *password)
{
    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!password) {
        dst[0] = '\0';
        return ESP_OK;
    }

    size_t len = strlen(password);

    if (len > WIFI_PASSWORD_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len >= dst_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(dst, 0, dst_size);
    memcpy(dst, password, len);
    dst[len] = '\0';

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* WebSocket URL builder                                                      */
/* -------------------------------------------------------------------------- */

void app_settings_build_ws_urls(app_settings_t *cfg)
{
    if (!cfg) {
        return;
    }

    /*
     * 如果 fmo_host 为空，回退到默认 host。
     */
    if (!is_non_empty_string(cfg->fmo_host)) {
        safe_strcpy(cfg->fmo_host, sizeof(cfg->fmo_host), DEFAULT_FMO_HOST);
    }

    snprintf(cfg->ws_audio_url,
             sizeof(cfg->ws_audio_url),
             "ws://%s/audio",
             cfg->fmo_host);

    snprintf(cfg->ws_event_url,
             sizeof(cfg->ws_event_url),
             "ws://%s/events",
             cfg->fmo_host);

    snprintf(cfg->ws_station_url,
             sizeof(cfg->ws_station_url),
             "ws://%s/ws",
             cfg->fmo_host);

    cfg->ws_audio_url[sizeof(cfg->ws_audio_url) - 1] = '\0';
    cfg->ws_event_url[sizeof(cfg->ws_event_url) - 1] = '\0';
    cfg->ws_station_url[sizeof(cfg->ws_station_url) - 1] = '\0';
}

/* -------------------------------------------------------------------------- */
/* Defaults                                                                   */
/* -------------------------------------------------------------------------- */

void app_settings_load_defaults(app_settings_t *cfg)
{
    if (!cfg) {
        return;
    }

    memset(cfg, 0, sizeof(app_settings_t));

    cfg->version = APP_SETTINGS_VERSION;

    cfg->backlight_percent = DEFAULT_BACKLIGHT_PERCENT;
    cfg->audio_volume = DEFAULT_AUDIO_VOLUME;
    cfg->idle_image_enabled = DEFAULT_IDLE_IMAGE_ENABLED;

    /*
     * 默认本机呼号。
     */
    if (copy_owner_callsign(cfg->owner_callsign,
                            sizeof(cfg->owner_callsign),
                            APP_DEFAULT_OWNER_CALLSIGN) != ESP_OK) {
        ESP_LOGW(TAG, "APP_DEFAULT_OWNER_CALLSIGN invalid, use BI4TKL");
        safe_strcpy(cfg->owner_callsign,
                    sizeof(cfg->owner_callsign),
                    "BI4TKL");
    }

    /*
     * 默认 WiFi SSID 不允许截断。
     * 如果 DEFAULT_WIFI_SSID 超过 32 字节，会保存失败并置空。
     */
    if (copy_wifi_ssid(cfg->wifi_ssid,
                       sizeof(cfg->wifi_ssid),
                       DEFAULT_WIFI_SSID) != ESP_OK) {
        ESP_LOGW(TAG, "DEFAULT_WIFI_SSID invalid or too long, clear wifi_ssid");
        cfg->wifi_ssid[0] = '\0';
    }

    if (copy_wifi_password(cfg->wifi_password,
                           sizeof(cfg->wifi_password),
                           DEFAULT_WIFI_PASSWORD) != ESP_OK) {
        ESP_LOGW(TAG,
                 "DEFAULT_WIFI_PASSWORD invalid or too long, clear wifi_password");
        cfg->wifi_password[0] = '\0';
    }

    /*
     * 用户只需要配置 FMO host。
     * 三个 WebSocket URL 由 fmo_host 自动生成。
     */
    safe_strcpy(cfg->fmo_host, sizeof(cfg->fmo_host), DEFAULT_FMO_HOST);
    app_settings_build_ws_urls(cfg);

    cfg->ws_station_current_refresh_ms =
        DEFAULT_WS_STATION_CURRENT_REFRESH_MS;
    cfg->ws_station_list_refresh_ms =
        DEFAULT_WS_STATION_LIST_REFRESH_MS;
    cfg->idle_image_timeout_ms =
        DEFAULT_IDLE_IMAGE_TIMEOUT_MS;

    cfg->battery_empty_mv = BATTERY_PERCENT_EMPTY_MV;
    cfg->battery_full_mv = BATTERY_PERCENT_FULL_MV;
    cfg->battery_offset_mv = BATTERY_VOLTAGE_OFFSET_MV;

    cfg->qso_count = 0;
    cfg->qso_latest_log_id = -1;
    cfg->qso_count_valid = false;

    cfg->screen_rotate_180 = false;
}

/* -------------------------------------------------------------------------- */
/* Validation and sanitize                                                    */
/* -------------------------------------------------------------------------- */

static bool app_settings_is_valid(const app_settings_t *cfg)
{
    if (!cfg) {
        return false;
    }

    if (cfg->version != APP_SETTINGS_VERSION) {
        return false;
    }

    if (cfg->backlight_percent > 100) {
        return false;
    }

    if (cfg->audio_volume > 100) {
        return false;
    }

    if (cfg->idle_image_enabled > 1) {
        return false;
    }

    /*
     * 先确保 NVS 中读出的字符串都有 '\0' 结尾。
     */
    if (!is_c_string_terminated(cfg->wifi_ssid,
                                sizeof(cfg->wifi_ssid))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->wifi_password,
                                sizeof(cfg->wifi_password))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->fmo_host,
                                sizeof(cfg->fmo_host))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->ws_audio_url,
                                sizeof(cfg->ws_audio_url))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->ws_event_url,
                                sizeof(cfg->ws_event_url))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->ws_station_url,
                                sizeof(cfg->ws_station_url))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->owner_callsign,
                                sizeof(cfg->owner_callsign))) {
        return false;
    }

    /*
     * 当前规则：WiFi SSID 必须非空，且长度为 1~32 字节。
     *
     * 如果后续希望允许“未配置 WiFi”的状态，可以改成：
     *
     * if (cfg->wifi_ssid[0] != '\0' &&
     *     !wifi_ssid_is_valid(cfg->wifi_ssid)) {
     *     return false;
     * }
     */
    if (!wifi_ssid_is_valid(cfg->wifi_ssid)) {
        return false;
    }

    if (!wifi_password_is_valid(cfg->wifi_password)) {
        return false;
    }

    if (!owner_callsign_is_valid(cfg->owner_callsign)) {
        return false;
    }

    if (!is_non_empty_string(cfg->fmo_host)) {
        return false;
    }

    if (!is_non_empty_string(cfg->ws_audio_url)) {
        return false;
    }

    if (!is_non_empty_string(cfg->ws_event_url)) {
        return false;
    }

    if (!is_non_empty_string(cfg->ws_station_url)) {
        return false;
    }

    /*
     * 电池校准基本合法性。
     */
    if (cfg->battery_empty_mv < 2500 || cfg->battery_empty_mv > 4200) {
        return false;
    }

    if (cfg->battery_full_mv < 3500 || cfg->battery_full_mv > 4500) {
        return false;
    }

    if (cfg->battery_full_mv <= cfg->battery_empty_mv) {
        return false;
    }

    return true;
}

static void app_settings_sanitize(app_settings_t *cfg)
{
    if (!cfg) {
        return;
    }

    cfg->version = APP_SETTINGS_VERSION;

    if (cfg->backlight_percent > 100) {
        cfg->backlight_percent = 100;
    }

    if (cfg->audio_volume > 100) {
        cfg->audio_volume = 100;
    }

    cfg->idle_image_enabled = cfg->idle_image_enabled ? 1 : 0;

    /*
     * 先保证字符串有结尾，避免 strlen 越界。
     */
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';
    cfg->fmo_host[sizeof(cfg->fmo_host) - 1] = '\0';
    cfg->owner_callsign[sizeof(cfg->owner_callsign) - 1] = '\0';

    /*
     * 本机呼号非法则恢复默认。
     */
    if (!owner_callsign_is_valid(cfg->owner_callsign)) {
        ESP_LOGW(TAG, "owner_callsign invalid, restore default");

        copy_owner_callsign(cfg->owner_callsign,
                            sizeof(cfg->owner_callsign),
                            APP_DEFAULT_OWNER_CALLSIGN);
    } else {
        /*
         * 规范化成大写。
         */
        char tmp_owner[sizeof(cfg->owner_callsign)];

        safe_strcpy(tmp_owner, sizeof(tmp_owner), cfg->owner_callsign);

        copy_owner_callsign(cfg->owner_callsign,
                            sizeof(cfg->owner_callsign),
                            tmp_owner);
    }

    /*
     * WiFi SSID 超过 32 字节时不能简单截断。
     */
    if (cfg->wifi_ssid[0] != '\0') {
        size_t ssid_len = strlen(cfg->wifi_ssid);

        if (ssid_len > WIFI_SSID_MAX_BYTES) {
            ESP_LOGW(TAG,
                     "wifi_ssid too long in sanitize: %u bytes, clear it",
                     (unsigned)ssid_len);
            cfg->wifi_ssid[0] = '\0';
        }
    }

    /*
     * 密码超过 63 字节时清空。
     */
    if (cfg->wifi_password[0] != '\0') {
        size_t pwd_len = strlen(cfg->wifi_password);

        if (pwd_len > WIFI_PASSWORD_MAX_BYTES) {
            ESP_LOGW(TAG,
                     "wifi_password too long in sanitize: %u bytes, clear it",
                     (unsigned)pwd_len);
            cfg->wifi_password[0] = '\0';
        }
    }

    if (!is_non_empty_string(cfg->fmo_host)) {
        safe_strcpy(cfg->fmo_host, sizeof(cfg->fmo_host), DEFAULT_FMO_HOST);
    }

    /*
     * 保存前统一重新生成 WebSocket URL。
     */
    app_settings_build_ws_urls(cfg);

    cfg->ws_audio_url[sizeof(cfg->ws_audio_url) - 1] = '\0';
    cfg->ws_event_url[sizeof(cfg->ws_event_url) - 1] = '\0';
    cfg->ws_station_url[sizeof(cfg->ws_station_url) - 1] = '\0';

    if (cfg->ws_station_current_refresh_ms < 5000) {
        cfg->ws_station_current_refresh_ms =
            DEFAULT_WS_STATION_CURRENT_REFRESH_MS;
    }

    if (cfg->ws_station_list_refresh_ms < 30000) {
        cfg->ws_station_list_refresh_ms =
            DEFAULT_WS_STATION_LIST_REFRESH_MS;
    }

    if (cfg->idle_image_timeout_ms < 5000) {
        cfg->idle_image_timeout_ms = DEFAULT_IDLE_IMAGE_TIMEOUT_MS;
    }

    if (cfg->battery_empty_mv < 2500 || cfg->battery_empty_mv > 4200) {
        cfg->battery_empty_mv = BATTERY_PERCENT_EMPTY_MV;
    }

    if (cfg->battery_full_mv < 3500 || cfg->battery_full_mv > 4500) {
        cfg->battery_full_mv = BATTERY_PERCENT_FULL_MV;
    }

    if (cfg->battery_full_mv <= cfg->battery_empty_mv) {
        cfg->battery_empty_mv = BATTERY_PERCENT_EMPTY_MV;
        cfg->battery_full_mv = BATTERY_PERCENT_FULL_MV;
    }
}

/* -------------------------------------------------------------------------- */
/* Save / load / init                                                         */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_screen_rotate_180(bool enabled)
{
    app_settings_t tmp = s_settings;

    tmp.screen_rotate_180 = enabled;

    return app_settings_save(&tmp);
}

esp_err_t app_settings_save(const app_settings_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t tmp = *cfg;

    app_settings_sanitize(&tmp);

    /*
     * 保存前再次校验 WiFi SSID。
     *
     * 当前保持原逻辑：WiFi SSID 不能为空。
     * 如果后续希望支持“未配置 WiFi”状态，可在这里放宽限制。
     */
    if (!wifi_ssid_is_valid(tmp.wifi_ssid)) {
        ESP_LOGE(TAG,
                 "invalid wifi_ssid, len=%u",
                 (unsigned)strlen(tmp.wifi_ssid));
        return ESP_ERR_INVALID_ARG;
    }

    if (!wifi_password_is_valid(tmp.wifi_password)) {
        ESP_LOGE(TAG,
                 "invalid wifi_password, len=%u",
                 (unsigned)strlen(tmp.wifi_password));
        return ESP_ERR_INVALID_ARG;
    }

    if (!owner_callsign_is_valid(tmp.owner_callsign)) {
        ESP_LOGE(TAG, "invalid owner_callsign=%s", tmp.owner_callsign);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY_BLOB, &tmp, sizeof(tmp));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        s_settings = tmp;

        ESP_LOGI(TAG, "settings saved");
        ESP_LOGI(TAG, "owner_callsign=%s", s_settings.owner_callsign);
        ESP_LOGI(TAG,
                 "wifi_ssid=%s, len=%u bytes",
                 s_settings.wifi_ssid,
                 (unsigned)strlen(s_settings.wifi_ssid));
        ESP_LOGI(TAG, "fmo_host=%s", s_settings.fmo_host);
        ESP_LOGI(TAG, "ws_audio=%s", s_settings.ws_audio_url);
        ESP_LOGI(TAG, "ws_event=%s", s_settings.ws_event_url);
        ESP_LOGI(TAG, "ws_station=%s", s_settings.ws_station_url);
        ESP_LOGI(TAG,
                 "backlight=%u, volume=%u",
                 s_settings.backlight_percent,
                 s_settings.audio_volume);
        ESP_LOGI(TAG,
                 "battery empty=%u full=%u offset=%d",
                 s_settings.battery_empty_mv,
                 s_settings.battery_full_mv,
                 s_settings.battery_offset_mv);
    } else {
        ESP_LOGE(TAG, "settings save failed: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t app_settings_load(app_settings_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    *cfg = s_settings;

    return ESP_OK;
}

const app_settings_t *app_settings_get(void)
{
    return &s_settings;
}

esp_err_t app_settings_init(void)
{
    /*
     * 默认先加载默认配置，避免 NVS 读取失败时 s_settings 未初始化。
     */
    app_settings_load_defaults(&s_settings);

    nvs_handle_t handle;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "nvs_open failed, use defaults: %s",
                 esp_err_to_name(err));
        return err;
    }

    app_settings_t loaded;
    memset(&loaded, 0, sizeof(loaded));

    size_t required_size = sizeof(loaded);

    err = nvs_get_blob(handle, NVS_KEY_BLOB, &loaded, &required_size);
    nvs_close(handle);

    if (err == ESP_OK &&
        required_size == sizeof(loaded) &&
        app_settings_is_valid(&loaded)) {

        /*
         * 即使 NVS 中已有 URL，也重新按 fmo_host 生成一次，
         * 保证规则统一。
         */
        app_settings_sanitize(&loaded);

        s_settings = loaded;

        ESP_LOGI(TAG, "settings loaded from NVS");
        ESP_LOGI(TAG, "owner_callsign=%s", s_settings.owner_callsign);
        ESP_LOGI(TAG,
                 "wifi_ssid=%s, len=%u bytes",
                 s_settings.wifi_ssid,
                 (unsigned)strlen(s_settings.wifi_ssid));
        ESP_LOGI(TAG, "fmo_host=%s", s_settings.fmo_host);
        ESP_LOGI(TAG,
                 "backlight=%u, volume=%u, idle=%u",
                 s_settings.backlight_percent,
                 s_settings.audio_volume,
                 s_settings.idle_image_enabled);
        ESP_LOGI(TAG,
                 "battery empty=%u full=%u offset=%d",
                 s_settings.battery_empty_mv,
                 s_settings.battery_full_mv,
                 s_settings.battery_offset_mv);

        return ESP_OK;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "settings not found, save defaults");
    } else {
        ESP_LOGW(TAG,
                 "settings invalid or load failed, save defaults, err=%s, size=%u",
                 esp_err_to_name(err),
                 (unsigned)required_size);
    }

    /*
     * NVS 无配置或配置无效时，保存默认配置。
     */
    return app_settings_save(&s_settings);
}

/* -------------------------------------------------------------------------- */
/* Setters: WiFi and FMO                                                      */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid);

    if (ssid_len == 0 || ssid_len > WIFI_SSID_MAX_BYTES) {
        ESP_LOGE(TAG,
                 "invalid wifi ssid length: %u bytes, max %u bytes",
                 (unsigned)ssid_len,
                 (unsigned)WIFI_SSID_MAX_BYTES);
        return ESP_ERR_INVALID_ARG;
    }

    size_t pwd_len = password ? strlen(password) : 0;

    if (pwd_len > WIFI_PASSWORD_MAX_BYTES) {
        ESP_LOGE(TAG,
                 "invalid wifi password length: %u bytes, max %u bytes",
                 (unsigned)pwd_len,
                 (unsigned)WIFI_PASSWORD_MAX_BYTES);
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t tmp = s_settings;

    esp_err_t err = copy_wifi_ssid(tmp.wifi_ssid,
                                   sizeof(tmp.wifi_ssid),
                                   ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "copy_wifi_ssid failed: %s", esp_err_to_name(err));
        return err;
    }

    err = copy_wifi_password(tmp.wifi_password,
                             sizeof(tmp.wifi_password),
                             password ? password : "");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "copy_wifi_password failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "set wifi ssid=%s, len=%u bytes",
             tmp.wifi_ssid,
             (unsigned)strlen(tmp.wifi_ssid));

    return app_settings_save(&tmp);
}

esp_err_t app_settings_set_fmo_host(const char *host)
{
    if (!host || host[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t tmp = s_settings;

    /*
     * 当前保持原逻辑：超长 host 会被 safe_strcpy 截断。
     *
     * 如果后续需要更严格的行为，可改为：
     * if (strlen(host) >= sizeof(tmp.fmo_host)) {
     *     return ESP_ERR_INVALID_SIZE;
     * }
     */
    safe_strcpy(tmp.fmo_host, sizeof(tmp.fmo_host), host);

    app_settings_build_ws_urls(&tmp);

    return app_settings_save(&tmp);
}

/* -------------------------------------------------------------------------- */
/* Setters: audio and display                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_audio_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }

    app_settings_t tmp = s_settings;

    tmp.audio_volume = volume;

    return app_settings_save(&tmp);
}

esp_err_t app_settings_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    app_settings_t tmp = s_settings;

    tmp.backlight_percent = percent;

    return app_settings_save(&tmp);
}

/* -------------------------------------------------------------------------- */
/* Setters: battery calibration                                               */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_battery_calibration(uint16_t empty_mv,
                                               uint16_t full_mv,
                                               int16_t offset_mv)
{
    if (empty_mv < 2500 || empty_mv > 4200) {
        return ESP_ERR_INVALID_ARG;
    }

    if (full_mv < 3500 || full_mv > 4500) {
        return ESP_ERR_INVALID_ARG;
    }

    if (full_mv <= empty_mv) {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t tmp = s_settings;

    tmp.battery_empty_mv = empty_mv;
    tmp.battery_full_mv = full_mv;
    tmp.battery_offset_mv = offset_mv;

    return app_settings_save(&tmp);
}

/* -------------------------------------------------------------------------- */
/* Setters: owner callsign                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_owner_callsign(const char *callsign)
{
    if (!callsign || callsign[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t tmp = s_settings;

    esp_err_t err = copy_owner_callsign(tmp.owner_callsign,
                                        sizeof(tmp.owner_callsign),
                                        callsign);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "copy_owner_callsign failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "set owner_callsign=%s", tmp.owner_callsign);

    return app_settings_save(&tmp);
}

/* -------------------------------------------------------------------------- */
/* Setters: QSO state                                                         */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_qso_state(uint32_t count,
                                     int32_t latest_log_id,
                                     bool valid)
{
    app_settings_t tmp = s_settings;

    tmp.qso_count = count;
    tmp.qso_latest_log_id = latest_log_id;
    tmp.qso_count_valid = valid;

    esp_err_t ret = app_settings_save(&tmp);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "qso state saved: valid=%d count=%lu latest=%ld",
                 valid ? 1 : 0,
                 (unsigned long)count,
                 (long)latest_log_id);
    } else {
        ESP_LOGW(TAG,
                 "qso state save failed: %s",
                 esp_err_to_name(ret));
    }

    return ret;
}
