/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file wifi_manager.c
 * @brief WiFi STA 连接、状态查询、扫描与省电恢复实现。
 *
 * 本模块负责：
 * - 初始化 ESP-NETIF / ESP-EVENT / WiFi STA；
 * - 根据 app_settings 中保存的 WiFi 配置连接 AP；
 * - 处理 WiFi 与 IP 事件；
 * - 断线自动重连；
 * - 获取 RSSI；
 * - 同步扫描附近 AP；
 * - 在省电模式下停止 WiFi，并在退出省电后恢复。
 */

#include "wifi_manager.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FreeRTOS headers --------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

/* Project headers ---------------------------------------------------------- */
#include "app_power_save.h"
#include "app_settings.h"
#include "audio_ws.h"
#include "ui_async.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "wifi_manager";

/* -------------------------------------------------------------------------- */
/* Private macros                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi 最大重连次数。
 */
#define WIFI_MAXIMUM_RETRY       5

/**
 * @brief WiFi SSID 最大字节数。
 *
 * 802.11 SSID 最大 32 字节。
 */
#define WIFI_SSID_MAX_BYTES      32

/**
 * @brief WiFi 密码最大字节数。
 */
#define WIFI_PASSWORD_MAX_BYTES  63

/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi 底层是否已初始化。
 */
static bool s_wifi_inited = false;

/**
 * @brief WiFi 是否已经 start。
 */
static bool s_wifi_started = false;

/**
 * @brief 当前是否正在主动停止 WiFi。
 *
 * 用于阻止 WIFI_EVENT_STA_DISCONNECTED 中的自动重连。
 */
static bool s_wifi_stopping = false;

/**
 * @brief 当前是否已连接 AP 并获得 IP。
 */
static bool s_wifi_connected = false;

/**
 * @brief 最近一次获取到的 RSSI。
 */
static int s_wifi_rssi = 0;

/**
 * @brief 当前重连次数。
 */
static int s_retry_num = 0;

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static void log_hex_bytes(const char *name,
                          const uint8_t *data,
                          size_t len);

static const char *wifi_disconnect_reason_to_name(int reason);

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data);

static void wifi_init_sta(void);

/* -------------------------------------------------------------------------- */
/* Debug helpers                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 打印字节序列，便于调试中文 SSID 等非 ASCII 文本。
 */
static void log_hex_bytes(const char *name,
                          const uint8_t *data,
                          size_t len)
{
    if (!name || !data) {
        return;
    }

    char buf[160];
    size_t pos = 0;

    memset(buf, 0, sizeof(buf));

    for (size_t i = 0; i < len && pos + 4 < sizeof(buf); i++) {
        pos += snprintf(buf + pos,
                        sizeof(buf) - pos,
                        "%02X ",
                        data[i]);
    }

    ESP_LOGI(TAG, "%s hex: %s", name, buf);
}

/**
 * @brief 将 WiFi 断开原因转换为字符串。
 *
 * 使用数字判断，避免不同 ESP-IDF 版本中个别枚举名不存在导致编译失败。
 */
static const char *wifi_disconnect_reason_to_name(int reason)
{
    switch (reason) {
    case 1:
        return "UNSPECIFIED";
    case 2:
        return "AUTH_EXPIRE";
    case 3:
        return "AUTH_LEAVE";
    case 4:
        return "ASSOC_EXPIRE";
    case 5:
        return "ASSOC_TOOMANY";
    case 6:
        return "NOT_AUTHED";
    case 7:
        return "NOT_ASSOCED";
    case 8:
        return "ASSOC_LEAVE";
    case 9:
        return "ASSOC_NOT_AUTHED";
    case 15:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case 16:
        return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17:
        return "IE_IN_4WAY_DIFFERS";
    case 18:
        return "GROUP_CIPHER_INVALID";
    case 19:
        return "PAIRWISE_CIPHER_INVALID";
    case 20:
        return "AKMP_INVALID";
    case 21:
        return "UNSUPP_RSN_IE_VERSION";
    case 22:
        return "INVALID_RSN_IE_CAP";
    case 23:
        return "802_1X_AUTH_FAILED";
    case 24:
        return "CIPHER_SUITE_REJECTED";
    case 200:
        return "BEACON_TIMEOUT";
    case 201:
        return "NO_AP_FOUND";
    case 202:
        return "AUTH_FAIL";
    case 203:
        return "ASSOC_FAIL";
    case 204:
        return "HANDSHAKE_TIMEOUT";
    case 205:
        return "CONNECTION_FAIL";
    case 206:
        return "AP_TSF_RESET";
    case 207:
        return "ROAMING";
    default:
        return "UNKNOWN";
    }
}

