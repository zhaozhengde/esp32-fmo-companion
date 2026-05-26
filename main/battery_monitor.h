/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file battery_monitor.h
 * @brief 电池电压采样与电量监测接口。
 *
 * 本模块负责：
 * - 初始化电池 ADC 采样；
 * - 周期读取电池电压；
 * - 根据配置中的校准参数计算实际电压；
 * - 估算电池百分比；
 * - 更新 UI 电池状态；
 * - 在低电量时联动省电管理模块。
 */

#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动电池监测任务。
 *
 * 该函数会初始化 ADC，并创建后台电池采样任务。
 * 重复调用不会重复创建任务。
 *
 * @return
 *      - ESP_OK：启动成功，或模块已经启动
 *      - ESP_ERR_NO_MEM：任务创建失败
 *      - 其他值：ADC 初始化失败
 */
esp_err_t battery_monitor_start(void);

/**
 * @brief 获取最近一次测得的电池电压。
 *
 * @return 电池电压，单位 mV。
 *
 * @note
 * 在首次采样完成前，返回值可能为 0。
 */
uint32_t battery_monitor_get_voltage_mv(void);

/**
 * @brief 获取最近一次计算出的电池百分比。
 *
 * @return 电池电量百分比，范围 0~100。
 *
 * @note
 * 在首次采样完成前，返回值可能为 0。
 */
uint8_t battery_monitor_get_percent(void);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_MONITOR_H */
