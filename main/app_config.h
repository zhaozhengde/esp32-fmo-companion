/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file app_config.h
 * @brief 应用级默认配置与功能开关。
 *
 * 本文件用于集中定义应用层默认参数，包括：
 * - 固件版本号；
 * - 默认 WiFi；
 * - 默认 FMO 服务地址；
 * - 默认 WebSocket 地址；
 * - 音量与背光默认值；
 * - 待机时钟配置；
 * - 站点刷新周期；
 * - 电池校准默认参数；
 * - 功能开关；
 * - WebSocket 音频缓冲参数；
 * - 蓝牙音频配置；
 * - 省电模式阈值。
 *
 * @note
 * 本文件只放“应用级配置”。
 * GPIO、LCD、触摸、I2S、ADC 等硬件引脚配置应放在 board_config.h。
 *
 * @warning
 * 不建议将真实 WiFi 密码、私有服务器地址、Token 或个人蓝牙设备名
 * 提交到公开仓库。公开发布前请改为占位符或通过 NVS/配置页保存。
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* -------------------------------------------------------------------------- */
/* Firmware version                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief 固件版本显示文本。
 *
 * 该文本会显示在设置页右上角。
 */
#define APP_VERSION_TEXT "v1.2.2"

/* -------------------------------------------------------------------------- */
/* Default identity                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief 默认本机呼号。
 *
 * 首次启动或 NVS 配置无效时使用。
 */
#define APP_DEFAULT_OWNER_CALLSIGN "BI8SIG"

/* -------------------------------------------------------------------------- */
/* Default WiFi configuration                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief 默认 WiFi SSID。
 *
 * @note
 * WiFi SSID 最大长度为 32 字节，不是 32 个字符。
 * 中文 UTF-8 SSID 中，一个汉字通常占 3 字节。
 *
 * @warning
 * 请勿在公开仓库中提交真实 WiFi 名称和密码。
 */
#define DEFAULT_WIFI_SSID       "YOUR_WIFI_SSID"

/**
 * @brief 默认 WiFi 密码。
 *
 * 允许为空字符串，用于开放网络。
 *
 * @warning
 * 请勿在公开仓库中提交真实 WiFi 密码。
 */
#define DEFAULT_WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

/* -------------------------------------------------------------------------- */
/* Default FMO / WebSocket configuration                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief 默认 FMO Host。
 *
 * 设置页中用户只需要填写该 Host，例如：
 *
 * @code
 * 192.168.3.165
 * example.com:8080
 * @endcode
 *
 * app_settings_build_ws_urls() 会基于该 Host 自动生成：
 *
 * @code
 * ws://<host>/audio
 * ws://<host>/events
 * ws://<host>/ws
 * @endcode
 */
#define DEFAULT_FMO_HOST        "192.168.3.165"

/**
 * @brief 默认音频 WebSocket URL。
 *
 * @note
 * 当前新版配置逻辑通常会根据 DEFAULT_FMO_HOST 自动生成该 URL。
 * 这里保留用于兼容旧代码或调试。
 */
#define DEFAULT_WS_AUDIO_URL    "ws://192.168.3.165/audio"

/**
 * @brief 默认事件 WebSocket URL。
 *
 * @note
 * 当前新版配置逻辑通常会根据 DEFAULT_FMO_HOST 自动生成该 URL。
 */
#define DEFAULT_WS_EVENT_URL    "ws://192.168.3.165/events"

/**
 * @brief 默认站点 WebSocket URL。
 *
 * @note
 * 当前新版配置逻辑通常会根据 DEFAULT_FMO_HOST 自动生成该 URL。
 */
#define DEFAULT_WS_STATION_URL  "ws://192.168.3.165/ws"

/* -------------------------------------------------------------------------- */
/* Default audio and display settings                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief 默认音量百分比，范围 0~100。
 */
#define DEFAULT_AUDIO_VOLUME       60

/**
 * @brief 默认背光百分比，范围 0~100。
 */
#define DEFAULT_BACKLIGHT_PERCENT  30

/* -------------------------------------------------------------------------- */
/* Idle clock configuration                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief 是否默认启用待机时钟页。
 *
 * 0：禁用
 * 1：启用
 */
#define DEFAULT_IDLE_IMAGE_ENABLED     1

/**
 * @brief 默认无操作进入待机时钟页的超时时间，单位 ms。
 *
 * 当前为 150 秒。
 */
#define DEFAULT_IDLE_IMAGE_TIMEOUT_MS  150000

/* -------------------------------------------------------------------------- */
/* Station refresh intervals                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 当前站点刷新周期，单位 ms。
 */
