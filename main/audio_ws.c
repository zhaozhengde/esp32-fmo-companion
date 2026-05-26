/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file audio_ws.c
 * @brief 音频、事件、站点 WebSocket 与网络音频播放实现。
 *
 * 本模块负责：
 * - Audio WebSocket 音频数据接收；
 * - Event WebSocket 事件接收与异步解析；
 * - Station WebSocket 站点管理与 QSO 请求；
 * - SNTP 时间同步；
 * - 网络 PCM 音频缓冲和播放；
 * - 本机 DAC / I2S 音频输出后端；
 * - QSO 数量完整扫描、增量更新和 NVS 缓存保存。
 *
 * 当前音频启动策略：
 * - Event WS 与 Station WS 在网络可用后持续连接；
 * - Audio WS 默认关闭，等待用户通过 UI 手动开启；
 * - Audio WS 开启后，播放任务在缓冲达到阈值后才打开功放。
 */

#include "audio_ws.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* FreeRTOS headers --------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* ESP-IDF headers ---------------------------------------------------------- */
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"

/* Project headers ---------------------------------------------------------- */
#include "app_config.h"
#include "app_settings.h"
#include "audio_output.h"
#include "board_config.h"
#include "event_parser.h"
#include "station_parser.h"
#include "ui_async.h"
#include "wifi_manager.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "audio_ws";

/* -------------------------------------------------------------------------- */
/* Private macros: QSO synchronization                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief QSO 最新记录周期检查间隔，单位 ms。
 *
 * 当前为 120 秒。
 */
#define QSO_COUNT_POLL_INTERVAL_MS       120000

/**
 * @brief 手动完整扫描每页请求数量。
 */
#define QSO_COUNT_FULL_PAGE_SIZE         20

/**
 * @brief 增量扫描每页请求数量。
 */
#define QSO_COUNT_DELTA_PAGE_SIZE        20

/**
 * @brief 最新记录检查请求数量。
 *
 * 周期检查时只请求最新 1 条，用于判断是否需要启动增量扫描。
 */
#define QSO_COUNT_CHECK_PAGE_SIZE        1

/**
 * @brief 手动完整扫描最大页数。
 *
 * 当前 pageSize=20，最多覆盖 10000 条记录。
 */
#define QSO_COUNT_MAX_FULL_SCAN_PAGES    500

/**
 * @brief 增量扫描最大页数。
 *
 * 当前 pageSize=20，最多自动查找 100 条新增记录。
 * 如果仍然找不到旧 latest log id，则提示用户手动同步。
 */
#define QSO_COUNT_MAX_DELTA_SCAN_PAGES   5

/* -------------------------------------------------------------------------- */
/* Private macros: local audio output                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief 本机 I2S 输出端口。
 */
#define AUDIO_I2S_PORT                   I2S_NUM_0

/**
 * @brief ESP32 内置 DAC 输出模式。
 *
 * 当前使用双通道输出相同音频内容。
 */
#define AUDIO_I2S_DAC_MODE               I2S_DAC_CHANNEL_BOTH_EN

/**
 * @brief 是否启用线性插值升采样。
 *
 * 0：重复采样
 * 1：线性插值
 */
#define AUDIO_UPSAMPLE_LINEAR            0

/**
 * @brief 说话结束后的尾音保留时间，单位 us。
 *
 * 用于避免切断最后几个音频包。
 */
#define AUDIO_TAIL_KEEP_US               600000

/* -------------------------------------------------------------------------- */
/* Private macros: event parsing                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Event WebSocket JSON 拼包缓存大小。
 *
 * esp_websocket_client 的 DATA 事件可能只提供完整 payload 的一部分，
 * 因此需要先在缓存中拼出完整 JSON，再投递给解析任务。
 */
#define EVENT_WS_JSON_BUF_SIZE           2048

/**
 * @brief Event JSON 解析队列长度。
 */
#define EVENT_PARSE_QUEUE_LEN            4

/**
 * @brief Event JSON 解析任务栈大小。
 */
#define EVENT_PARSE_TASK_STACK           6144

/**
 * @brief Event JSON 解析任务优先级。
 */
#define EVENT_PARSE_TASK_PRIO            3

/**
 * @brief Station WebSocket JSON 拼包缓存大小。
 */
#define STATION_WS_JSON_BUF_SIZE         4096

/* -------------------------------------------------------------------------- */
/* Compile-time checks and compatibility defaults                             */
/* -------------------------------------------------------------------------- */

#ifndef APP_ENABLE_STATION_CURRENT_AUTO_POLL
#define APP_ENABLE_STATION_CURRENT_AUTO_POLL 1
#endif

#ifndef APP_ENABLE_STATION_LIST_AUTO_POLL
#define APP_ENABLE_STATION_LIST_AUTO_POLL 0
#endif

#if (AUDIO_OUTPUT_SAMPLE_RATE != \
     (AUDIO_WS_INPUT_SAMPLE_RATE * AUDIO_UPSAMPLE_FACTOR))
#error "AUDIO_OUTPUT_SAMPLE_RATE must equal AUDIO_WS_INPUT_SAMPLE_RATE * AUDIO_UPSAMPLE_FACTOR"
#endif

#if AUDIO_UPSAMPLE_FACTOR < 1
#error "AUDIO_UPSAMPLE_FACTOR must be >= 1"
#endif

/* -------------------------------------------------------------------------- */
/* Private types                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief QSO 数量同步模式。
 */
typedef enum {
    QSO_COUNT_MODE_NONE = 0,      /*!< 当前没有扫描任务 */
    QSO_COUNT_MODE_FULL_SCAN,     /*!< 用户触发的完整扫描 */
    QSO_COUNT_MODE_CHECK_LATEST,  /*!< 周期检查最新记录 */
    QSO_COUNT_MODE_DELTA_SCAN,    /*!< 发现更新后的增量扫描 */
} qso_count_mode_t;

/**
 * @brief 异步保存 QSO 状态请求。
 *
 * NVS 保存放在独立任务中执行，避免阻塞 WebSocket 回调任务。
 */
typedef struct {
    uint32_t count;
    int32_t latest_log_id;
    bool valid;
} qso_save_req_t;

/**
 * @brief Event JSON 解析任务消息。
 */
typedef struct {
    size_t len;
    char json[];
} event_json_msg_t;

/* -------------------------------------------------------------------------- */
/* Private variables: module lifecycle                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief audio_ws 模块是否已经启动完成。
 */
static volatile bool s_audio_ws_started = false;

/**
 * @brief audio_ws 模块是否正在停止。
 */
static volatile bool s_audio_ws_stopping = false;

/**
 * @brief audio_ws 模块是否正在启动。
 */
static volatile bool s_audio_ws_starting = false;

/* -------------------------------------------------------------------------- */
/* Private variables: WebSocket clients                                       */
/* -------------------------------------------------------------------------- */

static esp_websocket_client_handle_t s_ws_audio = NULL;
static esp_websocket_client_handle_t s_ws_event = NULL;
static esp_websocket_client_handle_t s_ws_station = NULL;

/**
 * @brief 用户是否开启 Audio WebSocket。
 *
 * false：
 * - 默认静音；
 * - 不连接 /audio。
 *
 * true：
 * - 用户主动开启音频；
 * - 连接 /audio。
 */
static volatile bool s_audio_ws_user_enabled = false;

/**
 * @brief Audio WebSocket 当前是否已连接。
 */
static volatile bool s_audio_ws_connected = false;

/* -------------------------------------------------------------------------- */
/* Private variables: local audio playback                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 本机 I2S / DAC 是否已经初始化。
 */
static bool s_i2s_started = false;

/**
 * @brief 当前用户音量百分比。
 */
static uint8_t s_audio_volume = DEFAULT_AUDIO_VOLUME;

/**
 * @brief 本机功放当前是否已经开启。
 */
static bool s_amp_enabled = false;

/**
 * @brief 本机音频是否被外部输出层静音。
 *
 * 当蓝牙音频接管播放时，audio_output.c 会调用：
 *
 * @code
 * local_audio_mute(true);
 * @endcode
 */
static volatile bool s_local_audio_muted = false;

/**
 * @brief Event 控制的说话状态。
 *
 * 当前保留用于通联状态判断和 QSO 同步限制。
 */
static volatile bool s_qso_speaking = false;

/**
 * @brief 说话结束后的尾音有效截止时间，单位 us。
 */
static int64_t s_audio_active_until_us = 0;

/**
 * @brief 低电保护是否禁止本地音频播放。
 */
static volatile bool s_low_power_audio_disabled = false;

/**
 * @brief 音频门控状态保护锁。
 */
static portMUX_TYPE s_audio_gate_mux = portMUX_INITIALIZER_UNLOCKED;

/* -------------------------------------------------------------------------- */
/* Private variables: audio ring buffer                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief 网络输入 PCM ring buffer。
 *
 * 保存格式：
 * - 16-bit signed PCM；
 * - mono；
 * - AUDIO_WS_INPUT_SAMPLE_RATE。
 */
static int16_t *s_ring_buf = NULL;

static volatile size_t s_ring_write = 0;
static volatile size_t s_ring_read = 0;

static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 播放任务每次读取的 PCM 临时缓冲。
 */
static int16_t s_play_pcm[PLAY_CHUNK_SAMPLES];

/**
 * @brief 转换后的 DAC 输出缓冲。
 *
 * 每个输入 sample：
 * - 经过 AUDIO_UPSAMPLE_FACTOR 倍升采样；
 * - 输出为双通道 DAC word。
 */
static uint16_t s_play_dac_buf[
    PLAY_CHUNK_SAMPLES * AUDIO_UPSAMPLE_FACTOR * 2
];

/* -------------------------------------------------------------------------- */
/* Private variables: audio statistics and adaptive buffer                    */
/* -------------------------------------------------------------------------- */

static uint32_t s_underflow_total = 0;
static uint32_t s_dynamic_drop_total = 0;
static uint32_t s_overflow_drop_total = 0;

static size_t s_target_buffer_samples = TARGET_BUFFER_DEFAULT;
static uint32_t s_underflow_in_window = 0;
static uint32_t s_good_seconds = 0;

static uint32_t s_audio_rx_bytes = 0;
static int64_t s_audio_rx_last_log_us = 0;

static uint32_t s_latency_log_count = 0;

/* -------------------------------------------------------------------------- */
/* Private variables: background tasks                                        */
/* -------------------------------------------------------------------------- */

static TaskHandle_t s_audio_play_task_handle = NULL;

#if APP_ENABLE_STATION_CURRENT_AUTO_POLL
static TaskHandle_t s_station_current_poll_task_handle = NULL;
#endif

#if APP_ENABLE_STATION_LIST_AUTO_POLL
static TaskHandle_t s_station_list_poll_task_handle = NULL;
#endif

static TaskHandle_t s_qso_count_poll_task_handle = NULL;

/* -------------------------------------------------------------------------- */
/* Private variables: Event / Station JSON parsing                            */
/* -------------------------------------------------------------------------- */

static char s_event_json_buf[EVENT_WS_JSON_BUF_SIZE];
static size_t s_event_json_len = 0;

static QueueHandle_t s_event_parse_queue = NULL;
static TaskHandle_t s_event_parse_task_handle = NULL;

static char s_station_json_buf[STATION_WS_JSON_BUF_SIZE];
static size_t s_station_json_len = 0;

/* -------------------------------------------------------------------------- */
/* Private variables: QSO synchronization                                     */
/* -------------------------------------------------------------------------- */