/* -------------------------------------------------------------------------- */
/* WiFi event handling                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi / IP 事件处理函数。
 */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START");

        s_wifi_started = true;

        /*
         * 如果正在省电停止 WiFi，不要连接。
         */
        if (s_wifi_stopping || app_power_save_is_active()) {
            ESP_LOGW(TAG, "wifi start ignored because power save/stopping");
            return;
        }

        esp_wifi_connect();

        ui_async_update_status("WiFi连接中");
        return;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc =
            (wifi_event_sta_disconnected_t *)event_data;

        int reason = disc ? disc->reason : -1;

        ESP_LOGW(TAG,
                 "WiFi disconnected, reason=%d (%s)",
                 reason,
                 wifi_disconnect_reason_to_name(reason));

        s_wifi_connected = false;
        s_wifi_rssi = 0;

        /*
         * 如果是省电模式主动关闭 WiFi，不自动重连。
         */
        if (s_wifi_stopping || app_power_save_is_active()) {
            ESP_LOGW(TAG, "WiFi disconnected by power save, no reconnect");

            ui_async_update_wifi_rssi(-127);
            ui_async_update_status("WiFi已关闭");
            return;
        }

        /*
         * 根据常见原因给出更明确的 UI 状态。
         */
        if (reason == 201) {
            ui_async_update_status("未找到WiFi");
        } else if (reason == 202 || reason == 15 || reason == 204) {
            ui_async_update_status("WiFi认证失败");
        } else if (reason == 200) {
            ui_async_update_status("WiFi信号异常");
        } else {
            ui_async_update_status("WiFi断开");
        }

        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            s_retry_num++;

            ESP_LOGI(TAG,
                     "retry to connect to the AP, attempt %d",
                     s_retry_num);

            /*
             * 注意：
             * 事件回调中不建议长时间阻塞。
             * 当前保留 300ms 延迟以避免 AP 刚踢下线就立即重连。
             * 如果后续需要更规范，可改为延迟重连任务或 esp_timer。
             */
            vTaskDelay(pdMS_TO_TICKS(300));

            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "connect failed");
            ui_async_update_status("WiFi连接失败");
        }

        return;
    }

    if (event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        if (event) {
            ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        } else {
            ESP_LOGI(TAG, "got ip");
        }

        s_wifi_connected = true;
        s_wifi_started = true;
        s_wifi_stopping = false;
        s_retry_num = 0;

        ui_async_update_status("WiFi已连接");

        /*
         * 正常模式下，WiFi 获取 IP 后启动/恢复 WebSocket。
         * audio_ws_start() 内部应具备防重复启动逻辑。
         */
        if (!app_power_save_is_active()) {
            audio_ws_start();
        }

        return;
    }
}

/* -------------------------------------------------------------------------- */
/* WiFi initialization                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化并启动 WiFi STA。
 *
 * 该函数只在首次启动时调用。
 */
