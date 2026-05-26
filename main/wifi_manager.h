/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file wifi_manager.h
 * @brief WiFi STA 连接、状态查询与扫描接口。
 *
 * 本模块负责：
 * - 初始化 WiFi STA；
 * - 根据 app_settings 中的 SSID/密码连接 AP；
 * - 断线后自动重连；
 * - 查询连接状态和 RSSI；
 * - 同步扫描附近 WiFi；
 * - 省电模式下停止/恢复 WiFi。
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

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
 * @brief WiFi 扫描结果项。
 */
typedef struct {
    /**
     * @brief SSID。
     *
     * 802.11 SSID 最大 32 字节，这里额外 1 字节保存 '\0'。
     */
    char ssid[33];

    /**
     * @brief 信号强度，单位 dBm。
     */
    int8_t rssi;

    /**
     * @brief 认证模式，取值对应 wifi_auth_mode_t。
     */
    uint8_t authmode;

    /**
     * @brief AP 所在信道。
     */
    uint8_t channel;
} wifi_scan_item_t;

/* -------------------------------------------------------------------------- */
/* Public interfaces                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief 启动 WiFi STA。
 *
 * 会使用 app_settings 中保存的 SSID 和密码进行连接。
 * 如果 WiFi 已经启动，则直接返回 ESP_OK。
 *
 * @return
 *      - ESP_OK：启动成功或已经启动
 *      - 其他值：ESP-IDF WiFi 初始化/启动错误码
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief 停止 WiFi。
 *
 * 停止后会设置内部 stopping 标志，避免断线事件中自动重连。
 *
 * @return ESP_OK 或 WiFi 停止错误码。
 */
esp_err_t wifi_manager_stop(void);

/**
 * @brief 重新启动 WiFi。
 *
 * 通常用于退出省电模式后恢复 WiFi。
 *
 * @return ESP_OK 或 WiFi 启动错误码。
 */
esp_err_t wifi_manager_restart(void);

/**
 * @brief 判断 WiFi 管理模块是否已启动。
 *
 * @return true 表示 WiFi 已启动。
 */
bool wifi_manager_is_started(void);

/**
 * @brief 判断当前是否已连接 AP 并获得 IP。
 *
 * @return true 表示已连接。
 */
bool wifi_manager_is_connected(void);

/**
 * @brief 获取当前连接 AP 的 RSSI。
 *
 * @return
 *      - 已连接：RSSI，单位 dBm
 *      - 未连接或获取失败：0
 */
int wifi_manager_get_rssi(void);

/**
 * @brief 同步扫描附近 WiFi。
 *
 * @param items 输出扫描结果数组。
 * @param max_items 输出数组最大项数。
 * @param out_count 实际写入结果数量。
 *
 * @return
 *      - ESP_OK：扫描成功
 *      - ESP_ERR_INVALID_ARG：参数非法
 *      - ESP_ERR_INVALID_STATE：WiFi 未启动或当前处于省电模式
 *      - ESP_ERR_NO_MEM：内存不足
 *      - 其他值：ESP-IDF WiFi 扫描错误码
 *
 * @note
 * 该函数会阻塞约 1~3 秒。不要在 LVGL/UI 线程中直接调用。
 * 当前 UI 设置页已通过后台任务调用该函数。
 */
esp_err_t wifi_manager_scan(wifi_scan_item_t *items,
                            int max_items,
                            int *out_count);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
