/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file battery_monitor.c
 * @brief 电池电压采样、电量估算与低电省电联动实现。
 *
 * 本模块使用 ESP32 ADC1 周期采样电池电压，并根据：
 * - ADC 校准结果；
 * - 硬件分压比例；
 * - 用户配置的电压偏移；
 * - 空电/满电校准值；
 *
 * 计算电池实际电压与电量百分比。
 *
 * 当电量或电压过低时，本模块会通知 app_power_save 模块
 * 进入低电量省电模式。
 *
 * @note
 * 当前实现没有充电状态检测能力，因此 UI 更新时默认按
 * “未充电”状态显示电量颜色。
 */

#include "battery_monitor.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

/* FreeRTOS headers --------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF headers ---------------------------------------------------------- */
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_err.h"
#include "esp_log.h"

/* Project headers ---------------------------------------------------------- */
#include "app_config.h"
#include "app_power_save.h"
#include "app_settings.h"
#include "board_config.h"
#include "ui_async.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "battery_monitor";

/* -------------------------------------------------------------------------- */
/* Private macros: battery power-state thresholds                             */
/* -------------------------------------------------------------------------- */

/*
 * 电量提示状态阈值。
 *
 * 当前设备在约 2.87 V 时已经无法稳定供电，因此保护阈值必须明显
 * 高于该电压，以便在设备掉电前留出提示和省电处理空间。
 */
#define BATTERY_WARN_MV              3600
#define BATTERY_CRITICAL_MV          3450
#define BATTERY_PROTECT_MV           3350

#define BATTERY_WARN_PERCENT         20
#define BATTERY_CRITICAL_PERCENT     10
#define BATTERY_PROTECT_PERCENT      5

/*
 * 状态恢复阈值。
 *
 * 恢复条件高于低电告警条件，形成回差，
 * 避免电压或电量在阈值附近波动时频繁切换 UI 提示状态。
 */
#define BATTERY_RECOVER_MV           3650
#define BATTERY_RECOVER_PERCENT      25

/* -------------------------------------------------------------------------- */
/* Private macros: ADC configuration                                          */
/* -------------------------------------------------------------------------- */

/*
 * 当前硬件连接关系：
 *
 * BOARD_BAT_ADC_GPIO = GPIO34
 * GPIO34             = ADC1_CHANNEL_6
 *
 * @note
 * 电池采样必须优先使用 ADC1。
 * 在经典 ESP32 上，ADC2 会与 WiFi 使用存在冲突。
 *
 * 如果后续修改 BOARD_BAT_ADC_GPIO，需要同步确认 ADC channel。
 */
#define BATTERY_ADC_CHANNEL          ADC1_CHANNEL_6
#define BATTERY_ADC_WIDTH            ADC_WIDTH_BIT_12
#define BATTERY_ADC_ATTEN            ADC_ATTEN_DB_11

/**
 * @brief 每次电压测量的 ADC 采样次数。
 *
 * 多次采样后取平均值，用于降低瞬时噪声影响。
 */
#define BATTERY_SAMPLE_COUNT         16

/**
 * @brief 电池任务采样周期，单位 ms。
 */
#define BATTERY_TASK_PERIOD_MS       5000

/**
 * @brief ADC 默认参考电压，单位 mV。
 *
 * 如果芯片 eFuse 中没有可用校准值，则使用该默认参考电压。
 */
#define BATTERY_ADC_DEFAULT_VREF     1100

/* -------------------------------------------------------------------------- */
/* Private types                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 电池状态提示等级。
 */
typedef enum {
    BATTERY_POWER_NORMAL = 0,   /*!< 电量正常 */
    BATTERY_POWER_LOW,          /*!< 电量偏低 */
    BATTERY_POWER_CRITICAL,     /*!< 电量严重不足，建议充电 */
    BATTERY_POWER_PROTECT,      /*!< 电量过低，接近保护状态 */
} battery_power_state_t;

/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief ADC 校准特性数据。
 */
static esp_adc_cal_characteristics_t s_adc_chars;

/**
 * @brief 最近一次采样计算得到的电池电压，单位 mV。
 */
static uint32_t s_battery_voltage_mv = 0;

/**
 * @brief 最近一次计算得到的电池百分比。
 */
static uint8_t s_battery_percent = 0;

/**
 * @brief 当前电池提示状态。
 */
static battery_power_state_t s_power_state = BATTERY_POWER_NORMAL;

/**
 * @brief 是否已经因为低电量触发省电模式。
 */
static bool s_low_battery_power_save = false;

