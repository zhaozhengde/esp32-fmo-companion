/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file bt_audio.c
 * @brief 蓝牙 A2DP Source 音频输出实现。
 *
 * 本模块在启用 APP_CONFIG_BT_AUDIO_ENABLE 时提供蓝牙音频输出能力：
 * - 初始化 Classic Bluetooth；
 * - 扫描并按名称匹配目标蓝牙音箱/耳机；
 * - 建立 A2DP Source 连接；
 * - 接收上层 PCM 数据；
 * - 转换为 44.1kHz / stereo / 16-bit PCM；
 * - 通过 A2DP data callback 输出到蓝牙设备。
 *
 * 当 APP_CONFIG_BT_AUDIO_ENABLE 为 0 时，本文件提供空实现，
 * 以保证其他模块可以正常链接。
 */

#include "bt_audio.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_err.h"
#include "esp_log.h"

/* Project headers ---------------------------------------------------------- */
#include "app_config.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "bt_audio";

#if APP_CONFIG_BT_AUDIO_ENABLE

/* FreeRTOS headers --------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF Bluetooth headers ------------------------------------------------ */
#include "esp_a2dp_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

/* Project headers ---------------------------------------------------------- */
#include "audio_output.h"
#include "ui_async.h"

/* -------------------------------------------------------------------------- */
/* Private macros                                                             */
/* -------------------------------------------------------------------------- */

/*
 * A2DP Source 常用输出格式。
 */
#define BT_OUT_SAMPLE_RATE       APP_CONFIG_BT_SAMPLE_RATE
#define BT_OUT_CHANNELS          APP_CONFIG_BT_CHANNELS
#define BT_OUT_BITS              APP_CONFIG_BT_BITS_PER_SAMPLE

/*
 * 蓝牙 PCM ring buffer。
 *
 * ESP32-WROOM-32E-N4 无 PSRAM，DRAM 较紧张。
 * 不建议把蓝牙音频 ring buffer 放到静态 .bss。
 *
 * 当前先使用 8KB。
 * 如果后续声音卡顿，可尝试增大到 12KB / 16KB。
 */
#define BT_PCM_RING_SIZE         (8 * 1024)

/*
 * PCM 转换临时缓冲。
 * 不宜过大，避免增加调用栈压力。
 */
#define BT_CONVERT_TMP_SIZE      1024

/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

static bool s_bt_inited = false;
static bool s_bt_started = false;
static bool s_bt_connected = false;
static bool s_bt_connecting = false;
static bool s_bt_discovery_started = false;

/**
 * @brief 目标蓝牙设备地址。
 */
static esp_bd_addr_t s_target_bda = {0};

/**
 * @brief 蓝牙 PCM ring buffer。
 */
static uint8_t *s_bt_ring = NULL;
static size_t s_bt_ring_size = 0;

static volatile size_t s_ring_r = 0;
static volatile size_t s_ring_w = 0;
static volatile size_t s_ring_used = 0;

static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 简单升采样累加器。
 */
static uint32_t s_resample_acc = 0;

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static void bt_ring_reset(void);
static size_t bt_ring_free_no_lock(void);
static size_t bt_ring_write(const uint8_t *data, size_t len);
static size_t bt_ring_read(uint8_t *out, size_t len);

static bool bt_eir_get_name(const uint8_t *eir,
                            char *name,
                            size_t name_len);
static void bt_bda_to_str(const esp_bd_addr_t bda,
                          char *str,
                          size_t size);

static int32_t bt_a2d_data_cb(uint8_t *data, int32_t len);
static void bt_a2d_cb(esp_a2d_cb_event_t event,
                      esp_a2d_cb_param_t *param);
static void bt_gap_cb(esp_bt_gap_cb_event_t event,
                      esp_bt_gap_cb_param_t *param);

/* -------------------------------------------------------------------------- */
/* Ring buffer                                                                */
/* -------------------------------------------------------------------------- */

static void bt_ring_reset(void)
{
    portENTER_CRITICAL(&s_ring_mux);

    s_ring_r = 0;
    s_ring_w = 0;
    s_ring_used = 0;

    portEXIT_CRITICAL(&s_ring_mux);
}

static size_t bt_ring_free_no_lock(void)
{
    return s_bt_ring_size - s_ring_used;
}