static portMUX_TYPE s_qso_count_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_qso_count_scan_active = false;
static qso_count_mode_t s_qso_count_mode = QSO_COUNT_MODE_NONE;

static int s_qso_scan_page = 0;
static int s_qso_scan_total = 0;

/**
 * @brief 已保存到 NVS 的 QSO 状态内存副本。
 */
static uint32_t s_qso_saved_count = 0;
static int32_t s_qso_saved_latest_log_id = -1;
static bool s_qso_saved_valid = false;

/**
 * @brief 增量扫描过程中的临时状态。
 */
static int s_qso_delta_count = 0;
static int32_t s_qso_new_latest_log_id = -1;

/* -------------------------------------------------------------------------- */
/* Private function declarations: amplifier and audio gate                    */
/* -------------------------------------------------------------------------- */

static void audio_amp_init(void);
static void audio_amp_enable(bool enable);
static bool audio_gate_is_active(void);

/* -------------------------------------------------------------------------- */
/* Private function declarations: SNTP                                        */
/* -------------------------------------------------------------------------- */

static void sntp_time_sync_notification_cb(struct timeval *tv);
static void sntp_start_once(void);

/* -------------------------------------------------------------------------- */
/* Private function declarations: audio ring buffer                           */
/* -------------------------------------------------------------------------- */

static esp_err_t ring_init(void);
static size_t ring_available_samples(void);
static size_t ring_free_samples(void);
static void ring_clear(void);
static void ring_drop_old_samples(size_t samples);
static void ring_write_samples(const int16_t *data, size_t samples);
static size_t ring_read_samples(int16_t *out, size_t samples);

static void audio_stream_reset_state(bool clear_ring);
static void adaptive_buffer_update(size_t current_buf);
static uint8_t pcm_calculate_level_percent(const int16_t *pcm,
                                           size_t samples);

/* -------------------------------------------------------------------------- */
/* Private function declarations: local DAC / I2S output                      */
/* -------------------------------------------------------------------------- */

static esp_err_t audio_i2s_dac_init(void);
static inline uint16_t pcm16_sample_to_dac_word(int32_t v);
static size_t pcm16_to_i2s_dac(const int16_t *pcm,
                               uint16_t *out,
                               size_t samples);

/* -------------------------------------------------------------------------- */
/* Private function declarations: station requests and polling                */
/* -------------------------------------------------------------------------- */

static esp_err_t station_send_text_request(const char *req);
static void station_send_get_current(void);

#if APP_ENABLE_STATION_LIST_AUTO_POLL
static void station_send_get_list_range(int start, int count);
#endif

#if APP_ENABLE_STATION_CURRENT_AUTO_POLL
static void station_current_poll_task(void *arg);
#endif

#if APP_ENABLE_STATION_LIST_AUTO_POLL
static void station_list_poll_task(void *arg);
#endif

/* -------------------------------------------------------------------------- */
/* Private function declarations: Event parsing                               */
/* -------------------------------------------------------------------------- */

static void event_parse_task(void *arg);
static esp_err_t event_parse_task_start(void);
static void event_post_json_to_parse_task(const char *json, size_t len);

/* -------------------------------------------------------------------------- */
/* Private function declarations: WebSocket clients                           */
/* -------------------------------------------------------------------------- */

static void websocket_audio_event_handler(void *handler_args,
                                          esp_event_base_t base,
                                          int32_t event_id,
                                          void *event_data);
static esp_err_t websocket_audio_start(void);
static void websocket_audio_stop(void);

static void websocket_event_event_handler(void *handler_args,
                                          esp_event_base_t base,
                                          int32_t event_id,
                                          void *event_data);
static esp_err_t websocket_event_start(void);

static void websocket_station_event_handler(void *handler_args,
                                            esp_event_base_t base,
                                            int32_t event_id,
                                            void *event_data);
static esp_err_t websocket_station_start(void);

/* -------------------------------------------------------------------------- */
/* Private function declarations: playback                                    */
/* -------------------------------------------------------------------------- */

static void audio_rx_rate_stat_add(size_t bytes);
static void net_audio_play_task(void *arg);

/* -------------------------------------------------------------------------- */
/* Private function declarations: QSO synchronization                         */
/* -------------------------------------------------------------------------- */

static void qso_count_poll_task(void *arg);
static void qso_save_task(void *arg);

static void audio_ws_qso_count_load_cache(void);
static void audio_ws_qso_count_check_latest(void);
static void audio_ws_qso_count_start_delta_scan(int32_t new_latest_log_id);

static esp_err_t audio_ws_qso_count_save_state(uint32_t count,
                                               int32_t latest_log_id,
                                               bool valid);

/* -------------------------------------------------------------------------- */
/* Amplifier control                                                          */
/* -------------------------------------------------------------------------- */

static void audio_amp_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOARD_AUDIO_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config audio amp failed: %s", esp_err_to_name(ret));
        return;
    }

    /*
     * 默认关闭功放。
     */
    gpio_set_level(BOARD_AUDIO_EN_GPIO, BOARD_AUDIO_MUTE_ACTIVE);

    s_amp_enabled = false;
}

static void audio_amp_enable(bool enable)
{
    /*
     * 本机静音、低电禁用、省电停止期间，不允许打开功放。
     */
    if (enable) {
        if (s_local_audio_muted ||
            s_low_power_audio_disabled ||
            s_audio_ws_stopping) {
            enable = false;
        }
    }

    if (s_amp_enabled == enable) {
        return;
    }

    s_amp_enabled = enable;

    gpio_set_level(BOARD_AUDIO_EN_GPIO,
                   enable ? BOARD_AUDIO_EN_ACTIVE :
                            BOARD_AUDIO_MUTE_ACTIVE);

    /*
     * 不在这里打印日志。
     * 该函数可能被播放任务高频调用。
     */
}

static bool audio_gate_is_active(void)
{
    /*
     * 低电保护优先级最高。
     */
    if (s_low_power_audio_disabled) {
        return false;
    }

#if APP_ENABLE_WS_EVENT
    bool speaking;
    int64_t active_until;

    portENTER_CRITICAL(&s_audio_gate_mux);
    speaking = s_qso_speaking;
    active_until = s_audio_active_until_us;
    portEXIT_CRITICAL(&s_audio_gate_mux);

    if (speaking) {
        return true;
    }

    int64_t now = esp_timer_get_time();

    return now < active_until;
#else
    return true;
#endif
}

/* -------------------------------------------------------------------------- */
/* SNTP                                                                       */
/* -------------------------------------------------------------------------- */

static void sntp_time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;

    ESP_LOGI(TAG, "SNTP time synchronized");

    /*
     * 中国标准时间：UTC+8。
     * POSIX TZ 中 CST-8 表示 UTC+8。
     */
    setenv("TZ", "CST-8", 1);
    tzset();

    ui_async_update_status("已校时");
}

static void sntp_start_once(void)
{
    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "SNTP already started");
        return;
    }

    ESP_LOGI(TAG, "Starting SNTP...");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "ntp.tencent.com");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntp_time_sync_notification_cb);
    esp_sntp_init();
}

/* -------------------------------------------------------------------------- */
/* Audio ring buffer                                                          */
/* -------------------------------------------------------------------------- */

static esp_err_t ring_init(void)
{
    if (s_ring_buf) {
        return ESP_OK;
    }

    size_t bytes = AUDIO_RING_SAMPLES * sizeof(int16_t);

    s_ring_buf = heap_caps_calloc(AUDIO_RING_SAMPLES,
                                  sizeof(int16_t),
                                  MALLOC_CAP_8BIT);

    if (!s_ring_buf) {
        ESP_LOGE(TAG,
                 "ring buffer alloc failed, size=%u bytes",
                 (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }

    s_ring_write = 0;
    s_ring_read = 0;

    ESP_LOGI(TAG,
             "ring buffer allocated, samples=%d, bytes=%u",
             AUDIO_RING_SAMPLES,
             (unsigned)bytes);

    return ESP_OK;
}

static size_t ring_available_samples(void)
{
    if (!s_ring_buf) {
        return 0;
    }

    size_t write_idx;
    size_t read_idx;

    portENTER_CRITICAL(&s_ring_mux);
    write_idx = s_ring_write;
    read_idx = s_ring_read;
    portEXIT_CRITICAL(&s_ring_mux);

    if (write_idx >= read_idx) {
        return write_idx - read_idx;
    }

    return AUDIO_RING_SAMPLES - read_idx + write_idx;
}

static size_t ring_free_samples(void)
{
    size_t avail = ring_available_samples();

    if (avail >= AUDIO_RING_SAMPLES - 1) {
        return 0;
    }

    return (AUDIO_RING_SAMPLES - 1) - avail;
}

static void ring_clear(void)
{
    if (!s_ring_buf) {
        return;
    }

    portENTER_CRITICAL(&s_ring_mux);

    s_ring_write = 0;
    s_ring_read = 0;

    portEXIT_CRITICAL(&s_ring_mux);
}

static void audio_stream_reset_state(bool clear_ring)
{
    if (clear_ring) {
        ring_clear();
    }

    s_target_buffer_samples = TARGET_BUFFER_DEFAULT;
    s_underflow_in_window = 0;
    s_good_seconds = 0;

    s_audio_rx_bytes = 0;
    s_audio_rx_last_log_us = 0;
}

static void ring_drop_old_samples(size_t samples)
{
    if (!s_ring_buf || samples == 0) {
        return;
    }

    portENTER_CRITICAL(&s_ring_mux);

    size_t avail;

    if (s_ring_write >= s_ring_read) {
        avail = s_ring_write - s_ring_read;
    } else {
        avail = AUDIO_RING_SAMPLES - s_ring_read + s_ring_write;
    }

    if (samples > avail) {
        samples = avail;
    }

    s_ring_read = (s_ring_read + samples) % AUDIO_RING_SAMPLES;

    portEXIT_CRITICAL(&s_ring_mux);
}

static void ring_write_samples(const int16_t *data, size_t samples)
{
    if (!s_ring_buf || !data || samples == 0) {
        return;
    }

    size_t free_samples = ring_free_samples();

    if (samples > free_samples) {
        size_t need_drop = samples - free_samples;

        ring_drop_old_samples(need_drop);
        s_overflow_drop_total += need_drop;
    }

    portENTER_CRITICAL(&s_ring_mux);

    for (size_t i = 0; i < samples; i++) {
        s_ring_buf[s_ring_write] = data[i];
        s_ring_write = (s_ring_write + 1) % AUDIO_RING_SAMPLES;
    }

    portEXIT_CRITICAL(&s_ring_mux);
}

static size_t ring_read_samples(int16_t *out, size_t samples)
{
    if (!s_ring_buf || !out || samples == 0) {
        return 0;
    }

    size_t count = 0;

    portENTER_CRITICAL(&s_ring_mux);

    while (count < samples && s_ring_read != s_ring_write) {
        out[count++] = s_ring_buf[s_ring_read];
        s_ring_read = (s_ring_read + 1) % AUDIO_RING_SAMPLES;
    }

    portEXIT_CRITICAL(&s_ring_mux);

    return count;
}

/* -------------------------------------------------------------------------- */
/* Adaptive buffering                                                         */
/* -------------------------------------------------------------------------- */

static void adaptive_buffer_update(size_t current_buf)
{
    /*
     * 防止异常情况下 target 超过最大值。
     */
    if (s_target_buffer_samples > TARGET_BUFFER_MAX) {
        s_target_buffer_samples = TARGET_BUFFER_MAX;
    }

    if (s_underflow_in_window > 0) {
        if (s_target_buffer_samples < TARGET_BUFFER_MAX) {
            s_target_buffer_samples += TARGET_BUFFER_INC_STEP;

            if (s_target_buffer_samples > TARGET_BUFFER_MAX) {
                s_target_buffer_samples = TARGET_BUFFER_MAX;
            }

            ESP_LOGW(TAG,
                     "adaptive: underflow, target buffer -> %d",
                     (int)s_target_buffer_samples);
        }

        s_good_seconds = 0;
    } else {
        if (current_buf > s_target_buffer_samples / 2) {
            s_good_seconds++;
        } else {
            s_good_seconds = 0;
        }

        if (s_good_seconds >= 5) {
            if (s_target_buffer_samples >
                TARGET_BUFFER_MIN + TARGET_BUFFER_DEC_STEP) {

                s_target_buffer_samples -= TARGET_BUFFER_DEC_STEP;

                if (s_target_buffer_samples < TARGET_BUFFER_MIN) {
                    s_target_buffer_samples = TARGET_BUFFER_MIN;
                }

                ESP_LOGI(TAG,
                         "adaptive: stable, target buffer -> %d",
                         (int)s_target_buffer_samples);
            }

            s_good_seconds = 0;
        }
    }

    s_underflow_in_window = 0;
}

/* -------------------------------------------------------------------------- */
/* PCM level                                                                  */
/* -------------------------------------------------------------------------- */

static uint8_t pcm_calculate_level_percent(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) {
        return 0;
    }

    int32_t peak = 0;

    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i];

        if (v < 0) {
            v = -v;
        }

        if (v > peak) {
            peak = v;
        }
    }

    /*
     * 以 12000 作为语音峰值参考，超过则显示 100%。
     */
    uint32_t level = (uint32_t)(peak * 100 / 12000);

    if (level > 100) {
        level = 100;
    }

    return (uint8_t)level;
}

