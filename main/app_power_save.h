/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file app_power_save.h
 * @brief 应用省电模式管理接口。
 *
 * 本模块使用“原因位图”的方式管理省电状态。
 *
 * 只要存在任意省电原因，系统即进入省电模式；
 * 当所有省电原因都被清除后，系统退出省电模式。
 *
 * 当前支持的省电原因：
 * - 手动省电；
 * - 低电量省电。
 */

#ifndef APP_POWER_SAVE_H
#define APP_POWER_SAVE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Public types                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 省电模式触发原因。
 *
 * 该枚举按 bitmask 使用，可组合多个原因。
 */
typedef enum {
    /**
     * @brief 用户手动开启省电模式。
     */
    APP_POWER_SAVE_REASON_MANUAL      = 1U << 0,

    /**
     * @brief 电池电量过低触发省电模式。
     */
    APP_POWER_SAVE_REASON_LOW_BATTERY = 1U << 1,
} app_power_save_reason_t;

/* -------------------------------------------------------------------------- */
/* Public interfaces                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化省电管理模块。
 *
 * 清空所有省电原因，并将内部状态重置为未省电。
 */
void app_power_save_init(void);

/**
 * @brief 设置或清除一个省电原因。
 *
 * 当设置任意原因后，系统会进入省电模式；
 * 当所有原因都被清除后，系统会退出省电模式。
 *
 * @param reason 省电原因。
 * @param enable true 表示设置该原因，false 表示清除该原因。
 */
void app_power_save_set_reason(app_power_save_reason_t reason, bool enable);

/**
 * @brief 切换手动省电状态。
 *
 * 如果当前已手动开启省电，则关闭手动省电原因；
 * 如果当前未手动开启省电，则设置手动省电原因。
 */
void app_power_save_toggle_manual(void);

/**
 * @brief 判断当前是否处于省电模式。
 *
 * @return
 *      - true：处于省电模式
 *      - false：未处于省电模式
 */
bool app_power_save_is_active(void);

/**
 * @brief 判断手动省电是否已开启。
 *
 * @return
 *      - true：手动省电原因存在
 *      - false：手动省电原因不存在
 */
bool app_power_save_is_manual_enabled(void);

/**
 * @brief 判断指定省电原因是否存在。
 *
 * @param reason 省电原因。
 *
 * @return
 *      - true：该原因存在
 *      - false：该原因不存在
 */
bool app_power_save_has_reason(app_power_save_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* APP_POWER_SAVE_H */