static size_t bt_ring_write(const uint8_t *data, size_t len)
{
    if (!s_bt_ring || s_bt_ring_size == 0) {
        return 0;
    }

    if (!data || len == 0) {
        return 0;
    }

    if (len > s_bt_ring_size) {
        /*
         * 如果单次数据超过 ring 大小，只保留最后一段，
         * 以保证实时性。
         */
        data += (len - s_bt_ring_size);
        len = s_bt_ring_size;
    }

    portENTER_CRITICAL(&s_ring_mux);

    /*
     * 空间不够时丢弃旧数据，保证实时性优先。
     */
    while (bt_ring_free_no_lock() < len && s_ring_used > 0) {
        s_ring_r = (s_ring_r + 1) % s_bt_ring_size;
        s_ring_used--;
    }

    for (size_t i = 0; i < len; i++) {
        s_bt_ring[s_ring_w] = data[i];
        s_ring_w = (s_ring_w + 1) % s_bt_ring_size;
    }

    s_ring_used += len;

    portEXIT_CRITICAL(&s_ring_mux);

    return len;
}

static size_t bt_ring_read(uint8_t *out, size_t len)
{
    if (!s_bt_ring || s_bt_ring_size == 0) {
        return 0;
    }

    if (!out || len == 0) {
        return 0;
    }

    size_t read_len = 0;

    portENTER_CRITICAL(&s_ring_mux);

    while (read_len < len && s_ring_used > 0) {
        out[read_len++] = s_bt_ring[s_ring_r];
        s_ring_r = (s_ring_r + 1) % s_bt_ring_size;
        s_ring_used--;
    }

    portEXIT_CRITICAL(&s_ring_mux);

    return read_len;
}

/* -------------------------------------------------------------------------- */
/* GAP helpers                                                                */
/* -------------------------------------------------------------------------- */

static bool bt_eir_get_name(const uint8_t *eir,
                            char *name,
                            size_t name_len)
{
    if (!eir || !name || name_len == 0) {
        return false;
    }

    uint8_t len = 0;
    uint8_t *data = NULL;

    data = esp_bt_gap_resolve_eir_data((uint8_t *)eir,
                                       ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,
                                       &len);
    if (!data) {
        data = esp_bt_gap_resolve_eir_data((uint8_t *)eir,
                                           ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME,
                                           &len);
    }

    if (!data || len == 0) {
        return false;
    }

    if (len >= name_len) {
        len = name_len - 1;
    }

    memcpy(name, data, len);
    name[len] = '\0';

    return true;
}

static void bt_bda_to_str(const esp_bd_addr_t bda,
                          char *str,
                          size_t size)
{
    if (!str || size < 18) {
        return;
    }

    snprintf(str,
             size,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0],
             bda[1],
             bda[2],
             bda[3],
             bda[4],
             bda[5]);
}

/* -------------------------------------------------------------------------- */
/* A2DP data callback                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief A2DP Source 数据回调。
 *
 * A2DP Source 会周期性调用该函数获取 PCM 数据。
 * 这里必须尽快返回，不能执行耗时操作。
 *
 * @param data 输出缓冲区。
 * @param len 需要填充的字节数。
 *
 * @return 实际提供的字节数。
 */
static int32_t bt_a2d_data_cb(uint8_t *data, int32_t len)
{
    if (!data || len <= 0) {
        return 0;
    }

    size_t got = bt_ring_read(data, (size_t)len);

    if (got < (size_t)len) {
        /*
         * 数据不足时补静音，避免蓝牙断流。
         */
        memset(data + got, 0, (size_t)len - got);
    }

    return len;
}

/* -------------------------------------------------------------------------- */
/* A2DP callback                                                              */
/* -------------------------------------------------------------------------- */

static void bt_a2d_cb(esp_a2d_cb_event_t event,
                      esp_a2d_cb_param_t *param)
{
    if (!param) {
        return;
    }

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        esp_a2d_connection_state_t state = param->conn_stat.state;

        char bda_str[18] = {0};
        bt_bda_to_str(param->conn_stat.remote_bda,
                      bda_str,
                      sizeof(bda_str));

        ESP_LOGI(TAG,
                 "A2DP connection state=%d, peer=%s",
                 state,
                 bda_str);

        if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_bt_connected = true;
            s_bt_connecting = false;
            bt_ring_reset();

            ui_async_update_status("蓝牙已连接");
            audio_output_on_bt_connected();
        } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_bt_connected = false;
            s_bt_connecting = false;
            bt_ring_reset();

            ui_async_update_status("蓝牙断开");
            audio_output_on_bt_disconnected();
        }

        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG,
                 "A2DP audio state=%d",
                 param->audio_stat.state);

        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            ui_async_update_status("蓝牙播放");
        }

        break;

    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(TAG, "A2DP audio cfg");
        break;

    default:
        ESP_LOGI(TAG, "A2DP event=%d", event);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* GAP callback                                                               */
/* -------------------------------------------------------------------------- */

