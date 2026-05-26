/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file main.c
 * @brief 应用启动入口。
 *
 * 启动流程：
 * 1. 初始化 NVS；
 * 2. 加载应用配置；
 * 3. 初始化省电管理；
 * 4. 初始化 LVGL；
 * 5. 初始化显示与触摸；
 * 6. 创建 LVGL tick 定时器；
 * 7. 创建 UI；
 * 8. 初始化音频输出抽象层；
 * 9. 启动 WiFi；
 * 10. 启动 RSSI 监控任务；
 * 11. WiFi 连接后启动 WebSocket；
 * 12. 启动电池监测；
 * 13. 进入 LVGL 主循环。
 */

#include <stdio.h>

/* FreeRTOS headers --------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

/* LVGL headers ------------------------------------------------------------- */
#include "lvgl.h"

/* Board / display port headers -------------------------------------------- */
#include "lv_port_disp.h"
#include "lv_port_indev.h"

/* Project headers ---------------------------------------------------------- */
#include "app_power_save.h"
#include "app_settings.h"
#include "app_ui.h"
#include "audio_output.h"
#include "audio_ws.h"
#include "battery_monitor.h"
#include "ui_async.h"
#include "wifi_manager.h"

/*
 * 如果当前 main.c 不直接调用 st7789.h 中的接口，可以删除该 include。
 * 目前显示初始化由 lv_port_disp_init() 完成。
 */
/* #include "st7789.h" */

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "main";

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static void lv_tick_task(void *arg);
static void app_nvs_init(void);
static void wifi_rssi_task(void *arg);
static void audio_ws_start_task(void *arg);

/* -------------------------------------------------------------------------- */
/* LVGL tick                                                                  */
/* -------------------------------------------------------------------------- */

static void lv_tick_task(void *arg)
{
    LV_UNUSED(arg);

    lv_tick_inc(1);
}

/* -------------------------------------------------------------------------- */
/* NVS initialization                                                         */
/* -------------------------------------------------------------------------- */

static void app_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG,
                 "NVS needs erase, err=%s",
                 esp_err_to_name(err));

        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_LOGI(TAG, "NVS initialized");
}

/* -------------------------------------------------------------------------- */
/* Background tasks                                                           */
/* -------------------------------------------------------------------------- */

static void wifi_rssi_task(void *arg)
{
    LV_UNUSED(arg);

    while (true) {
        if (wifi_manager_is_connected()) {
            int rssi = wifi_manager_get_rssi();

            ui_async_update_wifi_rssi(rssi);
        } else {
            ui_async_update_wifi_rssi(-127);
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void audio_ws_start_task(void *arg)
{
    LV_UNUSED(arg);

    ui_async_update_status("等待WiFi连接");

    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ui_async_update_status("连接FMO");

    esp_err_t err = audio_ws_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "audio_ws_start failed: %s",
                 esp_err_to_name(err));
        ui_async_update_status("FMO连接失败");
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Application entry                                                          */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    esp_log_level_set("XPT2046", ESP_LOG_WARN);

    ESP_LOGI(TAG, "app start");

    /*
     * 1. 初始化 NVS。
     * 后续 app_settings、WiFi、Web 配置都会依赖 NVS。
     */
    app_nvs_init();

    /*
     * 2. 初始化应用配置。
     * 如果 NVS 中没有配置或版本不匹配，会自动写入默认配置。
     */
    ESP_ERROR_CHECK(app_settings_init());

    const app_settings_t *cfg = app_settings_get();

    if (cfg) {
        ESP_LOGI(TAG,
                 "config: backlight=%u, volume=%u, rotate180=%d",
                 cfg->backlight_percent,
                 cfg->audio_volume,
                 cfg->screen_rotate_180 ? 1 : 0);
    } else {
        ESP_LOGW(TAG, "app_settings_get returned NULL");
    }

    /*
     * 3. 初始化省电管理。
     */
    app_power_save_init();

    /*
     * 4. 初始化 LVGL。
     */
    lv_init();

    /*
     * 5. 应用屏幕旋转配置。
     * 需要在 lv_port_disp_init() 前设置。
     */
    lv_port_disp_set_rotate_180(cfg && cfg->screen_rotate_180);

    /*
     * 6. 初始化显示。
     */
    lv_port_disp_init();

    /*
     * 7. 初始化触摸。
     */
    lv_port_indev_init();

    /*
     * 8. 创建 LVGL tick 定时器。
     * 周期 1000us，即每 1ms 调用一次 lv_tick_inc(1)。
     */
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "lv_tick",
    };

    esp_timer_handle_t periodic_timer = NULL;

    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args,
                                     &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 1000));

    /*
     * 9. 创建 UI。
     */
    app_ui_create();

    /*
     * 10. 使用 NVS 配置刷新背光。
     * app_ui_create() 内部已经会应用配置，这里再次刷新作为兜底。
     */
    if (cfg) {
        ui_async_set_backlight_percent(cfg->backlight_percent);
    }

    ui_async_update_status("配置加载完成");

    ESP_LOGI(TAG, "ui created");

    /*
     * 11. 初始化音频输出抽象层。
     *
     * 需要在 audio_ws 播放任务真正写入音频前完成。
     */
    ESP_ERROR_CHECK(audio_output_init());

    /*
     * 12. 启动 WiFi。
     */
    ESP_ERROR_CHECK(wifi_manager_start());

    /*
     * 13. 启动 WiFi RSSI 周期更新任务。
     */
    BaseType_t task_ret = xTaskCreate(wifi_rssi_task,
                                      "wifi_rssi",
                                      3072,
                                      NULL,
                                      5,
                                      NULL);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "create wifi_rssi_task failed");
    }

    /*
     * 14. 创建 WebSocket 启动任务。
     *
     * 该任务会等待 WiFi 连接成功后调用 audio_ws_start()。
     */
    task_ret = xTaskCreate(audio_ws_start_task,
                           "audio_ws_start",
                           4096,
                           NULL,
                           5,
                           NULL);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "create audio_ws_start_task failed");
    }

    /*
     * 15. 启动电池监测。
     */
    ESP_ERROR_CHECK(battery_monitor_start());

    /*
     * 16. LVGL 主循环。
     *
     * 当前项目中 LVGL 对象操作必须在该主循环上下文或
     * lv_async_call() 回调上下文中执行。
     */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));

#if LVGL_VERSION_MAJOR >= 8
        lv_timer_handler();
#else
        lv_task_handler();
#endif
    }
}