/* -------------------------------------------------------------------------- */
/* I2S DAC                                                                    */
/* -------------------------------------------------------------------------- */

static esp_err_t audio_i2s_dac_init(void)
{
    if (s_i2s_started) {
        return ESP_OK;
    }

    audio_amp_init();

    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN,
        .sample_rate = AUDIO_OUTPUT_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = PLAY_CHUNK_SAMPLES * AUDIO_UPSAMPLE_FACTOR * 2,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    esp_err_t ret = i2s_driver_install(AUDIO_I2S_PORT,
                                       &i2s_config,
                                       0,
                                       NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "i2s_driver_install failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_set_dac_mode(AUDIO_I2S_DAC_MODE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "i2s_set_dac_mode failed: %s",
                 esp_err_to_name(ret));

        i2s_driver_uninstall(AUDIO_I2S_PORT);
        return ret;
    }

    i2s_zero_dma_buffer(AUDIO_I2S_PORT);

    s_i2s_started = true;

    ESP_LOGI(TAG,
             "I2S built-in DAC initialized, input_rate=%d, output_rate=%d, upsample=%d, gpio=%d, dac_mode=%d",
             AUDIO_WS_INPUT_SAMPLE_RATE,
             AUDIO_OUTPUT_SAMPLE_RATE,
             AUDIO_UPSAMPLE_FACTOR,
             BOARD_AUDIO_DAC_GPIO,
             AUDIO_I2S_DAC_MODE);

    return ESP_OK;
}

static inline uint16_t pcm16_sample_to_dac_word(int32_t v)
{
    /*
     * 应用用户音量。
     */
    v = v * s_audio_volume / 100;

    /*
     * 限幅，避免 int16 溢出后转换异常。
     */
    if (v > 32767) {
        v = 32767;
    } else if (v < -32768) {
        v = -32768;
    }

    /*
     * 16-bit signed PCM -> ESP32 built-in DAC 8-bit:
     *
     * -32768..32767 -> 0..255
     *
     * I2S DAC 模式需要把 8-bit DAC 值放到高 8 位。
     */
    int32_t dac = (v >> 8) + 128;

    if (dac < 0) {
        dac = 0;
    } else if (dac > 255) {
        dac = 255;
    }

    return ((uint16_t)dac) << 8;
}

static size_t pcm16_to_i2s_dac(const int16_t *pcm,
                               uint16_t *out,
                               size_t samples)
{
    if (!pcm || !out || samples == 0) {
        return 0;
    }

    size_t out_index = 0;

#if AUDIO_UPSAMPLE_LINEAR

    /*
     * 线性插值升采样。
     *
     * 以 AUDIO_UPSAMPLE_FACTOR = 4 为例：
     *
     * 当前采样 x0，下一个采样 x1：
     * - j=0: x0
     * - j=1: x0*3/4 + x1*1/4
     * - j=2: x0*2/4 + x1*2/4
     * - j=3: x0*1/4 + x1*3/4
     *
     * 最后一个采样没有 x1，则使用自身，避免越界。
     */
    for (size_t i = 0; i < samples; i++) {
        int32_t x0 = pcm[i];
        int32_t x1 = (i + 1 < samples) ? pcm[i + 1] : pcm[i];

        for (int j = 0; j < AUDIO_UPSAMPLE_FACTOR; j++) {
            int32_t v;

            if (AUDIO_UPSAMPLE_FACTOR == 1) {
                v = x0;
            } else {
                v = (x0 * (AUDIO_UPSAMPLE_FACTOR - j) +
                     x1 * j) / AUDIO_UPSAMPLE_FACTOR;
            }

            uint16_t dac_word = pcm16_sample_to_dac_word(v);

            /*
             * 内置 DAC 双声道输出同样数据。
             */
            out[out_index++] = dac_word;
            out[out_index++] = dac_word;
        }
    }

#else

    /*
     * 重复采样升采样。
     *
     * 以 AUDIO_UPSAMPLE_FACTOR = 4 为例：
     * x0 x0 x0 x0
     */
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i];

        uint16_t dac_word = pcm16_sample_to_dac_word(v);

        for (int j = 0; j < AUDIO_UPSAMPLE_FACTOR; j++) {
            out[out_index++] = dac_word;
            out[out_index++] = dac_word;
        }
    }

#endif

    return out_index;
}

/* -------------------------------------------------------------------------- */
/* Local audio backend for audio_output.c                                     */
/* -------------------------------------------------------------------------- */

esp_err_t local_audio_write_pcm(const uint8_t *pcm_data,
                                int len,
                                int sample_rate,
                                int channels,
                                int bits_per_sample)
{
    if (!pcm_data || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 当前本机播放链路只处理：
     * - 16-bit PCM
     * - mono
     * - AUDIO_WS_INPUT_SAMPLE_RATE
     */
    if (bits_per_sample != 16) {
        ESP_LOGW(TAG, "local audio unsupported bits=%d", bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (channels != 1) {
        ESP_LOGW(TAG,
                 "local audio unsupported channels=%d",
                 channels);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (sample_rate != AUDIO_WS_INPUT_SAMPLE_RATE) {
        ESP_LOGW(TAG,
                 "local audio sample rate mismatch: %d, expect=%d",
                 sample_rate,
                 AUDIO_WS_INPUT_SAMPLE_RATE);

        /*
         * 当前仍继续播放。
         * 如果后续支持多采样率输入，应在这里增加重采样。
         */
    }

    int samples = len / (int)sizeof(int16_t);
    if (samples <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * s_play_dac_buf 大小：
     * PLAY_CHUNK_SAMPLES * AUDIO_UPSAMPLE_FACTOR * 2。
     *
     * 正常播放任务每次传入 PLAY_CHUNK_SAMPLES。
     * 这里限制 samples，避免异常调用越界。
     */
    if (samples > PLAY_CHUNK_SAMPLES) {
        samples = PLAY_CHUNK_SAMPLES;
    }

    const int16_t *pcm = (const int16_t *)pcm_data;

    size_t out_words = pcm16_to_i2s_dac(pcm,
                                        s_play_dac_buf,
                                        (size_t)samples);

    size_t bytes_written = 0;

    esp_err_t ret = i2s_write(AUDIO_I2S_PORT,
                              s_play_dac_buf,
                              out_words * sizeof(uint16_t),
                              &bytes_written,
                              pdMS_TO_TICKS(100));

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_write failed: %s", esp_err_to_name(ret));

        /*
         * 避免 I2S 偶发堵塞后播放任务长期异常。
         */
        if (s_i2s_started) {
            i2s_zero_dma_buffer(AUDIO_I2S_PORT);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
        return ret;
    }

    return ESP_OK;
}

void local_audio_mute(bool mute)
{
    s_local_audio_muted = mute;

    if (mute) {
        /*
         * 立即关闭本机功放，避免蓝牙播放时本机也出声。
         */
        audio_amp_enable(false);

        if (s_i2s_started) {
            i2s_zero_dma_buffer(AUDIO_I2S_PORT);
        }

        ESP_LOGI(TAG, "local audio muted");
    } else {
        /*
         * 不在这里立即打开功放。
         * 是否打开由播放任务根据音频缓冲状态决定。
         */
        ESP_LOGI(TAG, "local audio unmuted");
    }
}
/* -------------------------------------------------------------------------- */
/* Station requests                                                           */
/* -------------------------------------------------------------------------- */

static esp_err_t station_send_text_request(const char *req)
{
    if (!req || req[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_audio_ws_stopping) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_ws_station) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!esp_websocket_client_is_connected(s_ws_station)) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 堆内存太低时不要继续发送 station 请求。
     * 否则 WebSocket 内部写入可能失败，甚至触发更严重的内存问题。
     */
    uint32_t heap = esp_get_free_heap_size();

    if (heap < 6000) {
        ESP_LOGW(TAG,
                 "skip station request, low heap=%u",
                 (unsigned)heap);
        return ESP_ERR_NO_MEM;
    }

    int ret = esp_websocket_client_send_text(s_ws_station,
                                             req,
                                             strlen(req),
                                             pdMS_TO_TICKS(1000));

    return (ret >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_ws_station_get_pinned_list(int start, int count)
{
    if (start < 0) {
        start = 0;
    }

    if (count <= 0) {
        count = 1;
    }

    char req[192];

    /*
     * station_build_get_pinned_list_range() 返回 esp_err_t。
     * ESP_OK == 0 表示成功。
     */
    esp_err_t ret = station_build_get_pinned_list_range(req,
                                                        sizeof(req),
                                                        start,
                                                        count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "build station getPinnedList failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "station getPinnedList request: %s", req);

    return station_send_text_request(req);
}

static void station_send_get_current(void)
{
    char req[128];

    esp_err_t ret = station_build_get_current(req, sizeof(req));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "build getCurrent failed: %s",
                 esp_err_to_name(ret));
        return;
    }

    ret = station_send_text_request(req);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "send getCurrent failed: %s",
                 esp_err_to_name(ret));
    }
}

#if APP_ENABLE_STATION_LIST_AUTO_POLL

static void station_send_get_list_range(int start, int count)
{
    char req[160];

    esp_err_t ret = station_build_get_list_range(req,
                                                 sizeof(req),
                                                 start,
                                                 count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "build getListRange failed: %s",
                 esp_err_to_name(ret));
        return;
    }

    ret = station_send_text_request(req);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "send getListRange failed: %s",
                 esp_err_to_name(ret));
    }
}

#endif /* APP_ENABLE_STATION_LIST_AUTO_POLL */