static void bt_gap_cb(esp_bt_gap_cb_event_t event,
                      esp_bt_gap_cb_param_t *param)
{
    if (!param) {
        return;
    }

    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        char name[64] = {0};
        bool has_name = false;

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];

            if (p->type == ESP_BT_GAP_DEV_PROP_EIR) {
                has_name = bt_eir_get_name((const uint8_t *)p->val,
                                           name,
                                           sizeof(name));
            } else if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                int len = p->len;

                if (len >= (int)sizeof(name)) {
                    len = sizeof(name) - 1;
                }

                memcpy(name, p->val, len);
                name[len] = '\0';
                has_name = true;
            }
        }

        if (has_name) {
            char bda_str[18] = {0};

            bt_bda_to_str(param->disc_res.bda,
                          bda_str,
                          sizeof(bda_str));

            ESP_LOGI(TAG,
                     "BT found: name=%s, bda=%s",
                     name,
                     bda_str);

            if (strcmp(name, APP_CONFIG_BT_TARGET_NAME) == 0) {
                ESP_LOGI(TAG, "BT target matched: %s", name);

                memcpy(s_target_bda,
                       param->disc_res.bda,
                       sizeof(esp_bd_addr_t));

                if (s_bt_discovery_started) {
                    esp_bt_gap_cancel_discovery();
                    s_bt_discovery_started = false;
                }

                if (!s_bt_connected && !s_bt_connecting) {
                    s_bt_connecting = true;
                    ui_async_update_status("蓝牙连接中");

                    esp_err_t ret = esp_a2d_source_connect(s_target_bda);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG,
                                 "esp_a2d_source_connect failed: %s",
                                 esp_err_to_name(ret));

                        s_bt_connecting = false;
                        ui_async_update_status("蓝牙连接失败");
                    }
                }
            }
        }

        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        ESP_LOGI(TAG,
                 "BT discovery state=%d",
                 param->disc_st_chg.state);

        if (param->disc_st_chg.state ==
            ESP_BT_GAP_DISCOVERY_STARTED) {
            s_bt_discovery_started = true;
            ui_async_update_status("蓝牙扫描中");
        } else if (param->disc_st_chg.state ==
                   ESP_BT_GAP_DISCOVERY_STOPPED) {
            s_bt_discovery_started = false;

            if (!s_bt_connected && !s_bt_connecting) {
                ui_async_update_status("蓝牙未连接");
            }
        }

        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG,
                     "BT auth success: %s",
                     param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(TAG,
                     "BT auth failed, status=%d",
                     param->auth_cmpl.stat);
        }

        break;

    default:
        ESP_LOGI(TAG, "GAP event=%d", event);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t bt_audio_init(void)
{
    if (s_bt_inited) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "BT audio init");

    /*
     * 不使用 BLE，释放 BLE 内存。
     * 如果未来需要 BLE 配网，则不能释放 BLE。
     */
    esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG,
                 "release BLE mem failed: %s",
                 esp_err_to_name(ret));
    }

    esp_bt_controller_config_t bt_cfg =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_bt_controller_init failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_bt_controller_enable failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_bluedroid_init failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_bluedroid_enable failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_gap_register_callback(bt_gap_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_bt_gap_register_callback failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_register_callback(bt_a2d_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_a2d_register_callback failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_register_data_callback(bt_a2d_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_a2d_source_register_data_callback failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_a2d_source_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_a2d_source_init failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_dev_set_device_name(APP_CONFIG_BT_DEVICE_NAME);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "esp_bt_dev_set_device_name failed: %s",
                 esp_err_to_name(ret));
    }

    /*
     * 设置本机蓝牙为可连接、可发现。
     */
    ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                   ESP_BT_GENERAL_DISCOVERABLE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "esp_bt_gap_set_scan_mode failed: %s",
                 esp_err_to_name(ret));
    }

    /*
     * 运行时分配蓝牙音频 ring buffer，避免占用静态 .bss。
     */
    if (!s_bt_ring) {
        s_bt_ring = heap_caps_malloc(BT_PCM_RING_SIZE, MALLOC_CAP_8BIT);
        if (!s_bt_ring) {
            ESP_LOGE(TAG,
                     "alloc BT ring buffer failed, size=%d",
                     BT_PCM_RING_SIZE);
            return ESP_ERR_NO_MEM;
        }

        s_bt_ring_size = BT_PCM_RING_SIZE;

        ESP_LOGI(TAG,
                 "BT ring allocated, size=%u, free heap=%u",
                 (unsigned)s_bt_ring_size,
                 (unsigned)esp_get_free_heap_size());
    }

    bt_ring_reset();

    s_bt_inited = true;

    ESP_LOGI(TAG, "BT audio init done");

    return ESP_OK;
}