#define DEFAULT_WS_STATION_CURRENT_REFRESH_MS  10000

/**
 * @brief 站点列表刷新周期，单位 ms。
 *
 * 当前设置页主要采用手动刷新，自动刷新功能可由功能开关控制。
 */
#define DEFAULT_WS_STATION_LIST_REFRESH_MS     60000

/* -------------------------------------------------------------------------- */
/* Battery calibration defaults                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 电池电压换算比例，单位 permille。
 *
 * 例如 2000 表示 ADC 采样电压乘以 2.000。
 * 该值需要与实际分压电阻匹配。
 */
#define BATTERY_VOLTAGE_SCALE_PERMILLE  2000

/**
 * @brief 电池电压偏移校准值，单位 mV。
 *
 * 正值表示采样值偏低，需要加上偏移；
 * 负值表示采样值偏高，需要减去偏移。
 */
#define BATTERY_VOLTAGE_OFFSET_MV       0

/**
 * @brief 电池空电电压，单位 mV。
 *
 * 用于电量百分比估算。
 */
#define BATTERY_PERCENT_EMPTY_MV        3000

/**
 * @brief 电池满电电压，单位 mV。
 *
 * 用于电量百分比估算。
 */
#define BATTERY_PERCENT_FULL_MV         4180

/* -------------------------------------------------------------------------- */
/* Board feature switches                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 是否使用 ESP32 内置 DAC 输出音频。
 */
#define APP_BOARD_USE_INTERNAL_DAC      1

/**
 * @brief 当前板级是否带 XPT2046 触摸芯片。
 */
#define APP_BOARD_HAS_XPT2046_TOUCH     1

/**
 * @brief 当前板级是否带 SD 卡。
 */
#define APP_BOARD_HAS_SD_CARD           1

/**
 * @brief 是否使用 SPIFFS 保存待机图片或相关资源。
 */
#define APP_BOARD_HAS_SPIFFS_IDLE       1

/* -------------------------------------------------------------------------- */
/* Application feature switches                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 是否启用 SNTP 时间同步。
 */
#define APP_ENABLE_SNTP          1

/**
 * @brief 是否启用音频 WebSocket。
 */
#define APP_ENABLE_WS_AUDIO      1

/**
 * @brief 是否启用事件 WebSocket。
 */
#define APP_ENABLE_WS_EVENT      1

/**
 * @brief 是否启用站点 WebSocket。
 */
#define APP_ENABLE_WS_STATION    1

/**
 * @brief 是否启用站点列表功能。
 */
#define APP_ENABLE_STATION_LIST  1

/**
 * @brief 是否启用当前站点自动轮询。
 *
 * 当前保留每 10 秒 getCurrent。
 */
#define APP_ENABLE_STATION_CURRENT_AUTO_POLL  1

/**
 * @brief 是否启用站点列表自动轮询。
 *
 * 当前关闭，站点列表改为设置页手动刷新。
 */
#define APP_ENABLE_STATION_LIST_AUTO_POLL     0

/* -------------------------------------------------------------------------- */
/* WebSocket audio PCM format                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief 服务端输入音频采样率。
 *
 * 服务端音频格式：
 *
 * @code
 * raw 16-bit PCM little-endian, mono, 8000 Hz
 * @endcode
 */
#define AUDIO_WS_INPUT_SAMPLE_RATE      8000

/**
 * @brief ESP32 本地输出采样率。
 *
 * 当前 8000 Hz 输入，32000 Hz 输出，使用 4 倍简单升采样。
 */
#define AUDIO_OUTPUT_SAMPLE_RATE        32000

/**
 * @brief 音频升采样倍率。
 */
#define AUDIO_UPSAMPLE_FACTOR           4

/**
 * @brief 音频 RingBuffer 样本数。
 *
 * RingBuffer 存放 8000 Hz int16 mono PCM。
 * 8000 samples 约等于 1 秒，约 16 KB RAM。
 *
 * @note
 * 原注释中写 16000 samples = 2 秒，32 KB。
 * 但当前宏值为 8000，因此实际为约 1 秒，16 KB。
 */
#define AUDIO_RING_SAMPLES              8000

/**
 * @brief 每次播放处理的输入样本数。
 *
 * 128 / 8000 = 16 ms。
 */
#define PLAY_CHUNK_SAMPLES              128

/**
 * @brief 播放启动所需的最小缓冲样本数。
 *
 * 800 samples / 8000 Hz = 100 ms。
 */
#define START_BUFFER_SAMPLES            800

/**
 * @brief 默认目标缓冲样本数。
 *
 * 3000 samples / 8000 Hz = 375 ms。
 */
#define TARGET_BUFFER_DEFAULT           3000