/* -------------------------------------------------------------------------- */
/* Station polling tasks                                                      */
/* -------------------------------------------------------------------------- */

#if APP_ENABLE_STATION_CURRENT_AUTO_POLL

static void station_current_poll_task(void *arg)
{
    (void)arg;

    const app_settings_t *cfg = app_settings_get();

    uint32_t interval =
        cfg ? cfg->ws_station_current_refresh_ms :
              DEFAULT_WS_STATION_CURRENT_REFRESH_MS;

    /*
     * 当前站点刷新不要太频繁。
     */
    if (interval < 10000) {
        interval = 10000;
    }

    ESP_LOGI(TAG,
             "station_current_poll_task started, interval=%lu ms",
             (unsigned long)interval);

    uint32_t log_tick = 0;

    while (true) {
        if (!s_audio_ws_stopping &&
            s_audio_ws_started &&
            audio_ws_station_is_connected()) {

            esp_err_t ret = audio_ws_station_get_current();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "auto getCurrent failed: %s",
                         esp_err_to_name(ret));
            }
        }

        /*
         * 偶尔打印栈水位，方便确认不会溢出。
         */
        log_tick++;
        if (log_tick >= 6) {
            log_tick = 0;

            UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);

            ESP_LOGI(TAG,
                     "station_cur stack watermark=%u words, heap=%u",
                     (unsigned)watermark,
                     (unsigned)esp_get_free_heap_size());
        }

        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

#endif /* APP_ENABLE_STATION_CURRENT_AUTO_POLL */

#if APP_ENABLE_STATION_LIST_AUTO_POLL

static void station_list_poll_task(void *arg)
{
    (void)arg;

    const app_settings_t *cfg = app_settings_get();

    uint32_t interval =
        cfg ? cfg->ws_station_list_refresh_ms :
              DEFAULT_WS_STATION_LIST_REFRESH_MS;

    if (interval < 60000) {
        interval = 60000;
    }

    ESP_LOGI(TAG,
             "station_list_poll_task started, interval=%lu ms",
             (unsigned long)interval);

    while (true) {
        if (audio_ws_station_is_connected()) {
            station_send_get_list_range(0, 8);
        }

        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

#endif /* APP_ENABLE_STATION_LIST_AUTO_POLL */

/* -------------------------------------------------------------------------- */
/* Event parse task                                                           */
/* -------------------------------------------------------------------------- */

static void event_parse_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "event_parse_task started");

    while (true) {
        event_json_msg_t *msg = NULL;

        if (xQueueReceive(s_event_parse_queue,
                          &msg,
                          portMAX_DELAY) == pdTRUE) {
            if (msg) {
                event_parser_handle_json(msg->json, (int)msg->len);
                free(msg);
            }
        }
    }
}

static esp_err_t event_parse_task_start(void)
{
    if (!s_event_parse_queue) {
        s_event_parse_queue = xQueueCreate(EVENT_PARSE_QUEUE_LEN,
                                           sizeof(event_json_msg_t *));
        if (!s_event_parse_queue) {
            ESP_LOGE(TAG, "create event parse queue failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_event_parse_task_handle) {
        BaseType_t ret = xTaskCreate(event_parse_task,
                                     "event_parse",
                                     EVENT_PARSE_TASK_STACK,
                                     NULL,
                                     EVENT_PARSE_TASK_PRIO,
                                     &s_event_parse_task_handle);

        if (ret != pdPASS) {
            ESP_LOGE(TAG, "create event_parse_task failed");

            /*
             * 如果任务创建失败，删除刚创建的队列，
             * 避免留下半初始化状态。
             */
            if (s_event_parse_queue) {
                vQueueDelete(s_event_parse_queue);
                s_event_parse_queue = NULL;
            }

            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static void event_post_json_to_parse_task(const char *json, size_t len)
{
    if (!json || len == 0 || !s_event_parse_queue) {
        return;
    }

    /*
     * qso/history 类型的大消息不走 event_parse_task。
     * QSO 列表响应当前由 Station WS 分支识别后直接交给 event_parser。
     *
     * 这里过滤掉疑似 qso/history 的事件，避免占用 event 队列和解析任务栈。
     */
    if (strstr(json, "\"type\"") &&
        strstr(json, "\"qso\"") &&
        strstr(json, "\"subType\"") &&
        strstr(json, "\"history\"")) {
        return;
    }

    event_json_msg_t *msg =
        malloc(sizeof(event_json_msg_t) + len + 1);

    if (!msg) {
        ESP_LOGW(TAG,
                 "alloc event json msg failed, len=%u",
                 (unsigned)len);
        return;
    }

    msg->len = len;

    memcpy(msg->json, json, len);
    msg->json[len] = '\0';

    if (xQueueSend(s_event_parse_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event parse queue full, drop json");
        free(msg);
    }
}

/* -------------------------------------------------------------------------- */
/* WebSocket: audio gate                                                      */
/* -------------------------------------------------------------------------- */

void audio_ws_set_speaking(bool speaking)
{
#if APP_ENABLE_WS_EVENT
    bool was_speaking = false;

    /*
     * 如果已经停止或正在停止，忽略 speaking=true 的残留事件。
     *
     * 注意：
     * 该判断必须放在进入 critical 之前，
     * 避免提前 return 时忘记释放锁。
     */
    if (speaking) {
        if (s_audio_ws_stopping ||
            (!s_audio_ws_started && !s_audio_ws_starting)) {
            return;
        }
    }

    /*
     * critical 区域只修改门控状态。
     * 不在 critical 区域内执行 ring_clear()、功放控制或 UI 更新。
     */
    portENTER_CRITICAL(&s_audio_gate_mux);

    was_speaking = s_qso_speaking;
    s_qso_speaking = speaking;

    if (speaking) {
        /*
         * 进入 speaking 状态时，清掉尾音时间。
         */
        s_audio_active_until_us = 0;
    } else {
        /*
         * 说话结束后保留尾音窗口。
         */
        s_audio_active_until_us = esp_timer_get_time() + AUDIO_TAIL_KEEP_US;
    }

    portEXIT_CRITICAL(&s_audio_gate_mux);

    if (speaking) {
        /*
         * 从空闲进入说话状态时，清掉旧缓冲，避免播放旧音频。
         */
        if (!was_speaking) {
            audio_stream_reset_state(true);
        }

        /*
         * 不在这里打开功放。
         * speaking=true 只表示允许播放。
         * 真正打开功放交给播放任务，在音频缓冲足够后执行。
         */
    } else {
        /*
         * 不立即关闭功放。
         * 播放任务会根据当前播放状态和尾音窗口自行处理。
         */
    }
#else
    (void)speaking;
#endif
}

/* -------------------------------------------------------------------------- */
/* WebSocket: audio receive statistics                                        */
/* -------------------------------------------------------------------------- */

static void audio_rx_rate_stat_add(size_t bytes)
{
    s_audio_rx_bytes += bytes;

    int64_t now_us = esp_timer_get_time();

    if (s_audio_rx_last_log_us == 0) {
        s_audio_rx_last_log_us = now_us;
        return;
    }

    if ((now_us - s_audio_rx_last_log_us) >= 5000000) {
        uint32_t bytes_per_sec = s_audio_rx_bytes / 5;

        ESP_LOGI(TAG,
                 "audio rx rate: %u B/s, expect about 16000 B/s for 8k16mono",
                 (unsigned)bytes_per_sec);

        s_audio_rx_bytes = 0;
        s_audio_rx_last_log_us = now_us;
    }
}

/* -------------------------------------------------------------------------- */
/* WebSocket: audio                                                           */
/* -------------------------------------------------------------------------- */

static void websocket_audio_event_handler(void *handler_args,
                                          esp_event_base_t base,
                                          int32_t event_id,
                                          void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *data =
        (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Audio WebSocket connected");

        s_audio_ws_connected = true;

        /*
         * 只有用户明确开启音频时才提示。
         */
        if (s_audio_ws_user_enabled) {
            ui_async_update_status("音频已连");
        }

        /*
         * 连接成功后清空旧状态。
         */
        audio_stream_reset_state(true);

        /*
         * 不在 WebSocket connected 事件中打开功放。
         * 等播放任务检测到缓冲达到 START_BUFFER_SAMPLES 后再打开，
         * 避免连接瞬间听到底噪或空缓冲噪声。
         */
        audio_amp_enable(false);

        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Audio WebSocket disconnected");

        s_audio_ws_connected = false;

        audio_stream_reset_state(true);
        audio_amp_enable(false);
        ui_async_update_voice_level(0);

        /*
         * 如果是用户关闭音频，不提示“音频断开”，避免误导。
         */
        if (s_audio_ws_user_enabled) {
            ui_async_update_status("音频断开");
        } else {
            ui_async_update_status("已静音");
        }

        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "Audio WebSocket error");

        s_audio_ws_connected = false;

        audio_stream_reset_state(true);
        audio_amp_enable(false);
        ui_async_update_voice_level(0);

        if (s_audio_ws_user_enabled) {
            ui_async_update_status("音频错误");
        }

        break;

    case WEBSOCKET_EVENT_DATA:
        /*
         * 用户关闭音频后，理论上 Audio WS 已经 stop/destroy。
         * 这里再兜底，避免残留数据写入 ring。
         */
        if (!s_audio_ws_user_enabled) {
            ring_clear();
            break;
        }

        if (data &&
            data->op_code == 0x02 &&
            data->data_ptr &&
            data->data_len > 0) {

            size_t samples = data->data_len / sizeof(int16_t);
            if (samples == 0) {
                break;
            }

            audio_rx_rate_stat_add(data->data_len);

            /*
             * 当前播放逻辑：
             * 只要用户开启 Audio WS 且 Audio WS 已连接，
             * 就允许音频进入 ring buffer。
             */
            if (!s_audio_ws_user_enabled || !s_audio_ws_connected) {
                ring_clear();
                break;
            }

            size_t avail = ring_available_samples();
            size_t after_write = avail + samples;

            if (after_write > MAX_LATENCY_SAMPLES) {
                /*
                 * 延迟过高时，丢老数据到当前 target 附近。
                 */
                size_t target = s_target_buffer_samples;

                if (target < TARGET_BUFFER_MIN) {
                    target = TARGET_BUFFER_MIN;
                }

                if (target > TARGET_BUFFER_MAX) {
                    target = TARGET_BUFFER_MAX;
                }

                size_t drop = 0;

                if (after_write > target) {
                    drop = after_write - target;
                }

                if (drop > avail) {
                    drop = avail;
                }

                if (drop > 0) {
                    ring_drop_old_samples(drop);
                    s_dynamic_drop_total += drop;

                    s_latency_log_count++;
                    if ((s_latency_log_count % 20) == 0) {
                        ESP_LOGW(TAG,
                                 "latency control: avail=%d incoming=%d drop=%d target=%d",
                                 (int)avail,
                                 (int)samples,
                                 (int)drop,
                                 (int)target);
                    }
                }
            }

            ring_write_samples((const int16_t *)data->data_ptr, samples);
        }

        break;

    default:
        break;
    }
}

static esp_err_t websocket_audio_start(void)
{
    const app_settings_t *cfg = app_settings_get();

    if (!cfg || cfg->ws_audio_url[0] == '\0') {
        ESP_LOGW(TAG, "ws_audio_url empty");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 如果已经连接，直接返回。
     */
    if (s_ws_audio && esp_websocket_client_is_connected(s_ws_audio)) {
        ESP_LOGI(TAG, "Audio WebSocket already connected");
        return ESP_OK;
    }

    /*
     * 如果 client 存在但未连接，先销毁，避免半残留状态。
     */
    if (s_ws_audio) {
        ESP_LOGW(TAG,
                 "Audio WebSocket client exists but not connected, recreate");

        esp_websocket_client_stop(s_ws_audio);
        esp_websocket_client_destroy(s_ws_audio);
        s_ws_audio = NULL;
    }

    ESP_LOGI(TAG, "Audio WS URL: %s", cfg->ws_audio_url);

    s_audio_ws_connected = false;

    audio_stream_reset_state(true);
    audio_amp_enable(false);
    ui_async_update_voice_level(0);

    esp_websocket_client_config_t ws_cfg = {
        .uri = cfg->ws_audio_url,

        /*
         * Audio WS 数据帧频繁，适当增加栈。
         */
        .task_stack = 4096,
        .task_prio = 5,
        .reconnect_timeout_ms = 20000,
        .network_timeout_ms = 5000,
        .buffer_size = 768,
    };

    s_ws_audio = esp_websocket_client_init(&ws_cfg);
    if (!s_ws_audio) {
        ESP_LOGE(TAG, "create audio websocket failed");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_websocket_register_events(s_ws_audio,
                                                  WEBSOCKET_EVENT_ANY,
                                                  websocket_audio_event_handler,
                                                  NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "register audio websocket events failed: %s",
                 esp_err_to_name(ret));

        esp_websocket_client_destroy(s_ws_audio);
        s_ws_audio = NULL;
        return ret;
    }

    ret = esp_websocket_client_start(s_ws_audio);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "start audio websocket failed: %s",
                 esp_err_to_name(ret));

        esp_websocket_client_destroy(s_ws_audio);
        s_ws_audio = NULL;
        return ret;
    }

    ESP_LOGI(TAG,
             "Audio WebSocket started, heap=%u",
             (unsigned)esp_get_free_heap_size());

    return ESP_OK;
}

static void websocket_audio_stop(void)
{
    ESP_LOGI(TAG, "stopping Audio WebSocket");

    /*
     * 先认为已经断开，避免播放任务继续等数据。
     */
    s_audio_ws_connected = false;

    /*
     * 立即停止本地音频输出。
     */
    audio_stream_reset_state(true);
    ring_clear();
    audio_amp_enable(false);
    ui_async_update_voice_level(0);

    if (s_i2s_started) {
        i2s_zero_dma_buffer(AUDIO_I2S_PORT);
    }

    if (s_ws_audio) {
        if (esp_websocket_client_is_connected(s_ws_audio)) {
            esp_websocket_client_close(s_ws_audio,
                                       pdMS_TO_TICKS(1000));
        }

        esp_websocket_client_stop(s_ws_audio);
        esp_websocket_client_destroy(s_ws_audio);
        s_ws_audio = NULL;
    }

    ESP_LOGI(TAG, "Audio WebSocket stopped");
}

/* -------------------------------------------------------------------------- */
/* WebSocket: event                                                           */
/* -------------------------------------------------------------------------- */

static void websocket_event_event_handler(void *handler_args,
                                          esp_event_base_t base,
                                          int32_t event_id,
                                          void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *data =
        (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Event WebSocket connected");
        s_event_json_len = 0;
        ui_async_update_status("事件已连");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Event WebSocket disconnected");
        s_event_json_len = 0;
        ui_async_update_status("事件断开");
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "Event WebSocket error");
        ui_async_update_status("事件错误");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data &&
            data->op_code == 0x01 &&
            data->data_ptr &&
            data->data_len > 0) {

            int payload_offset = data->payload_offset;
            int payload_len = data->payload_len;

            if (payload_offset == 0) {
                s_event_json_len = 0;
            }

            if ((s_event_json_len + data->data_len) >=
                EVENT_WS_JSON_BUF_SIZE) {
                ESP_LOGW(TAG, "event json buffer overflow, drop frame");
                s_event_json_len = 0;
                break;
            }

            memcpy(s_event_json_buf + s_event_json_len,
                   data->data_ptr,
                   data->data_len);

            s_event_json_len += data->data_len;

            if ((payload_offset + data->data_len) >= payload_len) {
                s_event_json_buf[s_event_json_len] = '\0';

                event_post_json_to_parse_task(s_event_json_buf,
                                              s_event_json_len);
                s_event_json_len = 0;
            }
        }

        break;

    default:
        break;
    }
}

static esp_err_t websocket_event_start(void)
{
    const app_settings_t *cfg = app_settings_get();

    if (!cfg || cfg->ws_event_url[0] == '\0') {
        ESP_LOGW(TAG, "ws_event_url empty");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 如果已有 client，先销毁，避免重复创建。
     */
    if (s_ws_event) {
        esp_websocket_client_stop(s_ws_event);
        esp_websocket_client_destroy(s_ws_event);
        s_ws_event = NULL;
    }

    ESP_LOGI(TAG, "Event WS URL: %s", cfg->ws_event_url);

    esp_websocket_client_config_t ws_cfg = {
        .uri = cfg->ws_event_url,
        .task_stack = 3584,
        .task_prio = 4,
        .reconnect_timeout_ms = 20000,
        .network_timeout_ms = 5000,
        .buffer_size = 512,
    };

    s_ws_event = esp_websocket_client_init(&ws_cfg);
    if (!s_ws_event) {
        return ESP_FAIL;
    }

    esp_err_t ret = esp_websocket_register_events(s_ws_event,
                                                  WEBSOCKET_EVENT_ANY,
                                                  websocket_event_event_handler,
                                                  NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "register event websocket events failed: %s",
                 esp_err_to_name(ret));

        esp_websocket_client_destroy(s_ws_event);
        s_ws_event = NULL;
        return ret;
    }

    ret = esp_websocket_client_start(s_ws_event);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "start event websocket failed: %s",
                 esp_err_to_name(ret));

        esp_websocket_client_destroy(s_ws_event);
        s_ws_event = NULL;
        return ret;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* WebSocket: station                                                         */
/* -------------------------------------------------------------------------- */

static void websocket_station_event_handler(void *handler_args,
                                            esp_event_base_t base,
                                            int32_t event_id,
                                            void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *data =
        (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Station WebSocket connected");

        s_station_json_len = 0;
        ui_async_update_status("站点已连");

        /*
         * 连接后先获取一次当前站点。
         */
        station_send_get_current();

        /*
         * 开机不做完整 QSO 扫描。
         * 如果 NVS 有缓存，只检查 latestLogId。
         * 如果没有缓存，提示用户到设置页手动同步。
         */
        if (s_qso_saved_valid) {
            audio_ws_qso_count_check_latest();
        } else {
            ui_async_update_status("请同步QSO");
        }

        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Station WebSocket disconnected");
        s_station_json_len = 0;
        ui_async_update_status("站点断开");
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "Station WebSocket error");
        ui_async_update_status("站点错误");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data &&
            data->op_code == 0x01 &&
            data->data_ptr &&
            data->data_len > 0) {

            int payload_offset = data->payload_offset;
            int payload_len = data->payload_len;

            if (payload_offset == 0) {
                s_station_json_len = 0;
            }

            if ((s_station_json_len + data->data_len) >=
                STATION_WS_JSON_BUF_SIZE) {
                ESP_LOGW(TAG, "station json buffer overflow, drop frame");
                s_station_json_len = 0;
                break;
            }

            memcpy(s_station_json_buf + s_station_json_len,
                   data->data_ptr,
                   data->data_len);

            s_station_json_len += data->data_len;

            if ((payload_offset + data->data_len) >= payload_len) {
                s_station_json_buf[s_station_json_len] = '\0';

                /*
                 * Station WS 上也可能返回 QSO getListResponse。
                 * QSO 响应交给 event_parser 处理；
                 * 普通站点响应交给 station_parser 处理。
                 */
                if (strstr(s_station_json_buf, "\"type\":\"qso\"") ||
                    strstr(s_station_json_buf, "\"type\": \"qso\"")) {
                    ESP_LOGD(TAG,
                             "station ws qso getListResponse received");

                    event_parser_handle_json(s_station_json_buf,
                                             (int)s_station_json_len);
                } else {
                    station_parser_handle_json(s_station_json_buf,
                                               (int)s_station_json_len);
                }

                s_station_json_len = 0;
            }
        }

        break;

    default:
        break;
    }
}