esp_err_t bt_audio_start(void)
{
    if (!s_bt_inited) {
        esp_err_t ret = bt_audio_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (s_bt_connected || s_bt_connecting) {
        ESP_LOGI(TAG, "BT already connected/connecting");
        return ESP_OK;
    }

    if (s_bt_discovery_started) {
        ESP_LOGI(TAG, "BT discovery already started");
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "start BT discovery, target=%s",
             APP_CONFIG_BT_TARGET_NAME);

    ui_async_update_status("蓝牙扫描中");

    /*
     * 查询 10 * 1.28s ≈ 12.8s。
     */
    esp_err_t ret = esp_bt_gap_start_discovery(
        ESP_BT_INQ_MODE_GENERAL_INQUIRY,
        10,
        0
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_bt_gap_start_discovery failed: %s",
                 esp_err_to_name(ret));

        ui_async_update_status("蓝牙扫描失败");
        return ret;
    }

    s_bt_started = true;
    s_bt_discovery_started = true;

    return ESP_OK;
}

esp_err_t bt_audio_stop(void)
{
    if (!s_bt_inited) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "BT audio stop");

    if (s_bt_discovery_started) {
        esp_bt_gap_cancel_discovery();
        s_bt_discovery_started = false;
    }

    if (s_bt_connected) {
        esp_a2d_source_disconnect(s_target_bda);
    }

    s_bt_connected = false;
    s_bt_connecting = false;
    s_bt_started = false;

    bt_ring_reset();

    audio_output_on_bt_disconnected();

    return ESP_OK;
}

bool bt_audio_is_connected(void)
{
    return s_bt_connected;
}

esp_err_t bt_audio_write_pcm(const uint8_t *pcm_data,
                             int len,
                             int sample_rate,
                             int channels,
                             int bits_per_sample)
{
    if (!s_bt_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!pcm_data || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sample_rate <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (bits_per_sample != 16) {
        ESP_LOGW(TAG, "BT unsupported bits=%d", bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (channels != 1 && channels != 2) {
        ESP_LOGW(TAG, "BT unsupported channels=%d", channels);
        return ESP_ERR_NOT_SUPPORTED;
    }

    int bytes_per_frame = channels * (int)sizeof(int16_t);

    if (bytes_per_frame <= 0 || (len % bytes_per_frame) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int16_t *in = (const int16_t *)pcm_data;
    int in_frames = len / bytes_per_frame;

    if (in_frames <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tmp[BT_CONVERT_TMP_SIZE];
    size_t tmp_pos = 0;

    /*
     * 把输入 PCM 转成 A2DP Source 常用的 44.1kHz stereo 16-bit PCM。
     *
     * 当前实现：
     * - 输入要求 16-bit PCM；
     * - mono 输入：复制成 L/R；
     * - stereo 输入：取左声道再复制到 L/R；
     * - 升采样：零阶保持，语音场景可接受。
     */
    for (int i = 0; i < in_frames; i++) {
        int16_t mono;

        if (channels == 1) {
            mono = in[i];
        } else {
            mono = in[i * 2];
        }

        /*
         * 简单升采样：
         * 每个输入 sample 根据比例生成若干个输出 sample。
         */
        s_resample_acc += BT_OUT_SAMPLE_RATE;

        while (s_resample_acc >= (uint32_t)sample_rate) {
            s_resample_acc -= (uint32_t)sample_rate;

            /*
             * 输出 stereo 16-bit：L + R，共 4 字节。
             */
            if (tmp_pos + 4 > sizeof(tmp)) {
                bt_ring_write(tmp, tmp_pos);
                tmp_pos = 0;
            }

            tmp[tmp_pos++] = (uint8_t)(mono & 0xFF);
            tmp[tmp_pos++] = (uint8_t)((mono >> 8) & 0xFF);
            tmp[tmp_pos++] = (uint8_t)(mono & 0xFF);
            tmp[tmp_pos++] = (uint8_t)((mono >> 8) & 0xFF);
        }
    }

    if (tmp_pos > 0) {
        bt_ring_write(tmp, tmp_pos);
    }

    return ESP_OK;
}

#else /* APP_CONFIG_BT_AUDIO_ENABLE */

/* -------------------------------------------------------------------------- */
/* Stubs when Bluetooth audio is disabled                                     */
/* -------------------------------------------------------------------------- */

esp_err_t bt_audio_init(void)
{
    ESP_LOGI(TAG, "BT audio disabled");
    return ESP_OK;
}

esp_err_t bt_audio_start(void)
{
    ESP_LOGW(TAG, "BT audio disabled");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bt_audio_stop(void)
{
    return ESP_OK;
}

bool bt_audio_is_connected(void)
{
    return false;
}

esp_err_t bt_audio_write_pcm(const uint8_t *pcm_data,
                             int len,
                             int sample_rate,
                             int channels,
                             int bits_per_sample)
{
    (void)pcm_data;
    (void)len;
    (void)sample_rate;
    (void)channels;
    (void)bits_per_sample;

    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* APP_CONFIG_BT_AUDIO_ENABLE */
