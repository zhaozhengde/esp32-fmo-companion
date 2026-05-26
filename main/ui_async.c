/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file ui_async.c
 * @brief LVGL UI 异步更新接口实现。
 *
 * 本文件封装 lv_async_call()，用于从后台 FreeRTOS 任务、
 * WebSocket 回调、解析任务等非 LVGL 上下文安全地请求 UI 更新。
 *
 * 设计原则：
 * - 对外接口可以从任意任务调用；
 * - 内部动态分配消息结构；
 * - 通过 lv_async_call() 投递到 LVGL 上下文；
 * - 在 LVGL 回调中调用 app_ui_xxx()；
 * - 回调执行完成后释放消息结构。
 */

#include "ui_async.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_log.h"

/* LVGL headers ------------------------------------------------------------- */
#include "lvgl.h"

/* Project headers ---------------------------------------------------------- */
#include "app_ui.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "ui_async";

/* -------------------------------------------------------------------------- */
/* Private macros                                                             */
/* -------------------------------------------------------------------------- */

/*
 * 异步消息中的文本缓存长度。
 *
 * 该长度需要覆盖：
 * - 状态栏文本
 * - 呼号
 * - 站点名称
 * - QSO 弹窗提示
 *
 * 超出长度的文本会被截断。
 */
#define UI_ASYNC_TEXT_MAX_LEN    96

/*
 * UI 上显示的 QSO 数量上限。
 * 这里限制主要是为了避免异常值导致显示异常。
 */
#define UI_ASYNC_QSO_COUNT_MAX   999999UL

/* -------------------------------------------------------------------------- */
/* Private types                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief UI 异步消息类型。
 */
typedef enum {
    UI_ASYNC_STATUS = 1,              /*!< 更新底部状态栏 */
    UI_ASYNC_TALKER,                  /*!< 更新当前说话人，兼容旧接口 */
    UI_ASYNC_TALKER_STATE,            /*!< 更新当前说话人和说话状态 */
    UI_ASYNC_LAST_CALL,               /*!< 更新上次通联呼号 */
    UI_ASYNC_STATION,                 /*!< 更新当前站点 */
    UI_ASYNC_WIFI_RSSI,               /*!< 更新 WiFi RSSI */
    UI_ASYNC_BATTERY,                 /*!< 更新电池状态 */
    UI_ASYNC_VOICE_LEVEL,             /*!< 更新音频电平 */
    UI_ASYNC_BACKLIGHT,               /*!< 设置背光 */
    UI_ASYNC_STATION_LIST_UPDATED,    /*!< 站点列表缓存已更新 */
    UI_ASYNC_WAKE_FROM_IDLE,          /*!< 从普通待机状态唤醒 */
    UI_ASYNC_ENTER_POWER_SAVE_CLOCK,  /*!< 进入省电时钟页 */
    UI_ASYNC_EXIT_POWER_SAVE_CLOCK,   /*!< 退出省电时钟页 */
    UI_ASYNC_UPDATE_QSO_COUNT,        /*!< 更新 QSO 数量 */
    UI_ASYNC_QSO_SYNC_POPUP_SHOW,     /*!< 显示 QSO 同步弹窗 */
    UI_ASYNC_QSO_SYNC_POPUP_CLOSE,    /*!< 关闭 QSO 同步弹窗 */
} ui_async_type_t;

/**
 * @brief UI 异步消息。
 *
 * 字段说明：
 *
 * value1 / value2 作为通用数值参数：
 * - UI_ASYNC_TALKER_STATE:
 *   - value1 = active
 *
 * - UI_ASYNC_WIFI_RSSI:
 *   - value1 = rssi_dbm
 *
 * - UI_ASYNC_BATTERY:
 *   - value1 = percent
 *   - value2 = charging
 *
 * - UI_ASYNC_VOICE_LEVEL:
 *   - value1 = level
 *
 * - UI_ASYNC_BACKLIGHT:
 *   - value1 = percent
 *
 * - UI_ASYNC_UPDATE_QSO_COUNT:
 *   - value1 = count
 *
 * - UI_ASYNC_QSO_SYNC_POPUP_SHOW:
 *   - value1 = auto_close_ms
 *
 * text 作为通用文本参数：
 * - 状态栏文本
 * - 当前呼号
 * - 上次通联呼号
 * - 站点名称
 * - QSO 弹窗文本
 */
typedef struct {
    ui_async_type_t type;
    int32_t value1;
    int32_t value2;
    char text[UI_ASYNC_TEXT_MAX_LEN];
} ui_async_msg_t;

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static void ui_async_cb(void *arg);

static void ui_async_post_msg(ui_async_msg_t *msg);
static void ui_async_post_value(ui_async_type_t type,
                                int32_t value1,
                                int32_t value2);