static esp_err_t websocket_station_start(void)
{
    const app_settings_t *cfg = app_settings_get();

    if (!cfg || cfg->ws_station_url[0] == '\0') {
        ESP_LOGW(TAG, "ws_station_url empty");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 如果已有 client，先销毁，避免重复创建。
     */
    if (s_ws_station) {
        esp_websocket_client_stop(s_ws_station);
        esp_websocket_client_destroy(s_ws_station);
        s_ws_station = NULL;
    }

    ESP_LOGI(TAG, "Station WS URL: %s", cfg->ws_station_url);

    esp_websocket_client_config_t ws_cfg = {
        .uri = cfg->ws_station_url,
        .task_stack = 3584,
        .task_prio = 3,
        .reconnect_timeout_ms = 20000,
        .network_timeout_ms = 5000,
        .buffer_size = 768,
    };

    s_ws_station = esp_websocket_client_init(&ws_cfg);
    if (!s_ws_station) {
        return ESP_FAIL;
    }

    esp_err_t ret = esp_websocket_register_events(s_ws_station,
                                                  WEBSOCKET_EVENT_ANY,
                                                  websocket_station_event_handler,
                                                  NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "register station websocket events failed: %s",
                 esp_err_to_name(ret));

        esp_websocket_client_destroy(s_ws_station);
        s_ws_station = NULL;
        return ret;
    }

    ret = esp_websocket_client_start(s_ws_station);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "start station websocket failed: %s",
                 esp_err_to_name(ret));

        esp_websocket_client_destroy(s_ws_station);
        s_ws_station = NULL;
        return ret;
    }

    return ESP_OK;
}
/* -------------------------------------------------------------------------- */
/* Audio playback task                                                        */
/* -------------------------------------------------------------------------- */

