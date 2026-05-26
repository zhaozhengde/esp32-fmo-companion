/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file board_config.h
 * @brief 当前硬件板级引脚与外设参数配置。
 *
 * 本文件用于描述当前硬件板子的 GPIO 分配、屏幕分辨率、
 * 触摸校准参数、音频控制引脚、SD 卡 SPI 引脚和电池 ADC 引脚。
 *
 * @note
 * 本文件只放“硬件板级配置”。
 * 应用默认值、功能开关、WebSocket、音频缓冲、省电阈值等配置
 * 应放在 app_config.h。
 *
 * @warning
 * 修改本文件前请确认实际硬件原理图。
 * GPIO 冲突会导致 LCD、触摸、SD、音频或电池检测异常。
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* LCD: ST7789                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief LCD 水平分辨率。
 */
#define BOARD_LCD_H_RES             320

/**
 * @brief LCD 垂直分辨率。
 */
#define BOARD_LCD_V_RES             240

/**
 * @brief LCD SPI CS 引脚。
 */
#define BOARD_LCD_CS_GPIO           GPIO_NUM_15

/**
 * @brief LCD 数据/命令选择引脚。
 */
#define BOARD_LCD_DC_GPIO           GPIO_NUM_2

/**
 * @brief LCD SPI MOSI 引脚。
 */
#define BOARD_LCD_MOSI_GPIO         GPIO_NUM_13

/**
 * @brief LCD SPI MISO 引脚。
 *
 * 对于只写 LCD，该引脚可能不使用。
 */
#define BOARD_LCD_MISO_GPIO         GPIO_NUM_12

/**
 * @brief LCD SPI SCLK 引脚。
 */
#define BOARD_LCD_SCLK_GPIO         GPIO_NUM_14

/**
 * @brief LCD 背光控制引脚。
 *
 * app_ui.c 中会使用 LEDC PWM 控制该引脚。
 */
#define BOARD_LCD_BL_GPIO           GPIO_NUM_21

/**
 * @brief LCD 复位引脚。
 *
 * GPIO_NUM_NC 表示当前硬件未连接独立复位脚。
 */
#define BOARD_LCD_RST_GPIO          GPIO_NUM_NC

/**
 * @brief LCD 背光点亮电平。
 *
 * 1：高电平点亮
 * 0：低电平点亮
 */
#define BOARD_LCD_BL_ON_LEVEL       1

/**
 * @brief LCD 背光关闭电平。
 *
 * 1：高电平关闭
 * 0：低电平关闭
 */
#define BOARD_LCD_BL_OFF_LEVEL      0

/* -------------------------------------------------------------------------- */
/* Touch: XPT2046                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief XPT2046 触摸 SPI CS 引脚。
 */
#define BOARD_TOUCH_CS_GPIO         GPIO_NUM_33

/**
 * @brief XPT2046 触摸 SPI CLK 引脚。
 */
#define BOARD_TOUCH_CLK_GPIO        GPIO_NUM_25

/**
 * @brief XPT2046 触摸 SPI MOSI 引脚。
 */
#define BOARD_TOUCH_MOSI_GPIO       GPIO_NUM_32

/**
 * @brief XPT2046 触摸 SPI MISO 引脚。
 *
 * @note
 * GPIO39 是 ESP32 输入专用 GPIO，适合作为 MISO。
 */
#define BOARD_TOUCH_MISO_GPIO       GPIO_NUM_39

/**
 * @brief XPT2046 触摸中断引脚。
 *
 * @note
 * GPIO36 是 ESP32 输入专用 GPIO，适合作为 IRQ 输入。
 */
#define BOARD_TOUCH_IRQ_GPIO        GPIO_NUM_36

/**
 * @brief 触摸中断有效电平。
 *
 * 0：低电平有效
 * 1：高电平有效
 */
#define BOARD_TOUCH_IRQ_ACTIVE      0

/**
 * @brief 触摸原始 X 最小值。
 *
 * 该值来自触摸校准结果。
 */
#define BOARD_TOUCH_X_MIN           495

/**
 * @brief 触摸原始 X 最大值。
 *
 * 该值来自触摸校准结果。
 */
#define BOARD_TOUCH_X_MAX           3398

/**
 * @brief 触摸原始 Y 最小值。
 *
 * 该值来自触摸校准结果。
 */
#define BOARD_TOUCH_Y_MIN           721

/**
 * @brief 触摸原始 Y 最大值。
 *
 * 该值来自触摸校准结果。
 */
#define BOARD_TOUCH_Y_MAX           3448

/* -------------------------------------------------------------------------- */
/* Audio output                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief ESP32 内置 DAC 音频输出引脚。
 *
 * GPIO26 对应 ESP32 DAC 通道 2。
 */
#define BOARD_AUDIO_DAC_GPIO        GPIO_NUM_26

/**
 * @brief 音频功放使能/静音控制引脚。
 */
#define BOARD_AUDIO_EN_GPIO         GPIO_NUM_4

/**
 * @brief 音频功放使能有效电平。
 *
 * 0：低电平使能
 * 1：高电平使能
 */
#define BOARD_AUDIO_EN_ACTIVE       0

/**
 * @brief 音频功放静音有效电平。
 *
 * 0：低电平静音
 * 1：高电平静音
 */
#define BOARD_AUDIO_MUTE_ACTIVE     1

/* -------------------------------------------------------------------------- */
/* SD card                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief SD 卡 SPI CS 引脚。
 */
#define BOARD_SD_CS_GPIO            GPIO_NUM_5

/**
 * @brief SD 卡 SPI SCLK 引脚。
 */
#define BOARD_SD_SCLK_GPIO          GPIO_NUM_18

/**
 * @brief SD 卡 SPI MISO 引脚。
 */
#define BOARD_SD_MISO_GPIO          GPIO_NUM_19

/**
 * @brief SD 卡 SPI MOSI 引脚。
 */
#define BOARD_SD_MOSI_GPIO          GPIO_NUM_23

/* -------------------------------------------------------------------------- */
/* Battery ADC                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 电池电压 ADC 输入引脚。
 *
 * @note
 * GPIO34 是 ESP32 输入专用 GPIO，适合作为 ADC 输入。
 * 实际电池电压需要结合分压电阻和 app_config.h 中的
 * BATTERY_VOLTAGE_SCALE_PERMILLE 计算。
 */
#define BOARD_BAT_ADC_GPIO          GPIO_NUM_34

#ifdef __cplusplus
}
#endif

#endif /* BOARD_CONFIG_H */