/**
 * @brief 电池监测任务是否已经启动。
 */
static bool s_battery_started = false;

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static uint8_t battery_voltage_to_percent(uint32_t mv);

static void battery_adc_print_calibration_info(esp_adc_cal_value_t val_type);
static esp_err_t battery_adc_init(void);
static esp_err_t battery_read_voltage_mv(uint32_t *out_mv);

static battery_power_state_t battery_calc_power_state(uint32_t mv,
                                                      uint8_t percent);
static void battery_apply_power_state(uint32_t mv, uint8_t percent);
static void battery_update_power_save(uint32_t mv, uint8_t percent);

static void battery_monitor_task(void *arg);

/* -------------------------------------------------------------------------- */
/* Battery percentage conversion                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 将电池电压换算为电量百分比。
 *
 * 当前采用空电电压到满电电压之间的线性换算。
 * 空电和满电阈值优先使用 app_settings 中的用户校准值；
 * 配置非法时回退到 app_config.h 中的默认值。
 *
 * @param mv 电池电压，单位 mV。
 *
 * @return 电池百分比，范围 0~100。
 *
 * @note
 * 锂电池真实放电曲线并非严格线性。
 * 当前算法适合作为基础估算，后续可以替换为分段曲线映射。
 */
static uint8_t battery_voltage_to_percent(uint32_t mv)
{
    const app_settings_t *cfg = app_settings_get();

    uint16_t empty_mv = BATTERY_PERCENT_EMPTY_MV;
    uint16_t full_mv = BATTERY_PERCENT_FULL_MV;

    if (cfg) {
        empty_mv = cfg->battery_empty_mv;
        full_mv = cfg->battery_full_mv;
    }

    if (full_mv <= empty_mv) {
        empty_mv = BATTERY_PERCENT_EMPTY_MV;
        full_mv = BATTERY_PERCENT_FULL_MV;
    }

    if (mv <= empty_mv) {
        return 0;
    }

    if (mv >= full_mv) {
        return 100;
    }

    uint32_t range = full_mv - empty_mv;
    uint32_t offset = mv - empty_mv;

    return (uint8_t)((offset * 100U) / range);
}

/* -------------------------------------------------------------------------- */
/* ADC initialization and voltage reading                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 输出 ADC 校准信息。
 *
 * @param val_type ADC 校准数据来源类型。
 */
static void battery_adc_print_calibration_info(esp_adc_cal_value_t val_type)
{
    switch (val_type) {
    case ESP_ADC_CAL_VAL_EFUSE_TP:
        ESP_LOGI(TAG, "ADC calibration: eFuse Two Point");
        break;

    case ESP_ADC_CAL_VAL_EFUSE_VREF:
        ESP_LOGI(TAG, "ADC calibration: eFuse Vref");
        break;

    case ESP_ADC_CAL_VAL_DEFAULT_VREF:
    default:
        ESP_LOGW(TAG,
                 "ADC calibration: Default Vref=%dmV",
                 BATTERY_ADC_DEFAULT_VREF);
        break;
    }
}

/**
 * @brief 初始化电池 ADC。
 *
 * 当前继续使用 legacy ADC API，避免改变现有 DAC/I2S 相关实现关系。
 *
 * @return ESP_OK 或 ADC 初始化错误码。
 */
static esp_err_t battery_adc_init(void)
{
    esp_err_t err = adc1_config_width(BATTERY_ADC_WIDTH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "adc1_config_width failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = adc1_config_channel_atten(BATTERY_ADC_CHANNEL,
                                    BATTERY_ADC_ATTEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "adc1_config_channel_atten failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
        ADC_UNIT_1,
        BATTERY_ADC_ATTEN,
        BATTERY_ADC_WIDTH,
        BATTERY_ADC_DEFAULT_VREF,
        &s_adc_chars
    );

    battery_adc_print_calibration_info(val_type);

    ESP_LOGI(TAG,
             "battery adc initialized, gpio=%d, adc1_channel=%d, atten=%d",
             BOARD_BAT_ADC_GPIO,
             BATTERY_ADC_CHANNEL,
             BATTERY_ADC_ATTEN);

    return ESP_OK;
}

/**
 * @brief 读取并计算电池实际电压。
 *
 * 测量流程：
 * 1. 连续读取 BATTERY_SAMPLE_COUNT 次 ADC；
 * 2. 计算原始采样平均值；
 * 3. 根据 ADC 校准结果换算 ADC 引脚电压；
 * 4. 根据电阻分压比例还原电池电压；
 * 5. 应用用户配置的电压偏移校准值。
 *
 * @param out_mv 输出电池电压，单位 mV。
 *
 * @return
 *      - ESP_OK：读取成功
 *      - ESP_ERR_INVALID_ARG：参数为空
 *      - ESP_FAIL：没有获得有效采样
 */