static void net_audio_play_task(void *arg)
{
    (void)arg;

    bool started = false;
    uint32_t underflow_count = 0;
    uint32_t stat_tick = 0;
    uint32_t wait_log_cnt = 0;

    ESP_LOGI(TAG, "net_audio_play_task started");

    while (true) {
        /*
         * 当前播放条件：
         * - 用户开启 Audio WS；
         * - Audio WS 已连接；
         * - 未处于低电音频禁用；
         * - audio_ws 未处于停止中。
         *
         * 播放任务不再依赖 Event isSpeaking 直接门控播放。
         */
        if (!s_audio_ws_user_enabled ||
            !s_audio_ws_connected ||
            s_low_power_audio_disabled ||
            s_audio_ws_stopping) {

            if (started) {
                ESP_LOGI(TAG, "audio disabled/disconnected, stop playback");
            }

            started = false;
            underflow_count = 0;
            wait_log_cnt = 0;

            ring_clear();
            audio_amp_enable(false);
            ui_async_update_voice_level(0);

            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t avail = ring_available_samples();

        if (!started) {
            if (avail < START_BUFFER_SAMPLES) {
                wait_log_cnt++;

                if (wait_log_cnt >= 100) {
                    wait_log_cnt = 0;

                    ESP_LOGI(TAG,
                             "waiting audio buffer: avail=%d / start=%d",
                             (int)avail,
                             START_BUFFER_SAMPLES);
                }

                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            wait_log_cnt = 0;
            started = true;
            underflow_count = 0;

            /*
             * 用户开启音频并且缓冲达到启动阈值后，再打开功放。
             */
            audio_amp_enable(true);

            ESP_LOGI(TAG,
                     "audio playback started, buffered samples=%d",
                     (int)avail);
        }

        size_t got = ring_read_samples(s_play_pcm, PLAY_CHUNK_SAMPLES);

        if (got < PLAY_CHUNK_SAMPLES) {
            memset(&s_play_pcm[got],
                   0,
                   (PLAY_CHUNK_SAMPLES - got) * sizeof(int16_t));

            underflow_count++;
            s_underflow_total++;
            s_underflow_in_window++;

            /*
             * 不要每 16ms 都刷新 UI。
             */
            static uint8_t silent_ui_tick = 0;

            silent_ui_tick++;
            if (silent_ui_tick >= 12) {
                silent_ui_tick = 0;
                ui_async_update_voice_level(0);
            }
        } else {
            underflow_count = 0;

            uint8_t level = pcm_calculate_level_percent(s_play_pcm, got);

            /*
             * 不要每 16ms 都刷新 UI。
             * 每 9 个 chunk 约刷新一次。
             */
            static uint8_t level_ui_tick = 0;

            level_ui_tick++;
            if (level_ui_tick >= 9) {
                level_ui_tick = 0;
                ui_async_update_voice_level(level);
            }
        }

        /*
         * 统一音频输出入口。
         * AUTO 模式下可由 audio_output.c 自动选择本机或蓝牙输出。
         */
        esp_err_t ret = audio_output_write_pcm(
            (const uint8_t *)s_play_pcm,
            PLAY_CHUNK_SAMPLES * sizeof(int16_t),
            AUDIO_WS_INPUT_SAMPLE_RATE,
            1,
            16
        );

        if (ret != ESP_OK) {
            static uint32_t output_err_cnt = 0;

            output_err_cnt++;
            if ((output_err_cnt % 50) == 0) {
                ESP_LOGW(TAG,
                         "audio_output_write_pcm failed: %s",
                         esp_err_to_name(ret));
            }

            vTaskDelay(pdMS_TO_TICKS(5));
        }

        stat_tick++;
        if (stat_tick >= 300) {
            stat_tick = 0;

            size_t buf_now = ring_available_samples();

            adaptive_buffer_update(buf_now);

            ESP_LOGI(TAG,
                     "buf=%d smp, target=%d, underflow_total=%lu, underflow_recent=%lu, dyn_drop=%lu, overflow_drop=%lu, heap=%u",
                     (int)buf_now,
                     (int)s_target_buffer_samples,
                     (unsigned long)s_underflow_total,
                     (unsigned long)underflow_count,
                     (unsigned long)s_dynamic_drop_total,
                     (unsigned long)s_overflow_drop_total,
                     (unsigned)esp_get_free_heap_size());
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Public lifecycle interfaces                                                */
/* -------------------------------------------------------------------------- */

esp_err_t audio_ws_start(void)
{
    esp_err_t ret = ESP_OK;

    if (s_audio_ws_started || s_audio_ws_starting) {
        ESP_LOGW(TAG, "audio_ws already started/starting");
        return ESP_OK;
    }

    s_audio_ws_starting = true;
    s_audio_ws_stopping = false;
    s_low_power_audio_disabled = false;

    const app_settings_t *cfg = app_settings_get();

    if (cfg) {
        s_audio_volume = cfg->audio_volume;
    } else {
        s_audio_volume = DEFAULT_AUDIO_VOLUME;
    }

    if (s_audio_volume > 100) {
        s_audio_volume = DEFAULT_AUDIO_VOLUME;
    }

    ESP_LOGI(TAG, "audio_ws_start, volume=%u", s_audio_volume);

    audio_ws_qso_count_load_cache();

    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "WiFi is not connected");
        ui_async_update_status("WiFi未连接");
        ret = ESP_ERR_INVALID_STATE;
        goto fail;
    }

    ui_async_update_status("初始化音频");

    ESP_LOGI(TAG,
             "free heap before ring_init: %u",
             (unsigned)esp_get_free_heap_size());

    ret = ring_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ring_init failed: %s", esp_err_to_name(ret));
        ui_async_update_status("缓冲失败");
        goto fail;
    }

    ESP_LOGI(TAG,
             "free heap after ring_init: %u",
             (unsigned)esp_get_free_heap_size());

    ret = audio_i2s_dac_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "audio_i2s_dac_init failed: %s",
                 esp_err_to_name(ret));
        ui_async_update_status("DAC失败");
        goto fail;
    }

    ESP_LOGI(TAG,
             "free heap after i2s init: %u",
             (unsigned)esp_get_free_heap_size());

    ring_clear();

    /*
     * 播放任务需要常驻。
     * Audio WS 默认不连接，但播放任务会在用户开启音频且 WS 已连接后播放。
     */
    ESP_LOGI(TAG,
             "free heap before audio task: %u",
             (unsigned)esp_get_free_heap_size());

    if (s_audio_play_task_handle == NULL) {
        BaseType_t task_ret = xTaskCreate(net_audio_play_task,
                                          "net_audio_play",
                                          4096,
                                          NULL,
                                          5,
                                          &s_audio_play_task_handle);

        if (task_ret != pdPASS) {
            ESP_LOGE(TAG,
                     "create net_audio_play_task failed, heap=%u",
                     (unsigned)esp_get_free_heap_size());

            ui_async_update_status("音频任务失败");
            ret = ESP_ERR_NO_MEM;
            goto fail;
        }

        ESP_LOGI(TAG,
                 "net_audio_play_task created, free heap=%u",
                 (unsigned)esp_get_free_heap_size());
    }

#if APP_ENABLE_WS_AUDIO
    /*
     * 开机默认静音：
     * 不自动连接 /audio。
     *
     * 用户点击静音键开启音频时，再调用 audio_ws_audio_enable()
     * 创建并连接 Audio WS。
     */
    s_audio_ws_user_enabled = false;
    s_audio_ws_connected = false;

    audio_stream_reset_state(true);
    audio_amp_enable(false);
    ui_async_update_voice_level(0);

    ESP_LOGI(TAG, "Audio WebSocket disabled by default, skip start");
    ui_async_update_status("已静音");
#endif

#if APP_ENABLE_SNTP
    ui_async_update_status("SNTP校时");
    sntp_start_once();
#endif

#if APP_ENABLE_WS_EVENT
    /*
     * Event WS 持续连接，不依赖音频是否正在播放。
     * JSON 解析放到 event_parse_task，避免 websocket_task 栈溢出。
     */
    ui_async_update_status("连接事件");

    ret = event_parse_task_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "event_parse_task_start failed, keep going: %s",
                 esp_err_to_name(ret));
        ui_async_update_status("事件解析失败");
    } else {
        ret = websocket_event_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "websocket_event_start failed, keep audio running: %s",
                     esp_err_to_name(ret));
            ui_async_update_status("事件WS失败");
        } else {
            ESP_LOGI(TAG,
                     "free heap after event ws: %u",
                     (unsigned)esp_get_free_heap_size());
        }
    }

    vTaskDelay(pdMS_TO_TICKS(300));
#endif

#if APP_ENABLE_WS_STATION
    /*
     * Station WS 用于：
     * - 获取当前站点；
     * - 设置页手动获取站点列表；
     * - 切换当前站点；
     * - QSO getList 请求。
     */
    ui_async_update_status("连接FMO");

    ret = websocket_station_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "websocket_station_start failed: %s",
                 esp_err_to_name(ret));
        ui_async_update_status("FMO连接失败");
    } else {
        ESP_LOGI(TAG,
                 "free heap after station ws: %u",
                 (unsigned)esp_get_free_heap_size());
    }

    vTaskDelay(pdMS_TO_TICKS(500));
#endif

#if APP_ENABLE_WS_STATION

#if APP_ENABLE_STATION_CURRENT_AUTO_POLL
    /*
     * 只保留当前站点自动刷新任务。
     * 不再自动拉取站点列表。
     */
    if (s_station_current_poll_task_handle == NULL) {
        uint32_t heap = esp_get_free_heap_size();

        if (heap < 12000) {
            ESP_LOGW(TAG,
                     "skip station_current_poll_task, low heap=%u",
                     (unsigned)heap);
        } else {
            BaseType_t ret_task = xTaskCreate(
                station_current_poll_task,
                "station_cur",
                2048,
                NULL,
                3,
                &s_station_current_poll_task_handle
            );

            if (ret_task != pdPASS) {
                ESP_LOGE(TAG,
                         "create station_current_poll_task failed, heap=%u",
                         (unsigned)esp_get_free_heap_size());
                s_station_current_poll_task_handle = NULL;
            } else {
                ESP_LOGI(TAG,
                         "station_current_poll_task created, heap=%u",
                         (unsigned)esp_get_free_heap_size());
            }
        }
    }
#else
    ESP_LOGW(TAG, "station current auto poll disabled");
#endif

#if APP_ENABLE_STATION_LIST_AUTO_POLL
    /*
     * 默认关闭。
     * 站点列表改为设置页手动请求。
     */
    if (s_station_list_poll_task_handle == NULL) {
        BaseType_t ret_task = xTaskCreate(
            station_list_poll_task,
            "station_list",
            3072,
            NULL,
            3,
            &s_station_list_poll_task_handle
        );

        if (ret_task != pdPASS) {
            ESP_LOGE(TAG,
                     "create station_list_poll_task failed, heap=%u",
                     (unsigned)esp_get_free_heap_size());
            s_station_list_poll_task_handle = NULL;
        }
    }
#else
    ESP_LOGW(TAG, "station list auto poll disabled, use manual station list");
#endif

#endif /* APP_ENABLE_WS_STATION */

    ui_async_update_status("FMO已连接");

    if (!s_qso_count_poll_task_handle) {
        BaseType_t qso_task_ret = xTaskCreate(qso_count_poll_task,
                                              "qso_count",
                                              3072,
                                              NULL,
                                              3,
                                              &s_qso_count_poll_task_handle);

        if (qso_task_ret != pdPASS) {
            ESP_LOGW(TAG, "create qso_count_poll_task failed");
        } else {
            ESP_LOGI(TAG,
                     "qso_count_poll_task created, heap=%u",
                     (unsigned)esp_get_free_heap_size());
        }
    }

    ESP_LOGI(TAG, "Audio/Event/Station WebSocket started");

    s_audio_ws_started = true;
    s_audio_ws_starting = false;
    s_audio_ws_stopping = false;

    return ESP_OK;

fail:
    ESP_LOGW(TAG, "audio_ws_start failed, cleanup");

    s_audio_ws_starting = false;
    s_audio_ws_started = false;

    audio_ws_stop();

    return ret;
}