static void wifi_init_sta(void)
{
    const app_settings_t *cfg = app_settings_get();

    if (!cfg) {
        ESP_LOGE(TAG, "settings is null");
        return;
    }

    if (cfg->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "WiFi SSID empty, skip connect");
        ui_async_update_status("未配置WiFi");
        return;
    }

    /*
     * ESP32 / ESP-IDF 的 SSID 最大长度是 32 字节，不是 32 个字符。
     * app_settings_t.wifi_ssid 当前为 char[33]，
     * 可保存 32 字节 SSID + '\0'。
     */
    size_t ssid_len = strlen(cfg->wifi_ssid);
    size_t pwd_len = strlen(cfg->wifi_password);

    ESP_LOGI(TAG, "connect ssid: %s", cfg->wifi_ssid);
    ESP_LOGI(TAG, "ssid length: %d bytes", (int)ssid_len);
    ESP_LOGI(TAG, "password length: %d bytes", (int)pwd_len);
    log_hex_bytes("ssid", (const uint8_t *)cfg->wifi_ssid, ssid_len);

    if (ssid_len == 0) {
        ESP_LOGW(TAG, "WiFi SSID empty");
        ui_async_update_status("WiFi名称为空");
        return;
    }

    if (ssid_len > WIFI_SSID_MAX_BYTES) {
        ESP_LOGE(TAG,
                 "WiFi SSID too long: %d bytes, max %d bytes",
                 (int)ssid_len,
                 WIFI_SSID_MAX_BYTES);
        ui_async_update_status("WiFi名称过长");
        return;
    }

    if (pwd_len > WIFI_PASSWORD_MAX_BYTES) {
        ESP_LOGE(TAG,
                 "WiFi password too long: %d bytes, max %d bytes",
                 (int)pwd_len,
                 WIFI_PASSWORD_MAX_BYTES);
        ui_async_update_status("WiFi密码过长");
        return;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler,
                                               NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler,
                                               NULL));

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));

    /*
     * 中文 SSID 按 UTF-8 原始字节复制。
     * wifi_config 已经清零，ssid_len < 32 时自然有 '\0'。
     * ssid_len == 32 时也合法，因为 ESP-IDF SSID 是字节数组。
     */
    memcpy(wifi_config.sta.ssid, cfg->wifi_ssid, ssid_len);

    /*
     * password 数组长度是 64。
     * 前面已经限制 pwd_len <= 63。
     * wifi_config 已经清零，复制后自然有 '\0' 结尾。
     */
    if (pwd_len > 0) {
        memcpy(wifi_config.sta.password, cfg->wifi_password, pwd_len);
    }

    /*
     * 扫描所有信道，按信号选择 AP。
     * 对中文 SSID、多 AP 同名、路由器切信道更稳。
     */
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    /*
     * 如果有密码：
     *   最低认证方式设为 WPA_PSK，兼容 WPA/WPA2/WPA3 mixed。
     *
     * 如果无密码：
     *   允许 OPEN。
     */
    if (pwd_len > 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    /*
     * 兼容 WPA2/WPA3 mixed。
     * required=false 很重要，否则某些 WPA2 AP 可能连不上。
     */
    wifi_config.sta.pmf_cfg.capable = false;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG,
             "wifi config prepared, auth_threshold=%d, pmf capable=%d required=%d",
             wifi_config.sta.threshold.authmode,
             wifi_config.sta.pmf_cfg.capable,
             wifi_config.sta.pmf_cfg.required);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /*
     * 2.4G 兼容模式。
     */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
                                          WIFI_PROTOCOL_11B |
                                          WIFI_PROTOCOL_11G |
                                          WIFI_PROTOCOL_11N));

    /*
     * 固定 20 MHz，部分路由器/弱信号环境更稳。
     */
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));

    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_started = true;
    s_wifi_stopping = false;
    s_retry_num = 0;

    /*
     * 关闭 WiFi 省电模式，提高连接稳定性和音频实时性。
     */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi power save disabled");

    ui_async_update_status("WiFi启动完成");
}

/* -------------------------------------------------------------------------- */
/* Public interfaces: start / stop / restart                                  */
/* -------------------------------------------------------------------------- */

esp_err_t wifi_manager_start(void)
{
    if (s_wifi_inited) {
        if (s_wifi_started) {
            ESP_LOGW(TAG, "wifi already started");
            return ESP_OK;
        }

        /*
         * 已初始化但当前停止，直接 restart。
         */
        return wifi_manager_restart();
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG,
                 "esp_event_loop_create_default failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    s_wifi_inited = true;
    s_wifi_started = false;
    s_wifi_stopping = false;
    s_retry_num = 0;

    wifi_init_sta();

    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    ESP_LOGW(TAG, "wifi_manager_stop");

    if (!s_wifi_inited) {
        return ESP_OK;
    }

    /*
     * 设置停止标志，防止 DISCONNECTED 事件中自动重连。
     */
    s_wifi_stopping = true;
    s_wifi_connected = false;
    s_wifi_rssi = 0;
    s_retry_num = 0;

    ui_async_update_wifi_rssi(-127);

    /*
     * 如果 WiFi 本来就没启动，直接返回。
     */
    if (!s_wifi_started) {
        ui_async_update_status("WiFi已关闭");
        return ESP_OK;
    }

    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK &&
        ret != ESP_ERR_WIFI_NOT_CONNECT &&
        ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG,
                 "esp_wifi_disconnect failed: %s",
                 esp_err_to_name(ret));
    }

    ret = esp_wifi_stop();
    if (ret == ESP_OK ||
        ret == ESP_ERR_WIFI_NOT_STARTED ||
        ret == ESP_ERR_WIFI_NOT_INIT) {

        s_wifi_started = false;
        s_wifi_connected = false;

        ui_async_update_wifi_rssi(-127);
        ui_async_update_status("WiFi已关闭");

        return ESP_OK;
    }

    ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(ret));

    return ret;
}