/**
 * @brief 最小目标缓冲样本数。
 */
#define TARGET_BUFFER_MIN               1500

/**
 * @brief 最大目标缓冲样本数。
 */
#define TARGET_BUFFER_MAX               5000

/**
 * @brief 目标缓冲增加步进。
 */
#define TARGET_BUFFER_INC_STEP          500

/**
 * @brief 目标缓冲减少步进。
 */
#define TARGET_BUFFER_DEC_STEP          200

/**
 * @brief 最大允许延迟样本数。
 *
 * 超过该值认为延迟过高，可丢弃老数据。
 */
#define MAX_LATENCY_SAMPLES             6000

/**
 * @brief 旧动态追赶算法阈值。
 *
 * 当前不使用旧动态丢包算法，保留用于兼容或后续调试。
 */
#define CATCHUP_THRESHOLD_MARGIN        3000

/**
 * @brief 高延迟边界。
 *
 * 当前不使用旧动态丢包算法，保留用于兼容或后续调试。
 */
#define HIGH_LATENCY_MARGIN             6000

/**
 * @brief 动态丢包最大样本数。
 *
 * 当前不使用旧动态丢包算法，保留用于兼容或后续调试。
 */
#define MAX_DYNAMIC_DROP_SAMPLES        80

/**
 * @brief 播放欠载容忍阈值。
 *
 * 播放开始后不轻易重新缓冲，欠载时补静音。
 */
#define UNDERFLOW_TOLERANCE             100000

/* -------------------------------------------------------------------------- */
/* Bluetooth audio configuration                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 是否启用蓝牙音频输出功能。
 *
 * 0：关闭蓝牙音频，只使用本机播放
 * 1：启用蓝牙音频，支持连接蓝牙音箱/耳机播放
 *
 * @note
 * ESP32-WROOM-32E-N4 资源较紧张，默认建议设为 0。
 */
#define APP_CONFIG_BT_AUDIO_ENABLE      0

/**
 * @brief 音频输出模式：本机播放。
 */
#define APP_CONFIG_AUDIO_OUTPUT_LOCAL   0

/**
 * @brief 音频输出模式：蓝牙播放。
 */
#define APP_CONFIG_AUDIO_OUTPUT_BT      1

/**
 * @brief 音频输出模式：自动。
 *
 * 蓝牙已连接时走蓝牙，否则走本机。
 */
#define APP_CONFIG_AUDIO_OUTPUT_AUTO    2

/**
 * @brief 默认音频输出模式。
 */
#define APP_CONFIG_AUDIO_OUTPUT_DEFAULT APP_CONFIG_AUDIO_OUTPUT_AUTO

/**
 * @brief ESP32 蓝牙设备名。
 *
 * 手机或音箱扫描时可能看到该名称。
 */
#define APP_CONFIG_BT_DEVICE_NAME       "ESP32-AUDIO"

/**
 * @brief 蓝牙目标设备名。
 *
 * 初版按名称自动连接蓝牙音箱/耳机。
 *
 * @warning
 * 公开发布前建议改成占位符，或迁移到 app_settings/NVS。
 */
#define APP_CONFIG_BT_TARGET_NAME       "YOUR_BT_SPEAKER_NAME"

/**
 * @brief A2DP Source 输出采样率。
 *
 * ESP-IDF A2DP Source 示例通常使用 44100 Hz / stereo / 16-bit PCM。
 */
#define APP_CONFIG_BT_SAMPLE_RATE       44100

/**
 * @brief A2DP Source 输出声道数。
 */
#define APP_CONFIG_BT_CHANNELS          2

/**
 * @brief A2DP Source 每样本位数。
 */
#define APP_CONFIG_BT_BITS_PER_SAMPLE   16

/* -------------------------------------------------------------------------- */
/* Power-save configuration                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief 省电模式下背光百分比。
 */
#define APP_POWER_SAVE_BACKLIGHT_PERCENT  10

/**
 * @brief 低电量进入省电模式阈值，单位百分比。
 */
#define APP_POWER_SAVE_ENTER_PERCENT      10

/**
 * @brief 低电量退出省电模式阈值，单位百分比。
 *
 * 该值应高于进入阈值，形成回差，避免电量在阈值附近反复切换。
 */
#define APP_POWER_SAVE_EXIT_PERCENT       25

/**
 * @brief 低电量进入省电模式电压兜底阈值，单位 mV。
 */
#define APP_POWER_SAVE_ENTER_MV           3450

/**
 * @brief 低电量退出省电模式电压兜底阈值，单位 mV。
 */
#define APP_POWER_SAVE_EXIT_MV            3650

#endif /* APP_CONFIG_H */