esp_err_t audio_ws_stop(void)
{
    ESP_LOGW(TAG, "audio_ws_stop");

    s_audio_ws_stopping = true;
    s_audio_ws_starting = false;
    s_audio_ws_started = false;
    s_audio_ws_connected = false;

    /*
     * 省电/停止期间禁止本地音频门控重新打开。
     * audio_ws_start() 会恢复为 false。
     */
    s_low_power_audio_disabled = true;

    /*
     * 先关闭播放门控和功放。
     */
    audio_ws_set_speaking(false);
    ring_clear();
    audio_amp_enable(false);
    ui_async_update_voice_level(0);

    /*
     * 停止并销毁 Audio WS。
     */
    s_audio_ws_user_enabled = false;
    websocket_audio_stop();

    /*
     * 停止并销毁 Event WS。
     */
    if (s_ws_event) {
        esp_websocket_client_stop(s_ws_event);
        esp_websocket_client_destroy(s_ws_event);
        s_ws_event = NULL;
    }

    /*
     * 停止并销毁 Station WS。
     */
    if (s_ws_station) {
        esp_websocket_client_stop(s_ws_station);
        esp_websocket_client_destroy(s_ws_station);
        s_ws_station = NULL;
    }

    /*
     * 清空 Event parse 队列里的残留消息。
     * 避免进入省电后旧 callsign 事件继续触发 UI/音频状态变化。
     */
    if (s_event_parse_queue) {
        event_json_msg_t *msg = NULL;

        while (xQueueReceive(s_event_parse_queue, &msg, 0) == pdTRUE) {
            if (msg) {
                free(msg);
            }
        }
    }

    s_event_json_len = 0;
    s_station_json_len = 0;

    s_audio_ws_stopping = false;

    return ESP_OK;
}

bool audio_ws_is_started(void)
{
    return s_audio_ws_started;
}

/* -------------------------------------------------------------------------- */
/* Public audio interfaces                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t audio_ws_audio_enable(void)
{
#if !APP_ENABLE_WS_AUDIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_audio_ws_stopping) {
        ESP_LOGW(TAG, "audio enable ignored, ws stopping");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_audio_ws_started && !s_audio_ws_starting) {
        ESP_LOGW(TAG, "audio enable ignored, audio_ws not started");
        return ESP_ERR_INVALID_STATE;
    }

    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "audio enable failed, wifi not connected");
        ui_async_update_status("WiFi未连接");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ws_audio && esp_websocket_client_is_connected(s_ws_audio)) {
        s_audio_ws_user_enabled = true;
        s_audio_ws_connected = true;

        ESP_LOGI(TAG, "audio already enabled/connected");
        ui_async_update_status("音频已开启");

        /*
         * 不在这里直接打开功放。
         * 播放任务会在缓冲足够后打开。
         */
        return ESP_OK;
    }

    ESP_LOGI(TAG, "audio enable, start Audio WS");
    ui_async_update_status("连接音频");

    /*
     * 只有准备真正启动 Audio WS 时，才置 true。
     */
    s_audio_ws_user_enabled = true;

    esp_err_t ret = websocket_audio_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "audio enable failed: %s",
                 esp_err_to_name(ret));

        s_audio_ws_user_enabled = false;
        s_audio_ws_connected = false;

        audio_stream_reset_state(true);
        audio_amp_enable(false);
        ui_async_update_voice_level(0);

        ui_async_update_status("音频开启失败");
        return ret;
    }

    return ESP_OK;
#endif
}

esp_err_t audio_ws_audio_disable(void)
{
#if !APP_ENABLE_WS_AUDIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    /*
     * 用户关闭音频。
     */
    s_audio_ws_user_enabled = false;
    s_audio_ws_connected = false;

    ESP_LOGI(TAG, "audio disable, stop Audio WS");
    ui_async_update_status("已静音");

    websocket_audio_stop();

    return ESP_OK;
#endif
}

bool audio_ws_audio_is_enabled(void)
{
    return s_audio_ws_user_enabled;
}

bool audio_ws_audio_is_connected(void)
{
    return s_ws_audio &&
           esp_websocket_client_is_connected(s_ws_audio);
}

esp_err_t audio_set_volume(uint8_t vol)
{
    if (vol > 100) {
        vol = 100;
    }

    s_audio_volume = vol;

    ESP_LOGI(TAG, "Audio volume set to %u", s_audio_volume);

    return ESP_OK;
}

uint8_t audio_get_volume(void)
{
    return s_audio_volume;
}

void audio_ws_set_low_power_disabled(bool disabled)
{
    if (s_low_power_audio_disabled == disabled) {
        return;
    }

    s_low_power_audio_disabled = disabled;

    if (disabled) {
        ESP_LOGW(TAG, "low power audio disabled");

        ring_clear();
        audio_amp_enable(false);
        ui_async_update_voice_level(0);
        ui_async_update_status("电量过低");
    } else {
        ESP_LOGI(TAG, "low power audio restored");
        ui_async_update_status("音频恢复");
    }
}

/* -------------------------------------------------------------------------- */
/* Public station interfaces                                                  */
/* -------------------------------------------------------------------------- */

bool audio_ws_station_is_connected(void)
{
    return s_ws_station &&
           esp_websocket_client_is_connected(s_ws_station);
}

esp_err_t audio_ws_station_get_current(void)
{
    char req[128];

    esp_err_t ret = station_build_get_current(req, sizeof(req));
    if (ret != ESP_OK) {
        return ret;
    }

    return station_send_text_request(req);
}

esp_err_t audio_ws_station_get_list_range(int start, int count)
{
    char req[160];

    esp_err_t ret = station_build_get_list_range(req,
                                                 sizeof(req),
                                                 start,
                                                 count);
    if (ret != ESP_OK) {
        return ret;
    }

    return station_send_text_request(req);
}