static void ui_async_post_text(ui_async_type_t type,
                               const char *text);
static void ui_async_post_text_value(ui_async_type_t type,
                                     const char *text,
                                     int32_t value1,
                                     int32_t value2);
static void ui_async_copy_text(char *dst,
                               size_t dst_size,
                               const char *src);

/* -------------------------------------------------------------------------- */
/* Private helpers                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief 安全拷贝文本到异步消息。
 *
 * @param dst 目标缓存。
 * @param dst_size 目标缓存大小。
 * @param src 源字符串，可为 NULL。
 */
static void ui_async_copy_text(char *dst,
                               size_t dst_size,
                               const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (src && src[0]) {
        strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

/**
 * @brief 投递已构造好的异步消息。
 *
 * @param msg 异步消息指针。调用成功后所有权交给 LVGL async 回调。
 */
static void ui_async_post_msg(ui_async_msg_t *msg)
{
    if (!msg) {
        return;
    }

    /*
     * lv_async_call() 返回 void。
     *
     * 正常情况下，msg 会暂存在 LVGL async 队列中，
     * 并在 ui_async_cb() 执行完成后释放。
     *
     * 注意：
     * 如果 LVGL 未初始化或没有运行 lv_timer_handler()，
     * async 回调将不会被执行，msg 也无法释放。
     * 因此业务代码应在 UI/LVGL 初始化后再调用本模块接口。
     */
    lv_async_call(ui_async_cb, msg);
}

/**
 * @brief 投递纯数值消息。
 */
static void ui_async_post_value(ui_async_type_t type,
                                int32_t value1,
                                int32_t value2)
{
    ui_async_msg_t *msg = calloc(1, sizeof(ui_async_msg_t));
    if (!msg) {
        ESP_LOGW(TAG, "alloc async value msg failed, type=%d", (int)type);
        return;
    }

    msg->type = type;
    msg->value1 = value1;
    msg->value2 = value2;
    msg->text[0] = '\0';

    ui_async_post_msg(msg);
}

/**
 * @brief 投递纯文本消息。
 */
static void ui_async_post_text(ui_async_type_t type,
                               const char *text)
{
    ui_async_post_text_value(type, text, 0, 0);
}

/**
 * @brief 投递文本 + 数值消息。
 */
static void ui_async_post_text_value(ui_async_type_t type,
                                     const char *text,
                                     int32_t value1,
                                     int32_t value2)
{
    ui_async_msg_t *msg = calloc(1, sizeof(ui_async_msg_t));
    if (!msg) {
        ESP_LOGW(TAG, "alloc async text/value msg failed, type=%d", (int)type);
        return;
    }

    msg->type = type;
    msg->value1 = value1;
    msg->value2 = value2;

    ui_async_copy_text(msg->text, sizeof(msg->text), text);

    ui_async_post_msg(msg);
}

/* -------------------------------------------------------------------------- */
/* LVGL async callback                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief LVGL async 回调。
 *
 * 该函数运行在 LVGL 上下文中，可以安全调用 app_ui_xxx() 更新控件。
 *
 * @param arg ui_async_msg_t 指针。
 */
static void ui_async_cb(void *arg)
{
    ui_async_msg_t *msg = (ui_async_msg_t *)arg;
    if (!msg) {
        return;
    }

    switch (msg->type) {
    case UI_ASYNC_STATUS:
        app_ui_update_status(msg->text);
        break;

    case UI_ASYNC_TALKER:
        app_ui_update_talker(msg->text);
        break;

    case UI_ASYNC_TALKER_STATE:
        app_ui_update_talker_state(msg->text, msg->value1 ? true : false);
        break;

    case UI_ASYNC_LAST_CALL:
        app_ui_update_last_call(msg->text);
        break;

    case UI_ASYNC_STATION:
        app_ui_update_station(msg->text);
        break;

    case UI_ASYNC_WIFI_RSSI:
        app_ui_update_wifi_rssi((int)msg->value1);
        break;

    case UI_ASYNC_BATTERY:
        app_ui_update_battery((uint8_t)msg->value1,
                              msg->value2 ? true : false);
        break;

    case UI_ASYNC_VOICE_LEVEL:
        app_ui_update_voice_level((uint8_t)msg->value1);
        break;

    case UI_ASYNC_BACKLIGHT:
        app_ui_set_backlight_percent((uint8_t)msg->value1);
        break;

    case UI_ASYNC_STATION_LIST_UPDATED:
        app_ui_station_list_updated();
        break;

    case UI_ASYNC_WAKE_FROM_IDLE:
        app_ui_wake_from_idle();
        break;

    case UI_ASYNC_ENTER_POWER_SAVE_CLOCK:
        app_ui_enter_power_save_clock();
        break;

    case UI_ASYNC_EXIT_POWER_SAVE_CLOCK:
        app_ui_exit_power_save_clock();
        break;

    case UI_ASYNC_UPDATE_QSO_COUNT:
        app_ui_update_qso_count((uint32_t)msg->value1);
        break;

    case UI_ASYNC_QSO_SYNC_POPUP_SHOW:
        app_ui_qso_sync_popup_show(msg->text, (uint32_t)msg->value1);
        break;

    case UI_ASYNC_QSO_SYNC_POPUP_CLOSE:
        app_ui_qso_sync_popup_close();
        break;

    default:
        ESP_LOGW(TAG, "unknown async msg type=%d", (int)msg->type);
        break;
    }

    free(msg);
}

/* -------------------------------------------------------------------------- */
/* Public interfaces: status and main screen                                  */
/* -------------------------------------------------------------------------- */

void ui_async_update_status(const char *text)
{
    ui_async_post_text(UI_ASYNC_STATUS, text);
}

void ui_async_update_talker(const char *talker)
{
    ui_async_post_text(UI_ASYNC_TALKER, talker);
}

void ui_async_update_talker_state(const char *talker, bool active)
{
    ui_async_post_text_value(UI_ASYNC_TALKER_STATE,
                             talker,
                             active ? 1 : 0,
                             0);
}

void ui_async_update_last_call(const char *callsign)
{
    ui_async_post_text(UI_ASYNC_LAST_CALL, callsign);
}

void ui_async_update_station(const char *station)
{
    ui_async_post_text(UI_ASYNC_STATION, station);
}

void ui_async_update_qso_count(uint32_t count)
{
    if (count > UI_ASYNC_QSO_COUNT_MAX) {
        count = UI_ASYNC_QSO_COUNT_MAX;
    }

    ui_async_post_value(UI_ASYNC_UPDATE_QSO_COUNT, (int32_t)count, 0);
}

void ui_async_station_list_updated(void)
{
    ui_async_post_value(UI_ASYNC_STATION_LIST_UPDATED, 0, 0);
}

/* -------------------------------------------------------------------------- */
/* Public interfaces: WiFi, battery and audio                                 */
/* -------------------------------------------------------------------------- */

void ui_async_update_wifi_rssi(int rssi_dbm)
{
    ui_async_post_value(UI_ASYNC_WIFI_RSSI, (int32_t)rssi_dbm, 0);
}

void ui_async_update_battery(uint8_t percent, bool charging)
{
    if (percent > 100) {
        percent = 100;
    }

    ui_async_post_value(UI_ASYNC_BATTERY,
                        (int32_t)percent,
                        charging ? 1 : 0);
}

void ui_async_update_voice_level(uint8_t level)
{
    if (level > 100) {
        level = 100;
    }

    /*
     * 当前 app_ui 中音频电平条已取消，接口为空实现。
     *
     * 如果后续恢复音频电平条，建议不要对高频音频电平使用
     * 每帧 malloc + lv_async_call 的方式，否则会增加 heap 压力和
     * LVGL async 队列压力。
     */
    app_ui_set_voice_level_pending(level);
}

/* -------------------------------------------------------------------------- */
/* Public interfaces: backlight and idle / power-save clock                   */
/* -------------------------------------------------------------------------- */

void ui_async_set_backlight_percent(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    ui_async_post_value(UI_ASYNC_BACKLIGHT, (int32_t)percent, 0);
}

void ui_async_wake_from_idle(void)
{
    ui_async_post_value(UI_ASYNC_WAKE_FROM_IDLE, 0, 0);
}

void ui_async_enter_power_save_clock(void)
{
    ui_async_post_value(UI_ASYNC_ENTER_POWER_SAVE_CLOCK, 0, 0);
}

void ui_async_exit_power_save_clock(void)
{
    ui_async_post_value(UI_ASYNC_EXIT_POWER_SAVE_CLOCK, 0, 0);
}

/* -------------------------------------------------------------------------- */
/* Public interfaces: QSO sync popup                                          */
/* -------------------------------------------------------------------------- */

void ui_async_qso_sync_popup_show(const char *text, uint32_t auto_close_ms)
{
    /*
     * auto_close_ms 存入 int32_t。
     * 当前 UI 弹窗自动关闭时间通常较小，足够使用。
     * 如后续需要支持超过 INT32_MAX 的时间，应调整消息结构字段类型。
     */
    if (auto_close_ms > INT32_MAX) {
        auto_close_ms = INT32_MAX;
    }

    ui_async_post_text_value(UI_ASYNC_QSO_SYNC_POPUP_SHOW,
                             text,
                             (int32_t)auto_close_ms,
                             0);
}

void ui_async_qso_sync_popup_close(void)
{
    ui_async_post_value(UI_ASYNC_QSO_SYNC_POPUP_CLOSE, 0, 0);
}