static esp_err_t battery_read_voltage_mv(uint32_t *out_mv)
{
    if (!out_mv) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t raw_sum = 0;
    uint32_t valid_count = 0;

    for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
        int raw = adc1_get_raw(BATTERY_ADC_CHANNEL);

        if (raw < 0) {
            ESP_LOGW(TAG, "adc1_get_raw failed, raw=%d", raw);
            continue;
        }

        raw_sum += (uint32_t)raw;
        valid_count++;

        /*
         * 在多次采样之间留少量间隔，降低瞬时采样噪声影响。
         */
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (valid_count == 0) {
        return ESP_FAIL;
    }

    uint32_t raw_avg = raw_sum / valid_count;

    /*
     * esp_adc_cal_raw_to_voltage() 返回 ADC 引脚处电压，单位 mV。
     */
    uint32_t adc_mv = esp_adc_cal_raw_to_voltage(raw_avg, &s_adc_chars);

    /*
     * 根据电池分压比例还原实际电池电压。
     *
     * BATTERY_VOLTAGE_SCALE_PERMILLE 示例：
     * - 1000 = 1.000 倍，不放大
     * - 2000 = 2.000 倍，适用于约 1:1 电阻分压
     * - 3100 = 3.100 倍，适用于对应比例分压电路
     *
     * 当前 app_config.h 配置为 2000，即按 2.000 倍还原。
     */
    int32_t corrected_mv =
        (int32_t)((adc_mv * BATTERY_VOLTAGE_SCALE_PERMILLE) / 1000U);

    const app_settings_t *cfg = app_settings_get();

    if (cfg) {
        corrected_mv += cfg->battery_offset_mv;
    } else {
        corrected_mv += BATTERY_VOLTAGE_OFFSET_MV;
    }

    if (corrected_mv < 0) {
        corrected_mv = 0;
    }

    *out_mv = (uint32_t)corrected_mv;

    ESP_LOGI(TAG,
             "battery raw=%u, adc_mv=%u, corrected_mv=%u",
             (unsigned)raw_avg,
             (unsigned)adc_mv,
             (unsigned)*out_mv);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Battery state and low-power integration                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 根据电池电压和百分比计算提示状态。
 *
 * 电压或百分比任一达到阈值，即进入对应低电状态。
 * 恢复为正常状态时，要求同时满足恢复电压和恢复百分比，
 * 以形成回差并避免界面提示反复跳变。
 *
 * @param mv 电池电压，单位 mV。
 * @param percent 电池百分比。
 *
 * @return 新的电池提示状态。
 */
static battery_power_state_t battery_calc_power_state(uint32_t mv,
                                                      uint8_t percent)
{
    if (mv <= BATTERY_PROTECT_MV ||
        percent <= BATTERY_PROTECT_PERCENT) {
        return BATTERY_POWER_PROTECT;
    }

    if (mv <= BATTERY_CRITICAL_MV ||
        percent <= BATTERY_CRITICAL_PERCENT) {
        return BATTERY_POWER_CRITICAL;
    }

    if (mv <= BATTERY_WARN_MV ||
        percent <= BATTERY_WARN_PERCENT) {
        return BATTERY_POWER_LOW;
    }

    if (mv >= BATTERY_RECOVER_MV &&
        percent >= BATTERY_RECOVER_PERCENT) {
        return BATTERY_POWER_NORMAL;
    }

    /*
     * 处于恢复回差区域时保持原状态，避免状态抖动。
     */
    return s_power_state;
}

/**
 * @brief 根据电池状态变化更新 UI 提示。
 *
 * @param mv 电池电压，单位 mV。
 * @param percent 电池百分比。
 */
static void battery_apply_power_state(uint32_t mv, uint8_t percent)
{
    battery_power_state_t new_state =
        battery_calc_power_state(mv, percent);

    if (new_state == s_power_state) {
        return;
    }

    ESP_LOGW(TAG,
             "battery power state changed: %d -> %d, mv=%u, percent=%u",
             (int)s_power_state,
             (int)new_state,
             (unsigned)mv,
             (unsigned)percent);

    s_power_state = new_state;

    switch (s_power_state) {
    case BATTERY_POWER_NORMAL:
        ui_async_update_status("电量正常");
        break;

    case BATTERY_POWER_LOW:
        ui_async_update_status("电量低");
        break;

    case BATTERY_POWER_CRITICAL:
        ui_async_update_status("请充电");
        break;

    case BATTERY_POWER_PROTECT:
        ui_async_update_status("电量过低");
        break;

    default:
        break;
    }
}

/**
 * @brief 根据低电阈值联动省电管理模块。
 *
 * 进入规则：
 * - 百分比低于或等于 APP_POWER_SAVE_ENTER_PERCENT；
 * - 或电压低于或等于 APP_POWER_SAVE_ENTER_MV。
 *
 * 退出规则：
 * - 百分比恢复到 APP_POWER_SAVE_EXIT_PERCENT 以上；
 * - 并且电压恢复到 APP_POWER_SAVE_EXIT_MV 以上。
 *
 * @param mv 电池电压，单位 mV。
 * @param percent 电池百分比。
 */
static void battery_update_power_save(uint32_t mv, uint8_t percent)
{
    if (!s_low_battery_power_save) {
        if (percent <= APP_POWER_SAVE_ENTER_PERCENT ||
            mv <= APP_POWER_SAVE_ENTER_MV) {

            s_low_battery_power_save = true;

            ESP_LOGW(TAG,
                     "enter low battery power save, mv=%u, percent=%u",
                     (unsigned)mv,
                     (unsigned)percent);

            app_power_save_set_reason(APP_POWER_SAVE_REASON_LOW_BATTERY,
                                      true);
        }

        return;
    }

    /*
     * 退出低电量省电模式时要求电压和百分比都恢复，
     * 避免在临界值附近反复启停省电模式。
     */
    if (percent >= APP_POWER_SAVE_EXIT_PERCENT &&
        mv >= APP_POWER_SAVE_EXIT_MV) {

        s_low_battery_power_save = false;

        ESP_LOGW(TAG,
                 "exit low battery power save, mv=%u, percent=%u",
                 (unsigned)mv,
                 (unsigned)percent);

        app_power_save_set_reason(APP_POWER_SAVE_REASON_LOW_BATTERY,
                                  false);
    }
}

/* -------------------------------------------------------------------------- */
/* Battery monitor task                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief 电池监测后台任务。
 *
 * 周期性执行：
 * - 电池采样；
 * - 电量换算；
 * - UI 更新；
 * - 低电状态更新；
 * - 低电省电控制。
 */
static void battery_monitor_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "battery monitor task started");

    while (true) {
        uint32_t mv = 0;

        esp_err_t err = battery_read_voltage_mv(&mv);

        if (err == ESP_OK) {
            s_battery_voltage_mv = mv;
            s_battery_percent = battery_voltage_to_percent(mv);

            /*
             * 当前没有硬件充电状态检测能力。
             * 因此按未充电状态更新 UI，确保低电颜色能够正常显示。
             */
            ui_async_update_battery(s_battery_percent, false);

            /*
             * 更新低电状态提示。
             */
            battery_apply_power_state(s_battery_voltage_mv,
                                      s_battery_percent);

            /*
             * 更新低电量省电模式。
             */
            battery_update_power_save(s_battery_voltage_mv,
                                      s_battery_percent);

            ESP_LOGI(TAG,
                     "battery=%umV, percent=%u%%, state=%d",
                     (unsigned)s_battery_voltage_mv,
                     (unsigned)s_battery_percent,
                     (int)s_power_state);
        } else {
            ESP_LOGW(TAG,
                     "battery read failed: %s",
                     esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(BATTERY_TASK_PERIOD_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* Public interfaces                                                          */
/* -------------------------------------------------------------------------- */

esp_err_t battery_monitor_start(void)
{
    if (s_battery_started) {
        ESP_LOGW(TAG, "battery monitor already started");
        return ESP_OK;
    }

    esp_err_t err = battery_adc_init();
    if (err != ESP_OK) {
        return err;
    }

    /*
     * 电池采样任务优先级无需过高，
     * 避免影响 Audio、WiFi 和 LVGL 等实时性更高的模块。
     */
    BaseType_t ret = xTaskCreate(battery_monitor_task,
                                 "battery_monitor",
                                 3072,
                                 NULL,
                                 2,
                                 NULL);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create battery task failed");
        return ESP_ERR_NO_MEM;
    }

    s_battery_started = true;

    ESP_LOGI(TAG, "battery monitor started");

    return ESP_OK;
}

uint32_t battery_monitor_get_voltage_mv(void)
{
    return s_battery_voltage_mv;
}

uint8_t battery_monitor_get_percent(void)
{
    return s_battery_percent;
}