esp_err_t audio_ws_station_get_list(int start, int count)
{
    if (start < 0) {
        start = 0;
    }

    if (count <= 0) {
        count = 1;
    }

    char req[192];

    esp_err_t ret = station_build_get_list_range(req,
                                                 sizeof(req),
                                                 start,
                                                 count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "build station getList failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "station getList request: %s", req);

    return station_send_text_request(req);
}

esp_err_t audio_ws_station_set_current(int uid)
{
    char req[160];

    esp_err_t ret = station_build_set_current(req, sizeof(req), uid);
    if (ret != ESP_OK) {
        return ret;
    }

    return station_send_text_request(req);
}

/* -------------------------------------------------------------------------- */
/* Public QSO request interface                                               */
/* -------------------------------------------------------------------------- */

esp_err_t audio_ws_qso_get_list(int page, int page_size)
{
    if (page < 0) {
        page = 0;
    }

    if (page_size <= 0) {
        page_size = QSO_COUNT_DELTA_PAGE_SIZE;
    }

    if (page_size > 100) {
        page_size = 100;
    }

    /*
     * qso/getList 必须走 /ws，也就是 Station WS。
     */
    if (!s_ws_station) {
        ESP_LOGW(TAG, "qso getList failed, station ws is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    if (!esp_websocket_client_is_connected(s_ws_station)) {
        ESP_LOGW(TAG, "qso getList failed, station ws not connected");
        return ESP_ERR_INVALID_STATE;
    }

    char req[160];

    int len = snprintf(
        req,
        sizeof(req),
        "{\"type\":\"qso\",\"subType\":\"getList\",\"data\":{\"page\":%d,\"pageSize\":%d}}",
        page,
        page_size
    );

    if (len <= 0 || len >= (int)sizeof(req)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGD(TAG, "qso getList request: %s", req);

    int ret = esp_websocket_client_send_text(s_ws_station,
                                             req,
                                             len,
                                             pdMS_TO_TICKS(1000));

    if (ret < 0) {
        ESP_LOGW(TAG, "send qso getList failed, ret=%d", ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* QSO polling                                                                */
/* -------------------------------------------------------------------------- */

static void qso_count_poll_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG,
             "qso_count_poll_task started, interval=%d ms",
             QSO_COUNT_POLL_INTERVAL_MS);

    /*
     * /ws connected 后会立即做一次 latest 检查。
     * 这里延迟一个周期后再开始周期检查。
     */
    vTaskDelay(pdMS_TO_TICKS(QSO_COUNT_POLL_INTERVAL_MS));

    while (true) {
        if (s_audio_ws_started &&
            !s_audio_ws_stopping &&
            s_ws_station &&
            esp_websocket_client_is_connected(s_ws_station)) {

            /*
             * 只做 latest 检查，不做完整扫描。
             */
            audio_ws_qso_count_check_latest();
        }

        vTaskDelay(pdMS_TO_TICKS(QSO_COUNT_POLL_INTERVAL_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* QSO response state machine                                                 */
/* -------------------------------------------------------------------------- */

void audio_ws_qso_count_handle_response(int page,
                                        int page_size,
                                        int count,
                                        int latest_log_id,
                                        const int *log_ids,
                                        int log_id_count)
{
    qso_count_mode_t mode = QSO_COUNT_MODE_NONE;

    bool scan_not_active = false;
    bool page_mismatch = false;
    bool need_next = false;
    bool finished_full = false;
    bool need_delta_start = false;
    bool finished_delta = false;
    bool delta_need_next = false;
    bool delta_failed_need_manual = false;
    bool unknown_mode = false;

    int expected_page = 0;
    int next_page = 0;
    int total = 0;

    int32_t old_latest = -1;
    int32_t new_latest = latest_log_id;
    int32_t latest_to_save = -1;

    if (page < 0) {
        page = 0;
    }

    if (page_size <= 0) {
        page_size = QSO_COUNT_DELTA_PAGE_SIZE;
    }

    if (count < 0) {
        count = 0;
    }

    portENTER_CRITICAL(&s_qso_count_mux);

    if (!s_qso_count_scan_active) {
        scan_not_active = true;
    } else {
        mode = s_qso_count_mode;

        if (mode == QSO_COUNT_MODE_CHECK_LATEST) {
            old_latest = s_qso_saved_latest_log_id;

            /*
             * latest 检查到此结束。
             * 如果发现变化，critical 外再启动 delta scan。
             */
            s_qso_count_scan_active = false;
            s_qso_count_mode = QSO_COUNT_MODE_NONE;

            if (latest_log_id >= 0 &&
                latest_log_id != s_qso_saved_latest_log_id) {
                need_delta_start = true;
            }
        } else if (mode == QSO_COUNT_MODE_FULL_SCAN) {
            if (page != s_qso_scan_page) {
                page_mismatch = true;
                expected_page = s_qso_scan_page;

                s_qso_count_scan_active = false;
                s_qso_count_mode = QSO_COUNT_MODE_NONE;
            } else {
                if (page == 0 && latest_log_id >= 0) {
                    s_qso_new_latest_log_id = latest_log_id;
                }

                s_qso_scan_total += count;
                total = s_qso_scan_total;

                if (count >= page_size &&
                    page_size > 0 &&
                    (s_qso_scan_page + 1) < QSO_COUNT_MAX_FULL_SCAN_PAGES) {

                    s_qso_scan_page++;
                    next_page = s_qso_scan_page;
                    need_next = true;
                } else {
                    latest_to_save = s_qso_new_latest_log_id;

                    s_qso_count_scan_active = false;
                    s_qso_count_mode = QSO_COUNT_MODE_NONE;
                    finished_full = true;
                }
            }
        } else if (mode == QSO_COUNT_MODE_DELTA_SCAN) {
            if (page != s_qso_scan_page) {
                page_mismatch = true;
                expected_page = s_qso_scan_page;

                s_qso_count_scan_active = false;
                s_qso_count_mode = QSO_COUNT_MODE_NONE;
            } else {
                int found_index = -1;

                if (log_ids && log_id_count > 0) {
                    for (int i = 0; i < log_id_count; i++) {
                        if (log_ids[i] == s_qso_saved_latest_log_id) {
                            found_index = i;
                            break;
                        }
                    }
                }

                if (found_index >= 0) {
                    /*
                     * found_index 之前的是新增 QSO。
                     */
                    s_qso_delta_count += found_index;

                    total = (int)s_qso_saved_count + s_qso_delta_count;
                    latest_to_save = s_qso_new_latest_log_id;

                    s_qso_count_scan_active = false;
                    s_qso_count_mode = QSO_COUNT_MODE_NONE;
                    finished_delta = true;
                } else {
                    /*
                     * 当前页全是新增。
                     */
                    s_qso_delta_count += count;

                    if (count >= page_size &&
                        page_size > 0 &&
                        (s_qso_scan_page + 1) <
                            QSO_COUNT_MAX_DELTA_SCAN_PAGES) {

                        s_qso_scan_page++;
                        next_page = s_qso_scan_page;
                        delta_need_next = true;
                    } else {
                        /*
                         * 增量扫描找不到旧 latest，说明变化太大。
                         * 不自动完整扫描，提示用户手动同步。
                         */
                        s_qso_count_scan_active = false;
                        s_qso_count_mode = QSO_COUNT_MODE_NONE;
                        delta_failed_need_manual = true;
                    }
                }
            }
        } else {
            unknown_mode = true;
            s_qso_count_scan_active = false;
            s_qso_count_mode = QSO_COUNT_MODE_NONE;
        }
    }

    portEXIT_CRITICAL(&s_qso_count_mux);

    if (scan_not_active) {
        return;
    }

    if (unknown_mode) {
        ESP_LOGW(TAG, "qso response ignored, unknown mode=%d", (int)mode);
        return;
    }

    if (page_mismatch) {
        ESP_LOGW(TAG,
                 "qso response page mismatch, expect=%d got=%d",
                 expected_page,
                 page);
        return;
    }

    if (mode == QSO_COUNT_MODE_CHECK_LATEST) {
        if (need_delta_start) {
            ESP_LOGI(TAG,
                     "qso latest changed, old=%ld new=%ld",
                     (long)old_latest,
                     (long)new_latest);

            audio_ws_qso_count_start_delta_scan(new_latest);
        }

        return;
    }

    if (mode == QSO_COUNT_MODE_FULL_SCAN) {
        if (finished_full) {
            ESP_LOGI(TAG,
                     "qso full scan finished, total=%d latest=%ld",
                     total,
                     (long)latest_to_save);

            /*
             * total==0 && latest==-1 表示确实没有 QSO，也允许保存。
             * total>0 时 latest 必须有效。
             */
            if (latest_to_save >= 0 || total == 0) {
                audio_ws_qso_count_save_state((uint32_t)total,
                                              latest_to_save,
                                              true);
            } else {
                ui_async_qso_sync_popup_show("QSO同步失败", 2000);
            }

            return;
        }

        if (need_next) {
            esp_err_t ret = audio_ws_qso_get_list(next_page, page_size);

            if (ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "qso full scan next page failed: %s",
                         esp_err_to_name(ret));
                ui_async_qso_sync_popup_show("请手动同步QSO", 2500);

                portENTER_CRITICAL(&s_qso_count_mux);
                s_qso_count_scan_active = false;
                s_qso_count_mode = QSO_COUNT_MODE_NONE;
                portEXIT_CRITICAL(&s_qso_count_mux);
            }
        }

        return;
    }

    if (mode == QSO_COUNT_MODE_DELTA_SCAN) {
        if (finished_delta) {
            ESP_LOGI(TAG,
                     "qso delta scan finished, total=%d latest=%ld",
                     total,
                     (long)latest_to_save);

            if (latest_to_save >= 0) {
                audio_ws_qso_count_save_state((uint32_t)total,
                                              latest_to_save,
                                              true);
            } else {
                ui_async_update_status("请同步QSO");
            }

            return;
        }

        if (delta_need_next) {
            esp_err_t ret = audio_ws_qso_get_list(next_page, page_size);

            if (ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "qso delta next page failed: %s",
                         esp_err_to_name(ret));
                ui_async_update_status("请同步QSO");

                portENTER_CRITICAL(&s_qso_count_mux);
                s_qso_count_scan_active = false;
                s_qso_count_mode = QSO_COUNT_MODE_NONE;
                portEXIT_CRITICAL(&s_qso_count_mux);
            }

            return;
        }

        if (delta_failed_need_manual) {
            ESP_LOGW(TAG, "qso delta scan failed, manual sync required");
            ui_async_update_status("请同步QSO");
            return;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* QSO cache and save                                                         */
/* -------------------------------------------------------------------------- */

static void audio_ws_qso_count_load_cache(void)
{
    const app_settings_t *cfg = app_settings_get();

    if (cfg && cfg->qso_count_valid) {
        s_qso_saved_count = cfg->qso_count;
        s_qso_saved_latest_log_id = cfg->qso_latest_log_id;
        s_qso_saved_valid = true;

        ESP_LOGI(TAG,
                 "qso cache loaded: count=%lu latest=%ld",
                 (unsigned long)s_qso_saved_count,
                 (long)s_qso_saved_latest_log_id);
    } else {
        s_qso_saved_count = 0;
        s_qso_saved_latest_log_id = -1;
        s_qso_saved_valid = false;

        ESP_LOGI(TAG, "qso cache empty, manual sync required");
    }
}

static esp_err_t audio_ws_qso_count_save_state(uint32_t count,
                                               int32_t latest_log_id,
                                               bool valid)
{
    qso_save_req_t *req = malloc(sizeof(qso_save_req_t));
    if (!req) {
        ESP_LOGW(TAG, "alloc qso_save_req failed");
        ui_async_update_status("QSO保存失败");
        return ESP_ERR_NO_MEM;
    }

    req->count = count;
    req->latest_log_id = latest_log_id;
    req->valid = valid;

    BaseType_t ret = xTaskCreate(qso_save_task,
                                 "qso_save",
                                 4096,
                                 req,
                                 3,
                                 NULL);

    if (ret != pdPASS) {
        free(req);

        ESP_LOGW(TAG, "create qso_save_task failed");
        ui_async_update_status("QSO保存失败");

        return ESP_FAIL;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* QSO manual full scan                                                       */
/* -------------------------------------------------------------------------- */

void audio_ws_qso_count_manual_full_scan(void)
{
    bool start_request = false;

    if (!s_ws_station ||
        !esp_websocket_client_is_connected(s_ws_station)) {
        ui_async_qso_sync_popup_show("站点WS未连接", 2000);
        return;
    }

    /*
     * 正在通联/播放时不建议同步，避免影响音频。
     */
    if (audio_gate_is_active()) {
        ui_async_qso_sync_popup_show("通联中稍后同步", 2000);
        return;
    }

    portENTER_CRITICAL(&s_qso_count_mux);

    if (!s_qso_count_scan_active) {
        s_qso_count_scan_active = true;
        s_qso_count_mode = QSO_COUNT_MODE_FULL_SCAN;
        s_qso_scan_page = 0;
        s_qso_scan_total = 0;
        s_qso_new_latest_log_id = -1;
        start_request = true;
    }

    portEXIT_CRITICAL(&s_qso_count_mux);

    if (!start_request) {
        ui_async_qso_sync_popup_show("QSO同步中...", 0);
        return;
    }

    ESP_LOGI(TAG, "qso manual full scan start");
    ui_async_qso_sync_popup_show("正在同步QSO...", 0);

    esp_err_t ret = audio_ws_qso_get_list(0, QSO_COUNT_FULL_PAGE_SIZE);

    if (ret != ESP_OK) {
        portENTER_CRITICAL(&s_qso_count_mux);
        s_qso_count_scan_active = false;
        s_qso_count_mode = QSO_COUNT_MODE_NONE;
        portEXIT_CRITICAL(&s_qso_count_mux);

        ui_async_qso_sync_popup_show("QSO同步失败", 2000);
    }
}

/* -------------------------------------------------------------------------- */
/* QSO latest check and delta scan                                             */
/* -------------------------------------------------------------------------- */

static void audio_ws_qso_count_check_latest(void)
{
    bool start_request = false;

    if (!s_qso_saved_valid) {
        /*
         * 没有缓存，不自动完整扫描。
         */
        return;
    }

    portENTER_CRITICAL(&s_qso_count_mux);

    if (!s_qso_count_scan_active) {
        s_qso_count_scan_active = true;
        s_qso_count_mode = QSO_COUNT_MODE_CHECK_LATEST;
        s_qso_scan_page = 0;
        s_qso_scan_total = 0;
        start_request = true;
    }

    portEXIT_CRITICAL(&s_qso_count_mux);

    if (!start_request) {
        return;
    }

    esp_err_t ret = audio_ws_qso_get_list(0, QSO_COUNT_CHECK_PAGE_SIZE);

    if (ret != ESP_OK) {
        portENTER_CRITICAL(&s_qso_count_mux);
        s_qso_count_scan_active = false;
        s_qso_count_mode = QSO_COUNT_MODE_NONE;
        portEXIT_CRITICAL(&s_qso_count_mux);
    }
}

static void audio_ws_qso_count_start_delta_scan(int32_t new_latest_log_id)
{
    bool start_request = false;

    portENTER_CRITICAL(&s_qso_count_mux);

    if (!s_qso_count_scan_active) {
        s_qso_count_scan_active = true;
        s_qso_count_mode = QSO_COUNT_MODE_DELTA_SCAN;
        s_qso_scan_page = 0;
        s_qso_scan_total = 0;
        s_qso_delta_count = 0;
        s_qso_new_latest_log_id = new_latest_log_id;
        start_request = true;
    }

    portEXIT_CRITICAL(&s_qso_count_mux);

    if (!start_request) {
        return;
    }

    ESP_LOGI(TAG,
             "qso delta scan start, oldLatest=%ld newLatest=%ld",
             (long)s_qso_saved_latest_log_id,
             (long)new_latest_log_id);

    esp_err_t ret = audio_ws_qso_get_list(0, QSO_COUNT_DELTA_PAGE_SIZE);

    if (ret != ESP_OK) {
        portENTER_CRITICAL(&s_qso_count_mux);
        s_qso_count_scan_active = false;
        s_qso_count_mode = QSO_COUNT_MODE_NONE;
        portEXIT_CRITICAL(&s_qso_count_mux);
    }
}

/* -------------------------------------------------------------------------- */
/* QSO save task                                                              */
/* -------------------------------------------------------------------------- */

static void qso_save_task(void *arg)
{
    qso_save_req_t *req = (qso_save_req_t *)arg;
    if (!req) {
        vTaskDelete(NULL);
        return;
    }

    uint32_t count = req->count;
    int32_t latest_log_id = req->latest_log_id;
    bool valid = req->valid;

    free(req);

    /*
     * 这里运行在独立任务，不占用 websocket_task 栈。
     */
    esp_err_t ret = app_settings_set_qso_state(count,
                                               latest_log_id,
                                               valid);

    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_qso_count_mux);

        s_qso_saved_count = count;
        s_qso_saved_latest_log_id = latest_log_id;
        s_qso_saved_valid = valid;

        portEXIT_CRITICAL(&s_qso_count_mux);

        ui_async_update_qso_count(count);
        ui_async_qso_sync_popup_show("QSO同步成功", 1500);

        ESP_LOGI(TAG,
                 "qso state saved async: valid=%d count=%lu latest=%ld",
                 valid ? 1 : 0,
                 (unsigned long)count,
                 (long)latest_log_id);
    } else {
        ui_async_qso_sync_popup_show("QSO保存失败", 2000);

        ESP_LOGW(TAG,
                 "qso state save async failed: %s",
                 esp_err_to_name(ret));
    }

    vTaskDelete(NULL);
}
