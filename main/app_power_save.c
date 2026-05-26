/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file app_power_save.c
 * @brief 应用省电模式管理实现。
 *
 * 本模块通过 bitmask 维护省电原因。
 *
 * 设计逻辑：
 * - 任意省电原因存在时进入省电；
 * - 所有省电原因清除后退出省电；
 * - 手动省电和低电量省电可以同时存在；
 * - 低电量省电存在时，UI 不允许通过普通点击退出省电时钟页。
 */

#include "app_power_save.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_log.h"

/* Project headers ---------------------------------------------------------- */
#include "app_config.h"
#include "app_settings.h"
#include "audio_ws.h"
#include "ui_async.h"
#include "wifi_manager.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "app_power_save";

/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief 当前省电原因位图。
 */
static uint32_t s_power_save_reasons = 0;

/**
 * @brief 当前是否已经进入省电模式。
 */
static bool s_power_save_active = false;

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static void app_power_save_enter(void);
static void app_power_save_exit(void);

/* -------------------------------------------------------------------------- */
/* Private helpers                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief 进入省电模式。
 *
 * 当前省电动作：
 * - 停止 WebSocket / 网络音频；
 * - 停止 WiFi；
 * - UI 进入省电时钟页；
 * - 降低背光；
 * - 更新状态提示。
 */
static void app_power_save_enter(void)
{
    if (s_power_save_active) {
        return;
    }

    s_power_save_active = true;

    ESP_LOGW(TAG,
             "enter power save, reasons=0x%lx",
             (unsigned long)s_power_save_reasons);

    /*
     * 先停止网络音频和 WebSocket。
     */
    audio_ws_stop();

    /*
     * 再关闭 WiFi。
     */
    wifi_manager_stop();

    /*
     * UI 进入省电时钟屏。
     */
    ui_async_enter_power_save_clock();

    /*
     * 背光降到省电亮度。
     */
    ui_async_set_backlight_percent(APP_POWER_SAVE_BACKLIGHT_PERCENT);

    ui_async_update_status("省电模式");
}

/**
 * @brief 退出省电模式。
 *
 * 当前恢复动作：
 * - 退出省电时钟页；
 * - 恢复背光；
 * - 重启 WiFi；
 * - 状态栏提示退出省电。
 *
 * @note
 * audio_ws_start() 建议在 WiFi got IP 后自动调用，
 * 因此这里不直接启动 WebSocket。
 */
static void app_power_save_exit(void)
{
    if (!s_power_save_active) {
        return;
    }

    s_power_save_active = false;

    ESP_LOGW(TAG, "exit power save");

    /*
     * 退出省电时钟屏。
     */
    ui_async_exit_power_save_clock();

    /*
     * 恢复背光。
     */
    const app_settings_t *cfg = app_settings_get();

    uint8_t bl = cfg ? cfg->backlight_percent : DEFAULT_BACKLIGHT_PERCENT;

    if (bl < 5) {
        bl = DEFAULT_BACKLIGHT_PERCENT;
    }

    ui_async_set_backlight_percent(bl);

    /*
     * 恢复 WiFi。
     * WebSocket 建议由 WiFi got IP 事件统一启动。
     */
    wifi_manager_restart();

    ui_async_update_status("退出省电");
}

/* -------------------------------------------------------------------------- */
/* Public interfaces                                                          */
/* -------------------------------------------------------------------------- */

void app_power_save_init(void)
{
    s_power_save_reasons = 0;
    s_power_save_active = false;
}

void app_power_save_set_reason(app_power_save_reason_t reason, bool enable)
{
    uint32_t reason_mask = (uint32_t)reason;

    if (reason_mask == 0) {
        return;
    }

    uint32_t old_reasons = s_power_save_reasons;

    if (enable) {
        s_power_save_reasons |= reason_mask;
    } else {
        s_power_save_reasons &= ~reason_mask;
    }

    if (old_reasons == s_power_save_reasons) {
        return;
    }

    ESP_LOGI(TAG,
             "power save reasons changed: 0x%lx -> 0x%lx",
             (unsigned long)old_reasons,
             (unsigned long)s_power_save_reasons);

    if (s_power_save_reasons != 0) {
        app_power_save_enter();
    } else {
        app_power_save_exit();
    }
}

void app_power_save_toggle_manual(void)
{
    bool manual = app_power_save_is_manual_enabled();

    app_power_save_set_reason(APP_POWER_SAVE_REASON_MANUAL, !manual);
}

bool app_power_save_is_active(void)
{
    return s_power_save_active;
}

bool app_power_save_is_manual_enabled(void)
{
    return (s_power_save_reasons & APP_POWER_SAVE_REASON_MANUAL) != 0;
}

bool app_power_save_has_reason(app_power_save_reason_t reason)
{
    return (s_power_save_reasons & (uint32_t)reason) != 0;
}