esp_err_t wifi_manager_restart(void)
{
    ESP_LOGW(TAG, "wifi_manager_restart");

    if (!s_wifi_inited) {
        return wifi_manager_start();
    }

    if (s_wifi_started) {
        ESP_LOGW(TAG, "wifi already started");
        return ESP_OK;
    }

    /*
     * 退出省电恢复时，允许 STA_START 事件连接 AP。
     */
    s_wifi_stopping = false;
    s_wifi_connected = false;
    s_wifi_rssi = 0;
    s_retry_num = 0;

    esp_err_t ret = esp_wifi_start();
    if (ret == ESP_OK) {
        s_wifi_started = true;
        ui_async_update_status("WiFi恢复中");
    } else if (ret == ESP_ERR_WIFI_NOT_STOPPED) {
        s_wifi_started = true;
        ret = ESP_OK;
    } else {
        ESP_LOGW(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* -------------------------------------------------------------------------- */
/* Public interfaces: state query                                             */
/* -------------------------------------------------------------------------- */

bool wifi_manager_is_started(void)
{
    return s_wifi_started;
}

bool wifi_manager_is_connected(void)
{
    return s_wifi_connected;
}

int wifi_manager_get_rssi(void)
{
    if (!s_wifi_connected) {
        return 0;
    }

    wifi_ap_record_t ap_info;

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        s_wifi_rssi = ap_info.rssi;
        return s_wifi_rssi;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Public interfaces: scan                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t wifi_manager_scan(wifi_scan_item_t *items,
                            int max_items,
                            int *out_count)
{
    if (!items || max_items <= 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;

    if (!s_wifi_inited || !s_wifi_started || app_power_save_is_active()) {
        ESP_LOGW(TAG, "wifi not available, cannot scan");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 50,
        .scan_time.active.max = 120,
    };

    ESP_LOGI(TAG, "start wifi scan");

    /*
     * 第二个参数 true 表示阻塞等待扫描完成。
     * 因此不要在 LVGL 线程中直接调用该函数。
     */
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "wifi scan start failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    uint16_t ap_count = 0;

    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "wifi scan get ap num failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    if (ap_count == 0) {
        ESP_LOGI(TAG, "wifi scan found 0 ap");
        return ESP_OK;
    }

    wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!records) {
        ESP_LOGE(TAG, "alloc scan records failed");
        return ESP_ERR_NO_MEM;
    }

    uint16_t record_count = ap_count;

    ret = esp_wifi_scan_get_ap_records(&record_count, records);
    if (ret != ESP_OK) {
        free(records);

        ESP_LOGW(TAG,
                 "wifi scan get records failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /*
     * 只取前 max_items 个。
     * ESP-IDF 扫描结果通常按信号排序，但不完全保证。
     */
    int count = 0;

    for (int i = 0; i < record_count && count < max_items; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }

        memset(&items[count], 0, sizeof(wifi_scan_item_t));

        /*
         * SSID 最大 32 bytes。
         */
        memcpy(items[count].ssid, records[i].ssid, WIFI_SSID_MAX_BYTES);
        items[count].ssid[WIFI_SSID_MAX_BYTES] = '\0';

        items[count].rssi = records[i].rssi;
        items[count].authmode = records[i].authmode;
        items[count].channel = records[i].primary;

        ESP_LOGI(TAG,
                 "scan[%d]: ssid=%s rssi=%d auth=%d ch=%d",
                 count,
                 items[count].ssid,
                 items[count].rssi,
                 items[count].authmode,
                 items[count].channel);

        count++;
    }

    free(records);

    *out_count = count;

    ESP_LOGI(TAG, "wifi scan done, count=%d", count);

    return ESP_OK;
}
