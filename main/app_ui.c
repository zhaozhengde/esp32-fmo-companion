/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file app_ui.c
 * @brief 应用主界面、设置页、待机时钟页及 UI 更新接口实现。
 *
 * 本文件负责创建和维护设备主界面，包括：
 * - 当前呼号、上次通联、当前站点和 QSO 数量显示；
 * - WiFi、电池、日期时间等状态显示；
 * - 设置页及其子页面；
 * - WiFi 扫描页面；
 * - 站点选择弹窗；
 * - QSO 同步弹窗；
 * - 普通待机时钟页和省电时钟页；
 * - LCD 背光 PWM 控制。
 *
 * @note
 * LVGL 对象通常只能在 LVGL 上下文中安全访问。
 * 来自 WiFi、WebSocket、电池监测等后台任务的 UI 更新，
 * 建议通过 ui_async.c 转发到 LVGL 上下文后再调用本模块接口。
 */

#include "app_ui.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* FreeRTOS headers --------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF headers ---------------------------------------------------------- */
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

/* LVGL headers ------------------------------------------------------------- */
#include "lvgl.h"
#include "misc/lv_txt.h"

/* Project headers ---------------------------------------------------------- */
#include "app_config.h"
#include "app_power_save.h"
#include "app_settings.h"
#include "audio_ws.h"
#include "board_config.h"
#include "station_parser.h"
#include "ui_icons.h"
#include "wifi_manager.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "app_ui";

/* -------------------------------------------------------------------------- */
/* Font declarations                                                          */
/* -------------------------------------------------------------------------- */

/*
 * 字体资源由 fonts 目录中的 LVGL 字体文件提供。
 *
 * font20      ：常规中文显示字体
 * EN_BIG      ：主界面当前呼号大字体
 * status_font ：状态栏和小按钮字体
 * font28      ：上次通联呼号字体
 * clock_font  ：待机时钟呼号字体
 */
LV_FONT_DECLARE(font20);
LV_FONT_DECLARE(EN_BIG);
LV_FONT_DECLARE(status_font);
LV_FONT_DECLARE(font28);
LV_FONT_DECLARE(clock_font);

/* -------------------------------------------------------------------------- */
/* Private macros: colors                                                     */
/* -------------------------------------------------------------------------- */

#define UI_COLOR_BG              lv_color_hex(0x000000)
#define UI_COLOR_PANEL           lv_color_hex(0x101010)
#define UI_COLOR_ORANGE          lv_color_hex(0xFF8800)
#define UI_COLOR_ORANGE_DARK     lv_color_hex(0x9A5200)
#define UI_COLOR_WHITE           lv_color_hex(0xFFFFFF)
#define UI_COLOR_GRAY            lv_color_hex(0xE7EAEE)
#define UI_COLOR_DARK_GRAY       lv_color_hex(0x202020)
#define UI_COLOR_GREEN           lv_color_hex(0x32D74B)
#define UI_COLOR_RED             lv_color_hex(0xFF453A)

/* -------------------------------------------------------------------------- */
/* Private macros: backlight PWM                                              */
/* -------------------------------------------------------------------------- */

#define APP_UI_BL_LEDC_TIMER     LEDC_TIMER_0
#define APP_UI_BL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define APP_UI_BL_LEDC_CHANNEL   LEDC_CHANNEL_0
#define APP_UI_BL_LEDC_DUTY_RES  LEDC_TIMER_10_BIT
#define APP_UI_BL_LEDC_FREQ_HZ   5000

#define APP_UI_BL_DUTY_MAX       ((1U << 10) - 1U)

/* -------------------------------------------------------------------------- */
/* Private macros: station list                                               */
/* -------------------------------------------------------------------------- */

/*
 * 每页最多显示 6 个站点。
 * 请求时多请求 1 个，用于判断是否存在下一页。
 */
#define STATION_MENU_PAGE_SIZE   6
#define STATION_MENU_FETCH_COUNT (STATION_MENU_PAGE_SIZE + 1)

/* -------------------------------------------------------------------------- */
/* Private macros: WiFi scan                                                  */
/* -------------------------------------------------------------------------- */

#define WIFI_SCAN_MENU_MAX_ITEMS 6

/* -------------------------------------------------------------------------- */
/* Private macros: idle clock                                                 */
/* -------------------------------------------------------------------------- */

#define APP_UI_IDLE_TIMER_PERIOD_MS  1000

/*
 * 默认无操作 30 秒进入待机时钟页。
 * 如果 app_settings 中 idle_image_enabled 为 true，
 * 则优先使用 cfg->idle_image_timeout_ms。
 */
#define APP_UI_IDLE_CLOCK_DEFAULT_MS 30000

/* -------------------------------------------------------------------------- */
/* Private variables: common UI state                                         */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_root = NULL;

/*
 * 当前用户或系统要求的正常背光值。
 * 例如：
 * - 设置页保存 80%
 * - 低电保护限制到 30%
 */
static uint8_t s_backlight_target_percent = 100;

/*
 * 当前实际输出到 PWM 的背光值。
 */
static uint8_t s_backlight_applied_percent = 100;

static bool s_backlight_pwm_inited = false;

/*
 * 当前电池和 WiFi 状态缓存。
 * 这些值主要用于 UI 颜色刷新和后续扩展。
 */
static uint8_t s_battery_percent = 0;
static bool s_battery_charging = false;
static int s_wifi_rssi = -127;

/* -------------------------------------------------------------------------- */
/* Private variables: main screen                                             */
/* -------------------------------------------------------------------------- */

/* 上部区域：当前呼号 */
static lv_obj_t *s_top_area = NULL;
static lv_obj_t *s_label_current_call = NULL;

/*
 * 当前呼号下方信息行：
 * - 左侧：上次通联
 * - 右侧：QSO 数量
 */
static lv_obj_t *s_info_row_area = NULL;
static lv_obj_t *s_label_last_call = NULL;
static lv_obj_t *s_img_qso_icon = NULL;
static lv_obj_t *s_label_qso_count = NULL;

/* 橙色站点条 */
static lv_obj_t *s_card_area = NULL;

/*
 * 当前站点显示区域。
 * s_station_text_box 固定高度，s_label_station 居中显示。
 */
static lv_obj_t *s_station_text_box = NULL;
static lv_obj_t *s_label_station = NULL;

/*
 * 右上角状态区域：
 * 日期时间 + WiFi + 电池。
 */
static lv_obj_t *s_top_status_group = NULL;
static lv_obj_t *s_label_datetime = NULL;
static lv_obj_t *s_img_wifi_icon = NULL;
static lv_obj_t *s_img_battery_icon = NULL;

/*
 * 左上角 speaking 状态胶囊。
 */
static lv_obj_t *s_speaking_pill = NULL;
static lv_obj_t *s_label_speaking = NULL;

/* 底部区域 */
static lv_obj_t *s_bottom_area = NULL;
static lv_obj_t *s_btn_power_save = NULL;
static lv_obj_t *s_img_power_save_icon = NULL;

static lv_obj_t *s_btn_mute = NULL;
static lv_obj_t *s_img_mute_icon = NULL;

static lv_obj_t *s_btn_settings = NULL;
static lv_obj_t *s_img_settings_icon = NULL;
static lv_obj_t *s_label_bottom_status = NULL;

/*
 * 新静音逻辑：
 * true  = Audio WS 关闭
 * false = Audio WS 开启
 *
 * 开机默认静音。
 */
static bool s_audio_muted = true;
static uint8_t s_audio_volume_before_mute = DEFAULT_AUDIO_VOLUME;


/* -------------------------------------------------------------------------- */
/* Private variables: settings page                                           */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_settings_page = NULL;
static lv_obj_t *s_label_status = NULL;

/* 设置页首页与子页面 */
static lv_obj_t *s_settings_home = NULL;
static lv_obj_t *s_settings_fmo_page = NULL;
static lv_obj_t *s_settings_callsign_page = NULL;
static lv_obj_t *s_settings_volume_page = NULL;
static lv_obj_t *s_settings_backlight_page = NULL;
static lv_obj_t *s_settings_wifi_page = NULL;
static lv_obj_t *s_settings_wifi_scan_page = NULL;
static lv_obj_t *s_settings_battery_page = NULL;
static lv_obj_t *s_settings_station_page = NULL;

/* 设置首页当前值显示 */
static lv_obj_t *s_label_setting_fmo_value = NULL;
static lv_obj_t *s_label_setting_callsign_value = NULL;
static lv_obj_t *s_label_setting_volume_value = NULL;
static lv_obj_t *s_label_setting_backlight_value = NULL;
static lv_obj_t *s_label_setting_rotate_value = NULL;
static lv_obj_t *s_label_setting_wifi_value = NULL;
static lv_obj_t *s_label_setting_battery_value = NULL;
static lv_obj_t *s_label_setting_station_value = NULL;
static lv_obj_t *s_label_setting_power_save_value = NULL;
static lv_obj_t *s_label_setting_qso_value = NULL;

/* FMO 地址输入 */
static lv_obj_t *s_ta_fmo_host = NULL;

/* 本机呼号输入 */
static lv_obj_t *s_ta_owner_callsign = NULL;

/* 音量设置 */
static lv_obj_t *s_slider_volume = NULL;
static lv_obj_t *s_label_volume_value = NULL;

/* 背光设置 */
static lv_obj_t *s_slider_backlight = NULL;
static lv_obj_t *s_label_backlight_value = NULL;

/* WiFi 设置 */
static lv_obj_t *s_ta_wifi_ssid = NULL;
static lv_obj_t *s_ta_wifi_password = NULL;

/* 电量校准 */
static lv_obj_t *s_ta_battery_empty = NULL;
static lv_obj_t *s_ta_battery_full = NULL;
static lv_obj_t *s_ta_battery_offset = NULL;

/* 设置页键盘 */
static lv_obj_t *s_settings_keyboard = NULL;

/* -------------------------------------------------------------------------- */
/* Private variables: settings station list                                   */
/* -------------------------------------------------------------------------- */

static int s_station_menu_page = 0;
static bool s_station_menu_has_next = false;

static lv_obj_t *s_station_item_btns[STATION_MENU_PAGE_SIZE];
static lv_obj_t *s_station_item_labels[STATION_MENU_PAGE_SIZE];
static int s_station_item_uids[STATION_MENU_PAGE_SIZE];

static lv_obj_t *s_label_station_page_status = NULL;
static lv_obj_t *s_station_btn_prev = NULL;
static lv_obj_t *s_station_btn_next = NULL;

/* -------------------------------------------------------------------------- */
/* Private variables: main station popup                                      */
/* -------------------------------------------------------------------------- */

/*
 * 主界面收藏站点弹窗。
 */
static lv_obj_t *s_main_station_popup = NULL;
static lv_obj_t *s_label_main_station_popup_status = NULL;

static int s_main_station_popup_page = 0;
static bool s_main_station_popup_has_next = false;

static lv_obj_t *s_main_station_item_btns[STATION_MENU_PAGE_SIZE];
static lv_obj_t *s_main_station_item_labels[STATION_MENU_PAGE_SIZE];
static int s_main_station_item_uids[STATION_MENU_PAGE_SIZE];

static lv_obj_t *s_main_station_btn_prev = NULL;
static lv_obj_t *s_main_station_btn_next = NULL;

/* -------------------------------------------------------------------------- */
/* Private variables: QSO sync popup                                          */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_qso_sync_popup = NULL;
static lv_obj_t *s_label_qso_sync_popup = NULL;
static lv_timer_t *s_qso_sync_popup_timer = NULL;

/* -------------------------------------------------------------------------- */
/* Private variables: WiFi scan                                               */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_wifi_scan_item_btns[WIFI_SCAN_MENU_MAX_ITEMS];
static lv_obj_t *s_wifi_scan_item_labels[WIFI_SCAN_MENU_MAX_ITEMS];

static wifi_scan_item_t s_wifi_scan_items[WIFI_SCAN_MENU_MAX_ITEMS];
static int s_wifi_scan_count = 0;

static lv_obj_t *s_label_wifi_scan_status = NULL;

/*
 * WiFi 异步扫描状态。
 *
 * 后台 FreeRTOS 任务只负责扫描并写入结果；
 * LVGL timer 在 UI 上下文中读取结果并刷新页面。
 */
static volatile bool s_wifi_scan_in_progress = false;
static volatile bool s_wifi_scan_result_ready = false;
static esp_err_t s_wifi_scan_result = ESP_OK;

static TaskHandle_t s_wifi_scan_task_handle = NULL;
static lv_timer_t *s_wifi_scan_poll_timer = NULL;

/* -------------------------------------------------------------------------- */
/* Private variables: idle clock and power-save clock                         */
/* -------------------------------------------------------------------------- */

static lv_timer_t *s_idle_timer = NULL;
static lv_timer_t *s_idle_clock_timer = NULL;

static bool s_idle_clock_active = false;
static bool s_power_save_clock_active = false;

static lv_obj_t *s_idle_clock_page = NULL;

static lv_obj_t *s_idle_clock_top_area = NULL;
static lv_obj_t *s_idle_clock_bottom_area = NULL;

static lv_obj_t *s_label_idle_clock_callsign = NULL;
static lv_obj_t *s_label_idle_clock_time = NULL;

/* -------------------------------------------------------------------------- */
/* Private function declarations: utility                                     */
/* -------------------------------------------------------------------------- */

static const lv_font_t *ui_font_cn(void);
static const lv_font_t *ui_font_EN_BIG(void);
static const lv_font_t *ui_font_status(void);
static const lv_font_t *ui_font_28(void);
static const lv_font_t *ui_font_clock(void);

static void label_set_color(lv_obj_t *label, lv_color_t color);
static void img_set_color(lv_obj_t *img, lv_color_t color);
static void label_set_font(lv_obj_t *label, const lv_font_t *font);
static void make_clean_obj(lv_obj_t *obj, lv_color_t bg_color, lv_opa_t opa);
static void set_label_text_safe(lv_obj_t *label,
                                const char *text,
                                const char *fallback);

static void label_apply_dynamic_letter_space(lv_obj_t *label,
                                             const char *text,
                                             const lv_font_t *font,
                                             lv_coord_t target_width,
                                             int min_letter_space,
                                             int max_letter_space,
                                             int default_letter_space);

static int settings_text_to_int(const char *s, int def_val);

/* -------------------------------------------------------------------------- */
/* Private function declarations: backlight and screen                         */
/* -------------------------------------------------------------------------- */

static esp_err_t app_ui_backlight_pwm_init_once(void);
static void app_ui_backlight_apply_raw(uint8_t percent);
static void app_ui_apply_screen_rotation(bool rotate_180);

/* -------------------------------------------------------------------------- */
/* Private function declarations: date and time                               */
/* -------------------------------------------------------------------------- */

static void app_ui_format_clock_text(char *buf, size_t buf_size);
static void app_ui_format_datetime_text(char *buf, size_t buf_size);
static void app_ui_datetime_update(void);
static void app_ui_datetime_timer_cb(lv_timer_t *timer);

/* -------------------------------------------------------------------------- */
/* Private function declarations: main screen                                 */
/* -------------------------------------------------------------------------- */

static void create_top_status_area(lv_obj_t *parent);
static void create_speaking_pill(lv_obj_t *parent);
static void create_top_area(lv_obj_t *parent);
static void create_info_row_area(lv_obj_t *parent);
static void create_card_area(lv_obj_t *parent);
static void create_bottom_area(lv_obj_t *parent);

static void current_call_apply_fit_style(const char *text);
static void idle_clock_callsign_apply_fit_style(const char *text);
static int wifi_rssi_to_percent(int rssi);

static void mute_button_event_cb(lv_event_t *e);
static void mute_button_update_style(void);

static void power_save_button_event_cb(lv_event_t *e);
static void power_save_button_update_style(void);

/* 当前版本已取消顶部音频电平条，该函数声明保留用于兼容预留。 */
static void voice_level_timer_cb(lv_timer_t *timer);

/* -------------------------------------------------------------------------- */
/* Private function declarations: settings page                               */
/* -------------------------------------------------------------------------- */

static void settings_refresh_home_values(void);
static void settings_hide_keyboard(void);
static void settings_show_home(void);

static lv_obj_t *settings_create_row(lv_obj_t *parent,
                                     const char *name,
                                     const char *value,
                                     lv_event_cb_t cb);

static lv_obj_t *settings_create_action_button(lv_obj_t *parent,
                                               const char *text,
                                               lv_color_t bg,
                                               lv_color_t fg,
                                               lv_event_cb_t cb);

static void settings_open_event_cb(lv_event_t *e);
static void settings_close_event_cb(lv_event_t *e);
static void settings_back_home_event_cb(lv_event_t *e);
static void settings_restart_event_cb(lv_event_t *e);

static void settings_keyboard_event_cb(lv_event_t *e);
static void settings_ta_text_focus_event_cb(lv_event_t *e);
static void settings_ta_number_focus_event_cb(lv_event_t *e);

static void settings_fmo_open_event_cb(lv_event_t *e);
static void settings_fmo_save_event_cb(lv_event_t *e);

static void settings_callsign_open_event_cb(lv_event_t *e);
static void settings_callsign_save_event_cb(lv_event_t *e);

static void settings_volume_open_event_cb(lv_event_t *e);
static void settings_volume_slider_event_cb(lv_event_t *e);
static void settings_volume_save_event_cb(lv_event_t *e);

static void settings_backlight_open_event_cb(lv_event_t *e);
static void settings_backlight_slider_event_cb(lv_event_t *e);
static void settings_backlight_save_event_cb(lv_event_t *e);

static void settings_rotate_toggle_event_cb(lv_event_t *e);

static void settings_power_save_toggle_event_cb(lv_event_t *e);
static void settings_qso_sync_event_cb(lv_event_t *e);

static void settings_wifi_open_event_cb(lv_event_t *e);
static void settings_wifi_save_event_cb(lv_event_t *e);

static void settings_battery_open_event_cb(lv_event_t *e);
static void settings_battery_save_event_cb(lv_event_t *e);

static void settings_station_open_event_cb(lv_event_t *e);
static void settings_station_prev_event_cb(lv_event_t *e);
static void settings_station_next_event_cb(lv_event_t *e);
static void settings_station_refresh_event_cb(lv_event_t *e);
static void settings_station_item_event_cb(lv_event_t *e);
static void settings_station_get_current_delay_cb(lv_timer_t *timer);

static void settings_station_set_button_enabled(lv_obj_t *btn, bool enabled);
static void settings_station_request_page(int page);
static void settings_station_list_render(void);

/* -------------------------------------------------------------------------- */
/* Private function declarations: WiFi scan page                              */
/* -------------------------------------------------------------------------- */

static void settings_wifi_scan_open_event_cb(lv_event_t *e);
static void settings_wifi_scan_item_event_cb(lv_event_t *e);
static void settings_wifi_scan_back_event_cb(lv_event_t *e);
static void settings_wifi_scan_poll_timer_cb(lv_timer_t *timer);
static void settings_wifi_scan_task(void *arg);
static void settings_wifi_scan_render(void);

/* -------------------------------------------------------------------------- */
/* Private function declarations: station popup and QSO popup                 */
/* -------------------------------------------------------------------------- */

static void create_main_station_popup(lv_obj_t *parent);
static void create_qso_sync_popup(lv_obj_t *parent);
static void qso_sync_popup_auto_close_cb(lv_timer_t *timer);

static void main_station_popup_open_event_cb(lv_event_t *e);
static void main_station_popup_close_event_cb(lv_event_t *e);
static void main_station_popup_prev_event_cb(lv_event_t *e);
static void main_station_popup_next_event_cb(lv_event_t *e);
static void main_station_popup_refresh_event_cb(lv_event_t *e);
static void main_station_popup_item_event_cb(lv_event_t *e);

static void main_station_popup_request_page(int page);
static void main_station_popup_render(void);

/* -------------------------------------------------------------------------- */
/* Private function declarations: page creation                               */
/* -------------------------------------------------------------------------- */

static void create_settings_page(lv_obj_t *parent);
static void create_settings_home(lv_obj_t *parent);
static void create_settings_fmo_page(lv_obj_t *parent);
static void create_settings_callsign_page(lv_obj_t *parent);
static void create_settings_volume_page(lv_obj_t *parent);
static void create_settings_backlight_page(lv_obj_t *parent);
static void create_settings_wifi_page(lv_obj_t *parent);
static void create_settings_wifi_scan_page(lv_obj_t *parent);
static void create_settings_battery_page(lv_obj_t *parent);
static void create_settings_station_page(lv_obj_t *parent);
static void create_settings_keyboard(lv_obj_t *parent);

/* -------------------------------------------------------------------------- */
/* Private function declarations: idle clock                                  */
/* -------------------------------------------------------------------------- */

static void create_idle_clock_page(lv_obj_t *parent);
static void app_ui_idle_timer_cb(lv_timer_t *timer);
static void app_ui_idle_clock_update(void);
static void app_ui_show_idle_clock(void);
static void app_ui_hide_idle_clock(void);
static void app_ui_idle_clock_timer_cb(lv_timer_t *timer);
static void app_ui_idle_clock_event_cb(lv_event_t *e);
static void app_ui_idle_clock_long_press_event_cb(lv_event_t *e);
static void app_ui_idle_clock_attach_events(lv_obj_t *obj);
static void app_ui_refresh_idle_clock_callsign(void);
static uint32_t app_ui_get_idle_clock_timeout_ms(void);

/* -------------------------------------------------------------------------- */
/* Utility functions                                                          */
/* -------------------------------------------------------------------------- */

static const lv_font_t *ui_font_cn(void)
{
    return &font20;
}

static const lv_font_t *ui_font_EN_BIG(void)
{
    return &EN_BIG;
}

static const lv_font_t *ui_font_status(void)
{
    return &status_font;
}

static const lv_font_t *ui_font_28(void)
{
    return &font28;
}

static const lv_font_t *ui_font_clock(void)
{
    return &clock_font;
}

static void label_set_color(lv_obj_t *label, lv_color_t color)
{
    if (!label) {
        return;
    }

    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
}

static void img_set_color(lv_obj_t *img, lv_color_t color)
{
    if (!img) {
        return;
    }

    lv_obj_set_style_img_recolor(img, color, LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, LV_PART_MAIN);
}

static void label_set_font(lv_obj_t *label, const lv_font_t *font)
{
    if (!label || !font) {
        return;
    }

    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
}

static void make_clean_obj(lv_obj_t *obj, lv_color_t bg_color, lv_opa_t opa)
{
    if (!obj) {
        return;
    }

    /*
     * 清理 LVGL 默认样式，避免白底、边框、padding 残留。
     */
    lv_obj_remove_style_all(obj);

    lv_obj_set_style_bg_color(obj, bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void set_label_text_safe(lv_obj_t *label,
                                const char *text,
                                const char *fallback)
{
    if (!label) {
        return;
    }

    const char *new_text = NULL;

    if (text && text[0]) {
        new_text = text;
    } else {
        new_text = fallback ? fallback : "--";
    }

    const char *old_text = lv_label_get_text(label);

    /*
     * 文本未变化时避免重复 set_text。
     * lv_label_set_text() 内部可能进行内存分配，
     * 高频重复调用会增加 LVGL heap 压力。
     */
    if (old_text && strcmp(old_text, new_text) == 0) {
        return;
    }

    lv_label_set_text(label, new_text);
}

static int settings_text_to_int(const char *s, int def_val)
{
    if (!s || s[0] == '\0') {
        return def_val;
    }

    return atoi(s);
}

/* -------------------------------------------------------------------------- */
/* Screen rotation and backlight                                               */
/* -------------------------------------------------------------------------- */

static void app_ui_apply_screen_rotation(bool rotate_180)
{
#if LVGL_VERSION_MAJOR >= 8
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) {
        return;
    }

    if (rotate_180) {
        lv_disp_set_rotation(disp, LV_DISP_ROT_180);
    } else {
        lv_disp_set_rotation(disp, LV_DISP_ROT_NONE);
    }

    lv_obj_invalidate(lv_scr_act());
#else
    /*
     * LVGL 7 或更老版本不支持 lv_disp_set_rotation()。
     * 如果使用旧版 LVGL，需要在显示驱动层实现旋转。
     */
    (void)rotate_180;
#endif
}

static esp_err_t app_ui_backlight_pwm_init_once(void)
{
    if (s_backlight_pwm_inited) {
        return ESP_OK;
    }

    ledc_timer_config_t timer_conf = {
        .speed_mode = APP_UI_BL_LEDC_MODE,
        .duty_resolution = APP_UI_BL_LEDC_DUTY_RES,
        .timer_num = APP_UI_BL_LEDC_TIMER,
        .freq_hz = APP_UI_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    esp_err_t ret = ledc_timer_config(&timer_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t ch_conf = {
        .gpio_num = BOARD_LCD_BL_GPIO,
        .speed_mode = APP_UI_BL_LEDC_MODE,
        .channel = APP_UI_BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = APP_UI_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };

    ret = ledc_channel_config(&ch_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_backlight_pwm_inited = true;

    ESP_LOGI(TAG,
             "backlight pwm initialized, gpio=%d, freq=%dHz",
             BOARD_LCD_BL_GPIO,
             APP_UI_BL_LEDC_FREQ_HZ);

    return ESP_OK;
}

static void app_ui_backlight_apply_raw(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    esp_err_t ret = app_ui_backlight_pwm_init_once();
    if (ret != ESP_OK) {
        return;
    }

    uint32_t duty;

#if BOARD_LCD_BL_ON_LEVEL
    /*
     * 高电平点亮：
     * percent 越大，duty 越大。
     */
    duty = (APP_UI_BL_DUTY_MAX * percent) / 100;
#else
    /*
     * 低电平点亮：
     * percent 越大，duty 越小。
     */
    duty = APP_UI_BL_DUTY_MAX - ((APP_UI_BL_DUTY_MAX * percent) / 100);
#endif

    ledc_set_duty(APP_UI_BL_LEDC_MODE, APP_UI_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(APP_UI_BL_LEDC_MODE, APP_UI_BL_LEDC_CHANNEL);

    s_backlight_applied_percent = percent;

    ESP_LOGI(TAG,
             "backlight applied=%u, duty=%lu",
             percent,
             (unsigned long)duty);
}

/* -------------------------------------------------------------------------- */
/* Date and time helpers                                                      */
/* -------------------------------------------------------------------------- */

static void app_ui_format_clock_text(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);

    /*
     * 如果系统时间已同步，tm_year 通常 >= 2020 - 1900。
     */
    if (tm_now.tm_year >= 120) {
        snprintf(buf,
                 buf_size,
                 "%02d:%02d",
                 tm_now.tm_hour,
                 tm_now.tm_min);
    } else {
        /*
         * 未同步时间时，显示设备运行时长。
         */
        int64_t us = esp_timer_get_time();
        uint32_t total_sec = (uint32_t)(us / 1000000);
        uint32_t hour = total_sec / 3600;
        uint32_t min = (total_sec % 3600) / 60;

        snprintf(buf,
                 buf_size,
                 "UP %02lu:%02lu",
                 (unsigned long)hour,
                 (unsigned long)min);
    }
}

static void app_ui_format_datetime_text(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);

    /*
     * 已校时：显示 YYYY-MM-DD HH:MM。
     */
    if (tm_now.tm_year >= 120) {
        snprintf(buf,
                 buf_size,
                 "%04d-%02d-%02d %02d:%02d",
                 tm_now.tm_year + 1900,
                 tm_now.tm_mon + 1,
                 tm_now.tm_mday,
                 tm_now.tm_hour,
                 tm_now.tm_min);
    } else {
        /*
         * 未校时：显示设备运行时长。
         */
        int64_t us = esp_timer_get_time();
        uint32_t total_sec = (uint32_t)(us / 1000000);
        uint32_t hour = total_sec / 3600;
        uint32_t min = (total_sec % 3600) / 60;

        snprintf(buf,
                 buf_size,
                 "UP %02lu:%02lu",
                 (unsigned long)hour,
                 (unsigned long)min);
    }
}

static void app_ui_datetime_update(void)
{
    if (!s_label_datetime) {
        return;
    }

    char buf[24];
    app_ui_format_datetime_text(buf, sizeof(buf));

    lv_label_set_text(s_label_datetime, buf);
}

static void app_ui_datetime_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    app_ui_datetime_update();
}

/* -------------------------------------------------------------------------- */
/* Text fitting helpers                                                       */
/* -------------------------------------------------------------------------- */

static void label_apply_dynamic_letter_space(lv_obj_t *label,
                                             const char *text,
                                             const lv_font_t *font,
                                             lv_coord_t target_width,
                                             int min_letter_space,
                                             int max_letter_space,
                                             int default_letter_space)
{
    if (!label || !font) {
        return;
    }

    if (!text || !text[0]) {
        text = "--";
    }

    int len = (int)strlen(text);

    /*
     * 0/1 个字符不需要两端分布。
     */
    if (len <= 1) {
        lv_obj_set_style_text_letter_space(label,
                                           default_letter_space,
                                           LV_PART_MAIN);
        return;
    }

    /*
     * 先计算 letter_space = 0 时的文本宽度。
     */
    lv_point_t size_zero;

    lv_txt_get_size(&size_zero,
                    text,
                    font,
                    0,              /* letter_space */
                    0,              /* line_space */
                    LV_COORD_MAX,
                    LV_TEXT_FLAG_NONE);

    int base_width = size_zero.x;

    /*
     * len 个字符之间有 len - 1 个间隙。
     * 将剩余宽度平均分配到这些间隙上。
     */
    int gaps = len - 1;
    int letter_space = default_letter_space;

    if (gaps > 0) {
        letter_space = (target_width - base_width) / gaps;
    }

    if (letter_space > max_letter_space) {
        letter_space = max_letter_space;
    }

    if (letter_space < min_letter_space) {
        letter_space = min_letter_space;
    }

    lv_obj_set_style_text_letter_space(label,
                                       letter_space,
                                       LV_PART_MAIN);
}

static void current_call_apply_fit_style(const char *text)
{
    if (!s_label_current_call) {
        return;
    }

    if (!text || !text[0]) {
        text = "--";
    }

    /*
     * 当前呼号区域屏幕宽 320。
     * target_width 不建议使用满宽，留一点边距更安全。
     */
    label_apply_dynamic_letter_space(s_label_current_call,
                                     text,
                                     ui_font_EN_BIG(),
                                     300,    /* target_width */
                                     -3,     /* min_letter_space */
                                     12,     /* max_letter_space */
                                     2);     /* default_letter_space */
}

static void idle_clock_callsign_apply_fit_style(const char *text)
{
    if (!s_label_idle_clock_callsign) {
        return;
    }

    if (!text || !text[0]) {
        text = APP_DEFAULT_OWNER_CALLSIGN;
    }

    /*
     * 待机时钟呼号字体较大，target_width 保守一些。
     */
    label_apply_dynamic_letter_space(s_label_idle_clock_callsign,
                                     text,
                                     ui_font_clock(),
                                     306,    /* target_width */
                                     -3,     /* min_letter_space */
                                     18,     /* max_letter_space */
                                     2);     /* default_letter_space */
}

/* -------------------------------------------------------------------------- */
/* Main screen helpers                                                        */
/* -------------------------------------------------------------------------- */

static int wifi_rssi_to_percent(int rssi)
{
    if (rssi <= -90) {
        return 0;
    }

    if (rssi >= -30) {
        return 100;
    }

    return (rssi + 90) * 100 / 60;
}

static void mute_button_update_style(void)
{
    if (!s_btn_mute || !s_img_mute_icon) {
        return;
    }

    /*
     * 为避免 LVGL 绘制圆角背景时频繁分配 mask，
     * 静音按钮不绘制背景，只改变图标颜色。
     */
    lv_obj_set_style_bg_opa(s_btn_mute, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_btn_mute, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_btn_mute, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_btn_mute, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_mute, 0, LV_PART_MAIN);

    if (s_audio_muted) {
        img_set_color(s_img_mute_icon, UI_COLOR_GRAY);
    } else {
        img_set_color(s_img_mute_icon, UI_COLOR_ORANGE);
    }
}

/* -------------------------------------------------------------------------- */
/* Station list requests                                                      */
/* -------------------------------------------------------------------------- */

static void settings_station_request_page(int page)
{
    if (page < 0) {
        page = 0;
    }

    s_station_menu_page = page;

    int start = s_station_menu_page * STATION_MENU_PAGE_SIZE;

    if (!audio_ws_station_is_connected()) {
        if (s_label_station_page_status) {
            lv_label_set_text(s_label_station_page_status, "站点WS未连接");
        }

        app_ui_update_status("站点WS未连接");
        return;
    }

    if (s_label_station_page_status) {
        lv_label_set_text(s_label_station_page_status, "加载中...");
    }

    /*
     * 多请求 1 条，用于判断是否还有下一页。
     * 例如每页显示 6 条，请求 7 条。
     */
    esp_err_t ret = audio_ws_station_get_list(start,
                                              STATION_MENU_FETCH_COUNT);

    if (ret != ESP_OK) {
        if (s_label_station_page_status) {
            lv_label_set_text(s_label_station_page_status, "请求失败");
        }

        app_ui_update_status("站点列表请求失败");
    }
}

static void main_station_popup_request_page(int page)
{
    if (page < 0) {
        page = 0;
    }

    s_main_station_popup_page = page;

    int start = s_main_station_popup_page * STATION_MENU_PAGE_SIZE;

    if (!audio_ws_station_is_connected()) {
        if (s_label_main_station_popup_status) {
            lv_label_set_text(s_label_main_station_popup_status, "站点WS未连接");
        }

        app_ui_update_status("站点WS未连接");
        return;
    }

    if (s_label_main_station_popup_status) {
        lv_label_set_text(s_label_main_station_popup_status, "加载中...");
    }

    /*
     * 主界面快捷切换使用收藏/置顶列表。
     */
    esp_err_t ret = audio_ws_station_get_pinned_list(start,
                                                     STATION_MENU_FETCH_COUNT);

    if (ret != ESP_OK) {
        if (s_label_main_station_popup_status) {
            lv_label_set_text(s_label_main_station_popup_status, "请求失败");
        }

        app_ui_update_status("收藏站点请求失败");
    }
}

/* -------------------------------------------------------------------------- */
/* Main station popup                                                         */
/* -------------------------------------------------------------------------- */

static void main_station_popup_render(void)
{
    station_item_t items[STATION_MENU_FETCH_COUNT];

    int count = station_cache_get_pinned_list(items,
                                              STATION_MENU_FETCH_COUNT);

    s_main_station_popup_has_next = (count > STATION_MENU_PAGE_SIZE);

    int display_count = count;
    if (display_count > STATION_MENU_PAGE_SIZE) {
        display_count = STATION_MENU_PAGE_SIZE;
    }

    for (int i = 0; i < STATION_MENU_PAGE_SIZE; i++) {
        s_main_station_item_uids[i] = -1;

        if (!s_main_station_item_btns[i] ||
            !s_main_station_item_labels[i]) {
            continue;
        }

        if (i < display_count) {
            s_main_station_item_uids[i] = items[i].uid;

            lv_label_set_text(s_main_station_item_labels[i], items[i].name);
            lv_obj_clear_flag(s_main_station_item_btns[i],
                              LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_main_station_item_labels[i], "--");
            lv_obj_add_flag(s_main_station_item_btns[i],
                            LV_OBJ_FLAG_HIDDEN);
        }
    }

    settings_station_set_button_enabled(s_main_station_btn_prev,
                                        s_main_station_popup_page > 0);

    settings_station_set_button_enabled(s_main_station_btn_next,
                                        s_main_station_popup_has_next);

    if (s_label_main_station_popup_status) {
        char buf[48];

        if (display_count == 0) {
            snprintf(buf,
                     sizeof(buf),
                     "P%d empty",
                     s_main_station_popup_page + 1);
        } else {
            snprintf(buf,
                     sizeof(buf),
                     "收藏 P%d %d%s",
                     s_main_station_popup_page + 1,
                     display_count,
                     s_main_station_popup_has_next ? "" : " end");
        }

        lv_label_set_text(s_label_main_station_popup_status, buf);
    }
}

static void main_station_popup_open_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (s_power_save_clock_active) {
        return;
    }

    if (!s_main_station_popup) {
        return;
    }

    /*
     * 设置页打开时，不弹出主界面站点列表。
     */
    if (s_settings_page &&
        !lv_obj_has_flag(s_settings_page, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    lv_obj_clear_flag(s_main_station_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_main_station_popup);

    main_station_popup_request_page(0);
}

static void main_station_popup_close_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (s_main_station_popup) {
        lv_obj_add_flag(s_main_station_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void main_station_popup_prev_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (s_main_station_popup_page <= 0) {
        app_ui_update_status("已经是第一页");
        return;
    }

    main_station_popup_request_page(s_main_station_popup_page - 1);
}

static void main_station_popup_next_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (!s_main_station_popup_has_next) {
        app_ui_update_status("已经是最后一页");
        return;
    }

    main_station_popup_request_page(s_main_station_popup_page + 1);
}

static void main_station_popup_refresh_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    main_station_popup_request_page(s_main_station_popup_page);
}

static void main_station_popup_item_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);

    int uid = -1;

    for (int i = 0; i < STATION_MENU_PAGE_SIZE; i++) {
        if (btn == s_main_station_item_btns[i]) {
            uid = s_main_station_item_uids[i];
            break;
        }
    }

    if (uid < 0) {
        return;
    }

    esp_err_t ret = audio_ws_station_set_current(uid);
    if (ret == ESP_OK) {
        app_ui_update_status("站点切换中");

        if (s_label_main_station_popup_status) {
            lv_label_set_text(s_label_main_station_popup_status,
                              "站点切换中...");
        }

        if (s_main_station_popup) {
            lv_obj_add_flag(s_main_station_popup, LV_OBJ_FLAG_HIDDEN);
        }

        /*
         * 复用设置页已有的延迟 getCurrent 回调。
         */
        lv_timer_create(settings_station_get_current_delay_cb, 300, NULL);
    } else {
        app_ui_update_status("站点切换失败");

        if (s_label_main_station_popup_status) {
            lv_label_set_text(s_label_main_station_popup_status, "切换失败");
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Settings station list render                                               */
/* -------------------------------------------------------------------------- */

static void settings_station_set_button_enabled(lv_obj_t *btn, bool enabled)
{
    if (!btn) {
        return;
    }

    if (enabled) {
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(btn, UI_COLOR_PANEL, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(btn, UI_COLOR_DARK_GRAY, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_PART_MAIN);
    }
}

static void settings_station_list_render(void)
{
    station_item_t items[STATION_MENU_FETCH_COUNT];

    int count = station_cache_get_list(items,
                                       STATION_MENU_FETCH_COUNT);

    /*
     * count > STATION_MENU_PAGE_SIZE 说明还有下一页。
     */
    s_station_menu_has_next = (count > STATION_MENU_PAGE_SIZE);

    int display_count = count;
    if (display_count > STATION_MENU_PAGE_SIZE) {
        display_count = STATION_MENU_PAGE_SIZE;
    }

    for (int i = 0; i < STATION_MENU_PAGE_SIZE; i++) {
        s_station_item_uids[i] = -1;

        if (!s_station_item_btns[i] || !s_station_item_labels[i]) {
            continue;
        }

        if (i < display_count) {
            s_station_item_uids[i] = items[i].uid;

            lv_label_set_text(s_station_item_labels[i], items[i].name);
            lv_obj_clear_flag(s_station_item_btns[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_station_item_labels[i], "--");
            lv_obj_add_flag(s_station_item_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /*
     * 上一页按钮：第一页禁用。
     */
    settings_station_set_button_enabled(s_station_btn_prev,
                                        s_station_menu_page > 0);

    /*
     * 下一页按钮：只有多返回一条时才启用。
     */
    settings_station_set_button_enabled(s_station_btn_next,
                                        s_station_menu_has_next);

    if (s_label_station_page_status) {
        char buf[64];

        if (display_count == 0) {
            snprintf(buf,
                     sizeof(buf),
                     "P%d empty",
                     s_station_menu_page + 1);
        } else {
            snprintf(buf,
                     sizeof(buf),
                     "P%d %d%s",
                     s_station_menu_page + 1,
                     display_count,
                     s_station_menu_has_next ? "" : " end");
        }

        lv_label_set_text(s_label_station_page_status, buf);
    }
}


/* -------------------------------------------------------------------------- */
/* Settings page: list update and home values                                 */
/* -------------------------------------------------------------------------- */

void app_ui_station_list_updated(void)
{
    /*
     * 设置页：全部站点列表。
     */
    if (s_settings_station_page &&
        !lv_obj_has_flag(s_settings_station_page, LV_OBJ_FLAG_HIDDEN)) {
        settings_station_list_render();
    }

    /*
     * 主界面弹窗：收藏站点列表。
     */
    if (s_main_station_popup &&
        !lv_obj_has_flag(s_main_station_popup, LV_OBJ_FLAG_HIDDEN)) {
        main_station_popup_render();
    }
}

static void settings_refresh_home_values(void)
{
    const app_settings_t *cfg = app_settings_get();
    if (!cfg) {
        return;
    }

    if (s_label_setting_fmo_value) {
        lv_label_set_text(s_label_setting_fmo_value, cfg->fmo_host);
    }

    if (s_label_setting_callsign_value) {
        if (cfg->owner_callsign[0]) {
            lv_label_set_text(s_label_setting_callsign_value,
                              cfg->owner_callsign);
        } else {
            lv_label_set_text(s_label_setting_callsign_value,
                              APP_DEFAULT_OWNER_CALLSIGN);
        }
    }

    if (s_label_setting_volume_value) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u%%", cfg->audio_volume);
        lv_label_set_text(s_label_setting_volume_value, buf);
    }

    if (s_label_setting_backlight_value) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u%%", cfg->backlight_percent);
        lv_label_set_text(s_label_setting_backlight_value, buf);
    }

    if (s_label_setting_wifi_value) {
        if (cfg->wifi_ssid[0]) {
            lv_label_set_text(s_label_setting_wifi_value, cfg->wifi_ssid);
        } else {
            lv_label_set_text(s_label_setting_wifi_value, "--");
        }
    }

    if (s_label_setting_battery_value) {
        char buf[32];

        snprintf(buf,
                 sizeof(buf),
                 "%u/%umV",
                 cfg->battery_empty_mv,
                 cfg->battery_full_mv);

        lv_label_set_text(s_label_setting_battery_value, buf);
    }

    if (s_label_setting_station_value) {
        lv_label_set_text(s_label_setting_station_value, "所有服务器");
    }

    if (s_label_setting_qso_value) {
        if (cfg->qso_count_valid) {
            char buf[24];

            snprintf(buf,
                     sizeof(buf),
                     "%lu",
                     (unsigned long)cfg->qso_count);

            lv_label_set_text(s_label_setting_qso_value, buf);
        } else {
            lv_label_set_text(s_label_setting_qso_value, "未同步");
        }
    }

    if (s_label_setting_power_save_value) {
        lv_label_set_text(
            s_label_setting_power_save_value,
            app_power_save_is_manual_enabled() ? "开" : "关"
        );
    }

    if (s_label_setting_rotate_value) {
        lv_label_set_text(
            s_label_setting_rotate_value,
            cfg->screen_rotate_180 ? "180°" : "正常"
        );
    }
}

/* -------------------------------------------------------------------------- */
/* Settings page: power save, QSO sync and screen rotation                    */
/* -------------------------------------------------------------------------- */

static void settings_power_save_toggle_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    app_power_save_toggle_manual();

    settings_refresh_home_values();
    power_save_button_update_style();
}

static void settings_qso_sync_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    /*
     * 点击后立即弹窗提示。
     * 后续成功或失败由 audio_ws.c 通过 ui_async_qso_sync_popup_show()
     * 回到 LVGL 上下文后更新。
     */
    app_ui_qso_sync_popup_show("正在同步QSO...", 0);

    audio_ws_qso_count_manual_full_scan();
}

static void settings_rotate_toggle_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    const app_settings_t *cfg = app_settings_get();

    bool old_val = cfg ? cfg->screen_rotate_180 : false;
    bool new_val = !old_val;

    esp_err_t ret = app_settings_set_screen_rotate_180(new_val);
    if (ret == ESP_OK) {
        settings_refresh_home_values();

        /*
         * 当前项目设计为保存后重启生效。
         * 如果后续希望立即生效，可在这里调用：
         * app_ui_apply_screen_rotation(new_val);
         */
        app_ui_update_status("旋转重启生效");
    } else {
        app_ui_update_status("旋转保存失败");
    }
}

/* -------------------------------------------------------------------------- */
/* Settings page: WiFi settings                                               */
/* -------------------------------------------------------------------------- */

static void settings_wifi_open_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    const app_settings_t *cfg = app_settings_get();

    settings_hide_keyboard();

    if (s_settings_home) {
        lv_obj_add_flag(s_settings_home, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_fmo_page) {
        lv_obj_add_flag(s_settings_fmo_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_volume_page) {
        lv_obj_add_flag(s_settings_volume_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_battery_page) {
        lv_obj_add_flag(s_settings_battery_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_wifi_page) {
        lv_obj_clear_flag(s_settings_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (cfg) {
        if (s_ta_wifi_ssid) {
            lv_textarea_set_text(s_ta_wifi_ssid, cfg->wifi_ssid);
        }

        if (s_ta_wifi_password) {
            lv_textarea_set_text(s_ta_wifi_password, cfg->wifi_password);
        }
    }
}

static void settings_wifi_save_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    settings_hide_keyboard();

    if (!s_ta_wifi_ssid || !s_ta_wifi_password) {
        return;
    }

    const char *ssid = lv_textarea_get_text(s_ta_wifi_ssid);
    const char *password = lv_textarea_get_text(s_ta_wifi_password);

    if (!ssid || ssid[0] == '\0') {
        app_ui_update_status("SSID不能为空");
        return;
    }

    esp_err_t ret = app_settings_set_wifi(ssid, password ? password : "");
    if (ret == ESP_OK) {
        app_ui_update_status("WiFi已保存");
        settings_refresh_home_values();
        settings_show_home();
    } else {
        app_ui_update_status("WiFi保存失败");
    }
}

/* -------------------------------------------------------------------------- */
/* Settings page: WiFi scan                                                   */
/* -------------------------------------------------------------------------- */

static void settings_wifi_scan_render(void)
{
    for (int i = 0; i < WIFI_SCAN_MENU_MAX_ITEMS; i++) {
        if (!s_wifi_scan_item_btns[i] || !s_wifi_scan_item_labels[i]) {
            continue;
        }

        if (i < s_wifi_scan_count) {
            char buf[80];

            snprintf(buf,
                     sizeof(buf),
                     "%s  %ddBm",
                     s_wifi_scan_items[i].ssid,
                     s_wifi_scan_items[i].rssi);

            lv_label_set_text(s_wifi_scan_item_labels[i], buf);
            lv_obj_clear_flag(s_wifi_scan_item_btns[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_wifi_scan_item_labels[i], "--");
            lv_obj_add_flag(s_wifi_scan_item_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_label_wifi_scan_status) {
        char buf[32];

        snprintf(buf, sizeof(buf), "%d AP", s_wifi_scan_count);
        lv_label_set_text(s_label_wifi_scan_status, buf);
    }
}

static void settings_wifi_scan_open_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    settings_hide_keyboard();

    if (s_settings_wifi_page) {
        lv_obj_add_flag(s_settings_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_wifi_scan_page) {
        lv_obj_clear_flag(s_settings_wifi_scan_page, LV_OBJ_FLAG_HIDDEN);
    }

    /*
     * 如果正在扫描，不重复启动。
     */
    if (s_wifi_scan_in_progress) {
        app_ui_update_status("正在扫描");

        if (s_label_wifi_scan_status) {
            lv_label_set_text(s_label_wifi_scan_status, "扫描中...");
        }

        return;
    }

    s_wifi_scan_count = 0;
    memset(s_wifi_scan_items, 0, sizeof(s_wifi_scan_items));

    for (int i = 0; i < WIFI_SCAN_MENU_MAX_ITEMS; i++) {
        if (s_wifi_scan_item_btns[i]) {
            lv_obj_add_flag(s_wifi_scan_item_btns[i], LV_OBJ_FLAG_HIDDEN);
        }

        if (s_wifi_scan_item_labels[i]) {
            lv_label_set_text(s_wifi_scan_item_labels[i], "--");
        }
    }

    if (s_label_wifi_scan_status) {
        lv_label_set_text(s_label_wifi_scan_status, "扫描中...");
    }

    app_ui_update_status("扫描WiFi");

    s_wifi_scan_result_ready = false;
    s_wifi_scan_result = ESP_OK;
    s_wifi_scan_in_progress = true;

    /*
     * 创建后台扫描任务。
     * 不在 LVGL 线程中同步扫描，避免 UI 卡顿。
     */
    BaseType_t ret = xTaskCreate(settings_wifi_scan_task,
                                 "wifi_scan_task",
                                 4096,
                                 NULL,
                                 3,
                                 &s_wifi_scan_task_handle);

    if (ret != pdPASS) {
        s_wifi_scan_in_progress = false;
        s_wifi_scan_task_handle = NULL;

        if (s_label_wifi_scan_status) {
            lv_label_set_text(s_label_wifi_scan_status, "启动失败");
        }

        app_ui_update_status("扫描启动失败");
        return;
    }

    /*
     * 使用 LVGL timer 轮询扫描结果。
     * timer 在 LVGL 上下文执行，可以安全刷新 UI。
     */
    if (!s_wifi_scan_poll_timer) {
        s_wifi_scan_poll_timer = lv_timer_create(
            settings_wifi_scan_poll_timer_cb,
            200,
            NULL
        );
    }
}

static void settings_wifi_scan_item_event_cb(lv_event_t *e)
{
    if (s_wifi_scan_in_progress) {
        app_ui_update_status("正在扫描");
        return;
    }

    lv_obj_t *btn = lv_event_get_target(e);

    int index = -1;

    for (int i = 0; i < WIFI_SCAN_MENU_MAX_ITEMS; i++) {
        if (btn == s_wifi_scan_item_btns[i]) {
            index = i;
            break;
        }
    }

    if (index < 0 || index >= s_wifi_scan_count) {
        return;
    }

    if (s_ta_wifi_ssid) {
        lv_textarea_set_text(s_ta_wifi_ssid,
                             s_wifi_scan_items[index].ssid);
    }

    /*
     * 返回 WiFi 设置页，让用户输入密码并保存。
     */
    if (s_settings_wifi_scan_page) {
        lv_obj_add_flag(s_settings_wifi_scan_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_wifi_page) {
        lv_obj_clear_flag(s_settings_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }

    app_ui_update_status("已选择WiFi");
}

static void settings_wifi_scan_back_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    /*
     * 如果正在扫描，不强行删除后台任务。
     * 只返回上一页，扫描完成后 timer 会检查页面是否隐藏。
     */
    if (s_settings_wifi_scan_page) {
        lv_obj_add_flag(s_settings_wifi_scan_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_wifi_page) {
        lv_obj_clear_flag(s_settings_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }

    app_ui_update_status("WiFi设置");
}

static void settings_wifi_scan_task(void *arg)
{
    LV_UNUSED(arg);

    wifi_scan_item_t tmp_items[WIFI_SCAN_MENU_MAX_ITEMS];
    int tmp_count = 0;

    memset(tmp_items, 0, sizeof(tmp_items));

    esp_err_t ret = wifi_manager_scan(tmp_items,
                                      WIFI_SCAN_MENU_MAX_ITEMS,
                                      &tmp_count);

    /*
     * 后台任务不操作 LVGL，只写扫描结果。
     * UI 刷新由 settings_wifi_scan_poll_timer_cb() 完成。
     */
    if (ret == ESP_OK) {
        memcpy(s_wifi_scan_items, tmp_items, sizeof(tmp_items));
        s_wifi_scan_count = tmp_count;
    } else {
        s_wifi_scan_count = 0;
    }

    s_wifi_scan_result = ret;
    s_wifi_scan_result_ready = true;
    s_wifi_scan_in_progress = false;
    s_wifi_scan_task_handle = NULL;

    vTaskDelete(NULL);
}

static void settings_wifi_scan_poll_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    if (!s_wifi_scan_result_ready) {
        return;
    }

    s_wifi_scan_result_ready = false;

    /*
     * 扫描页如果已经不显示，则不刷新列表。
     */
    if (!s_settings_wifi_scan_page ||
        lv_obj_has_flag(s_settings_wifi_scan_page, LV_OBJ_FLAG_HIDDEN)) {

        if (s_wifi_scan_poll_timer) {
            lv_timer_del(s_wifi_scan_poll_timer);
            s_wifi_scan_poll_timer = NULL;
        }

        return;
    }

    if (s_wifi_scan_result == ESP_OK) {
        settings_wifi_scan_render();

        if (s_wifi_scan_count == 0) {
            if (s_label_wifi_scan_status) {
                lv_label_set_text(s_label_wifi_scan_status, "未发现");
            }

            app_ui_update_status("未发现WiFi");
        } else {
            if (s_label_wifi_scan_status) {
                char buf[24];

                snprintf(buf, sizeof(buf), "%d AP", s_wifi_scan_count);
                lv_label_set_text(s_label_wifi_scan_status, buf);
            }

            app_ui_update_status("扫描完成");
        }
    } else {
        if (s_label_wifi_scan_status) {
            lv_label_set_text(s_label_wifi_scan_status, "扫描失败");
        }

        app_ui_update_status("WiFi扫描失败");
    }

    /*
     * 本次扫描完成后删除 timer。
     * 下次扫描时重新创建。
     */
    if (s_wifi_scan_poll_timer) {
        lv_timer_del(s_wifi_scan_poll_timer);
        s_wifi_scan_poll_timer = NULL;
    }
}

/* -------------------------------------------------------------------------- */
/* Settings page: station list events                                         */
/* -------------------------------------------------------------------------- */

static void settings_station_open_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    settings_hide_keyboard();

    if (s_settings_home) {
        lv_obj_add_flag(s_settings_home, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_fmo_page) {
        lv_obj_add_flag(s_settings_fmo_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_volume_page) {
        lv_obj_add_flag(s_settings_volume_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_wifi_page) {
        lv_obj_add_flag(s_settings_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_battery_page) {
        lv_obj_add_flag(s_settings_battery_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_station_page) {
        lv_obj_clear_flag(s_settings_station_page, LV_OBJ_FLAG_HIDDEN);
    }

    settings_station_request_page(0);
}

static void settings_station_prev_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (s_station_menu_page <= 0) {
        app_ui_update_status("已经是第一页");
        return;
    }

    settings_station_request_page(s_station_menu_page - 1);
}

static void settings_station_next_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (!s_station_menu_has_next) {
        app_ui_update_status("已经是最后一页");
        return;
    }

    settings_station_request_page(s_station_menu_page + 1);
}

static void settings_station_refresh_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    settings_station_request_page(s_station_menu_page);
}

static void settings_station_get_current_delay_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    audio_ws_station_get_current();

    lv_timer_del(timer);
}

static void settings_station_item_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);

    int uid = -1;

    for (int i = 0; i < STATION_MENU_PAGE_SIZE; i++) {
        if (btn == s_station_item_btns[i]) {
            uid = s_station_item_uids[i];
            break;
        }
    }

    if (uid < 0) {
        return;
    }

    esp_err_t ret = audio_ws_station_set_current(uid);
    if (ret == ESP_OK) {
        app_ui_update_status("站点切换中");

        if (s_label_station_page_status) {
            lv_label_set_text(s_label_station_page_status, "站点切换中...");
        }

        /*
         * setCurrent 后延迟 300ms 再请求当前站点，
         * 给服务端留出更新当前站点状态的时间。
         */
        lv_timer_create(settings_station_get_current_delay_cb, 300, NULL);

        /*
         * 操作后返回设置首页，更符合设置页使用习惯。
         */
        settings_show_home();
    } else {
        app_ui_update_status("站点切换失败");

        if (s_label_station_page_status) {
            lv_label_set_text(s_label_station_page_status, "切换失败");
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Settings page: keyboard and page navigation                                */
/* -------------------------------------------------------------------------- */

static void settings_hide_keyboard(void)
{
    if (s_settings_keyboard) {
        lv_obj_add_flag(s_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_settings_keyboard, NULL);
    }
}

static void settings_show_home(void)
{
    settings_hide_keyboard();

    if (s_settings_home) {
        lv_obj_clear_flag(s_settings_home, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_fmo_page) {
        lv_obj_add_flag(s_settings_fmo_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_callsign_page) {
        lv_obj_add_flag(s_settings_callsign_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_volume_page) {
        lv_obj_add_flag(s_settings_volume_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_backlight_page) {
        lv_obj_add_flag(s_settings_backlight_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_wifi_page) {
        lv_obj_add_flag(s_settings_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_battery_page) {
        lv_obj_add_flag(s_settings_battery_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_station_page) {
        lv_obj_add_flag(s_settings_station_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_wifi_scan_page) {
        lv_obj_add_flag(s_settings_wifi_scan_page, LV_OBJ_FLAG_HIDDEN);
    }

    settings_refresh_home_values();
}

static void settings_close_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    settings_hide_keyboard();

    if (s_settings_page) {
        lv_obj_add_flag(s_settings_page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settings_open_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (!s_settings_page) {
        return;
    }

    settings_refresh_home_values();
    settings_show_home();

    lv_obj_clear_flag(s_settings_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings_page);
}

static void settings_back_home_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    settings_show_home();
}

static void settings_ta_text_focus_event_cb(lv_event_t *e)
{
    if (!s_settings_keyboard) {
        return;
    }

    lv_obj_t *ta = lv_event_get_target(e);

    lv_keyboard_set_textarea(s_settings_keyboard, ta);

#if LV_USE_KEYBOARD
    lv_keyboard_set_mode(s_settings_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
#endif

    lv_obj_clear_flag(s_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings_keyboard);
}

static void settings_ta_number_focus_event_cb(lv_event_t *e)
{
    if (!s_settings_keyboard) {
        return;
    }

    lv_obj_t *ta = lv_event_get_target(e);

    lv_keyboard_set_textarea(s_settings_keyboard, ta);

#if LV_USE_KEYBOARD
    lv_keyboard_set_mode(s_settings_keyboard, LV_KEYBOARD_MODE_NUMBER);
#endif

    lv_obj_clear_flag(s_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings_keyboard);
}

static void settings_keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        settings_hide_keyboard();
    }
}

static void settings_restart_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    app_ui_update_status("正在重启");
    esp_restart();
}

/* -------------------------------------------------------------------------- */
/* Main screen: mute and power-save button events                             */
/* -------------------------------------------------------------------------- */

static void mute_button_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (s_audio_muted) {
        /*
         * 当前静音：点击后开启 Audio WS。
         */
        esp_err_t ret = audio_ws_audio_enable();
        if (ret == ESP_OK) {
            s_audio_muted = false;
            app_ui_update_status("音频已开启");
        } else {
            s_audio_muted = true;
            app_ui_update_status("音频开启失败");
        }
    } else {
        /*
         * 当前已开启音频：点击后关闭 Audio WS。
         */
        esp_err_t ret = audio_ws_audio_disable();
        if (ret == ESP_OK) {
            s_audio_muted = true;
            app_ui_update_status("已静音");
        } else {
            app_ui_update_status("静音失败");
        }
    }

    mute_button_update_style();
}

static void power_save_button_update_style(void)
{
    if (!s_btn_power_save || !s_img_power_save_icon) {
        return;
    }

    /*
     * 当前底部节能按钮使用图标显示，不再创建文字 label。
     * 因此这里通过图标颜色表示节能状态。
     */
    bool enabled = app_power_save_is_manual_enabled();

    lv_obj_set_style_bg_opa(s_btn_power_save, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_btn_power_save, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_btn_power_save, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_btn_power_save, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_power_save, 0, LV_PART_MAIN);

    if (enabled) {
        img_set_color(s_img_power_save_icon, UI_COLOR_ORANGE);
    } else {
        img_set_color(s_img_power_save_icon, UI_COLOR_GRAY);
    }
}

static void power_save_button_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    app_power_save_toggle_manual();

    power_save_button_update_style();

    /*
     * 同步刷新设置页“节能模式”的显示。
     */
    settings_refresh_home_values();

    if (app_power_save_is_manual_enabled()) {
        app_ui_update_status("节能模式");
    } else {
        app_ui_update_status("正常模式");
    }
}

/* -------------------------------------------------------------------------- */
/* Settings page: FMO address                                                 */
/* -------------------------------------------------------------------------- */

static void settings_fmo_open_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    const app_settings_t *cfg = app_settings_get();

    if (s_settings_home) {
        lv_obj_add_flag(s_settings_home, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_fmo_page) {
        lv_obj_clear_flag(s_settings_fmo_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_volume_page) {
        lv_obj_add_flag(s_settings_volume_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_ta_fmo_host && cfg) {
        lv_textarea_set_text(s_ta_fmo_host, cfg->fmo_host);
    }

    if (s_settings_keyboard && s_ta_fmo_host) {
        lv_keyboard_set_textarea(s_settings_keyboard, s_ta_fmo_host);

#if LV_USE_KEYBOARD
        lv_keyboard_set_mode(s_settings_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
#endif

        lv_obj_clear_flag(s_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_settings_keyboard);
    }
}

static void settings_fmo_save_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    settings_hide_keyboard();

    if (!s_ta_fmo_host) {
        return;
    }

    const char *host = lv_textarea_get_text(s_ta_fmo_host);
    if (!host || host[0] == '\0') {
        app_ui_update_status("IP不能为空");
        return;
    }

    esp_err_t ret = app_settings_set_fmo_host(host);
    if (ret == ESP_OK) {
        app_ui_update_status("FMO地址已保存");
        settings_refresh_home_values();
        settings_show_home();
    } else {
        app_ui_update_status("FMO地址保存失败");
    }
}

/* -------------------------------------------------------------------------- */
/* Settings page: volume                                                      */
/* -------------------------------------------------------------------------- */

static void settings_volume_open_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    const app_settings_t *cfg = app_settings_get();

    settings_hide_keyboard();

    if (s_settings_home) {
        lv_obj_add_flag(s_settings_home, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_fmo_page) {
        lv_obj_add_flag(s_settings_fmo_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_volume_page) {
        lv_obj_clear_flag(s_settings_volume_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (cfg && s_slider_volume) {
        lv_slider_set_value(s_slider_volume, cfg->audio_volume, LV_ANIM_OFF);

        if (s_label_volume_value) {
            char buf[16];

            snprintf(buf, sizeof(buf), "%u%%", cfg->audio_volume);
            lv_label_set_text(s_label_volume_value, buf);
        }
    }
}

static void settings_volume_slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int value = lv_slider_get_value(slider);

    if (value < 0) {
        value = 0;
    }

    if (value > 100) {
        value = 100;
    }

    if (s_label_volume_value) {
        char buf[16];

        snprintf(buf, sizeof(buf), "%d%%", value);
        lv_label_set_text(s_label_volume_value, buf);
    }

    /*
     * 音量拖动实时生效，但是否持久化由保存按钮决定。
     */
    audio_set_volume((uint8_t)value);

    if (value > 0) {
        s_audio_muted = false;
        s_audio_volume_before_mute = (uint8_t)value;
    } else {
        s_audio_muted = true;
    }

    mute_button_update_style();
}

static void settings_volume_save_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (!s_slider_volume) {
        return;
    }

    int value = lv_slider_get_value(s_slider_volume);

    if (value < 0) {
        value = 0;
    }

    if (value > 100) {
        value = 100;
    }

    esp_err_t ret = app_settings_set_audio_volume((uint8_t)value);
    if (ret == ESP_OK) {
        audio_set_volume((uint8_t)value);

        if (value > 0) {
            s_audio_muted = false;
            s_audio_volume_before_mute = (uint8_t)value;
        } else {
            s_audio_muted = true;
        }

        mute_button_update_style();

        app_ui_update_status("音量已保存");
        settings_refresh_home_values();
        settings_show_home();
    } else {
        app_ui_update_status("音量保存失败");
    }
}

/* -------------------------------------------------------------------------- */
/* Settings page: backlight                                                   */
/* -------------------------------------------------------------------------- */

static void settings_backlight_open_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    const app_settings_t *cfg = app_settings_get();

    settings_hide_keyboard();

    if (s_settings_home) {
        lv_obj_add_flag(s_settings_home, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_fmo_page) {
        lv_obj_add_flag(s_settings_fmo_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_volume_page) {
        lv_obj_add_flag(s_settings_volume_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_wifi_page) {
        lv_obj_add_flag(s_settings_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_battery_page) {
        lv_obj_add_flag(s_settings_battery_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_station_page) {
        lv_obj_add_flag(s_settings_station_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_backlight_page) {
        lv_obj_clear_flag(s_settings_backlight_page, LV_OBJ_FLAG_HIDDEN);
    }

    uint8_t val = cfg ? cfg->backlight_percent : s_backlight_target_percent;

    if (s_slider_backlight) {
        lv_slider_set_value(s_slider_backlight, val, LV_ANIM_OFF);
    }

    if (s_label_backlight_value) {
        char buf[16];

        snprintf(buf, sizeof(buf), "%u%%", val);
        lv_label_set_text(s_label_backlight_value, buf);
    }
}

void app_ui_wake_from_idle(void)
{
    /*
     * 省电模式下，不允许业务事件唤醒退出时钟页。
     */
    if (s_power_save_clock_active) {
        return;
    }

    app_ui_hide_idle_clock();

#if LVGL_VERSION_MAJOR >= 8
    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
        lv_disp_trig_activity(disp);
    }
#else
    lv_disp_trig_activity(NULL);
#endif

    app_ui_backlight_apply_raw(s_backlight_target_percent);
}

static void settings_backlight_slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int value = lv_slider_get_value(slider);

    if (value < 5) {
        value = 5;
    }

    if (value > 100) {
        value = 100;
    }

    if (s_label_backlight_value) {
        char buf[16];

        snprintf(buf, sizeof(buf), "%d%%", value);
        lv_label_set_text(s_label_backlight_value, buf);
    }

    /*
     * 实时生效，但不保存到 NVS。
     */
    app_ui_set_backlight_percent((uint8_t)value);
}

static void settings_backlight_save_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    if (!s_slider_backlight) {
        return;
    }

    int value = lv_slider_get_value(s_slider_backlight);

    if (value < 5) {
        value = 5;
    }

    if (value > 100) {
        value = 100;
    }

    esp_err_t ret = app_settings_set_backlight((uint8_t)value);
    if (ret == ESP_OK) {
        app_ui_set_backlight_percent((uint8_t)value);
        app_ui_update_status("背光已保存");
        settings_refresh_home_values();
        settings_show_home();
    } else {
        app_ui_update_status("背光保存失败");
    }
}

/* -------------------------------------------------------------------------- */
/* Settings page: battery calibration                                         */
/* -------------------------------------------------------------------------- */

static void settings_battery_open_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    const app_settings_t *cfg = app_settings_get();

    settings_hide_keyboard();

    if (s_settings_home) {
        lv_obj_add_flag(s_settings_home, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_fmo_page) {
        lv_obj_add_flag(s_settings_fmo_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_volume_page) {
        lv_obj_add_flag(s_settings_volume_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_wifi_page) {
        lv_obj_add_flag(s_settings_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_battery_page) {
        lv_obj_clear_flag(s_settings_battery_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (cfg) {
        char buf[16];

        if (s_ta_battery_empty) {
            snprintf(buf, sizeof(buf), "%u", cfg->battery_empty_mv);
            lv_textarea_set_text(s_ta_battery_empty, buf);
        }

        if (s_ta_battery_full) {
            snprintf(buf, sizeof(buf), "%u", cfg->battery_full_mv);
            lv_textarea_set_text(s_ta_battery_full, buf);
        }

        if (s_ta_battery_offset) {
            snprintf(buf, sizeof(buf), "%d", cfg->battery_offset_mv);
            lv_textarea_set_text(s_ta_battery_offset, buf);
        }
    }
}

static void settings_battery_save_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    settings_hide_keyboard();

    if (!s_ta_battery_empty || !s_ta_battery_full || !s_ta_battery_offset) {
        return;
    }

    int empty_mv = settings_text_to_int(
        lv_textarea_get_text(s_ta_battery_empty),
        3300
    );

    int full_mv = settings_text_to_int(
        lv_textarea_get_text(s_ta_battery_full),
        4200
    );

    int offset_mv = settings_text_to_int(
        lv_textarea_get_text(s_ta_battery_offset),
        0
    );

    /*
     * 合法性检查范围根据单节锂电池常见电压设置。
     * 如果后续更换电池类型，需要同步调整这里和 battery_monitor.c。
     */
    if (empty_mv < 2500 || empty_mv > 4200) {
        app_ui_update_status("空电电压无效");
        return;
    }

    if (full_mv < 3500 || full_mv > 4500) {
        app_ui_update_status("满电电压无效");
        return;
    }

    if (full_mv <= empty_mv) {
        app_ui_update_status("满电需大于空电");
        return;
    }

    if (offset_mv < -1000 || offset_mv > 1000) {
        app_ui_update_status("偏移超范围");
        return;
    }

    esp_err_t ret = app_settings_set_battery_calibration(
        (uint16_t)empty_mv,
        (uint16_t)full_mv,
        (int16_t)offset_mv
    );

    if (ret == ESP_OK) {
        app_ui_update_status("电量校准已保存");
        settings_refresh_home_values();
        settings_show_home();
    } else {
        app_ui_update_status("电量校准失败");
    }
}


/* -------------------------------------------------------------------------- */
/* Settings page: common widget creation helpers                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 创建设置首页中的一行设置项。
 *
 * 每行包含左侧名称和右侧当前值。
 * 点击整行时触发传入的事件回调。
 *
 * @param parent 父对象。
 * @param name 左侧设置项名称。
 * @param value 右侧当前值。
 * @param cb 点击回调，可为 NULL。
 *
 * @return 右侧 value label 对象，调用方可保存用于后续刷新。
 */
static lv_obj_t *settings_create_row(lv_obj_t *parent,
                                     const char *name,
                                     const char *value,
                                     lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    make_clean_obj(btn, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(btn, LV_PCT(94), 20);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *label_name = lv_label_create(btn);
    lv_label_set_text(label_name, name ? name : "");
    label_set_color(label_name, UI_COLOR_WHITE);
    label_set_font(label_name, ui_font_status());
    lv_obj_align(label_name, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *label_value = lv_label_create(btn);
    lv_label_set_text(label_value, value ? value : "");
    label_set_color(label_value, UI_COLOR_ORANGE);
    label_set_font(label_value, ui_font_status());
    lv_obj_set_width(label_value, 145);
    lv_label_set_long_mode(label_value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label_value,
                                LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    lv_obj_align(label_value, LV_ALIGN_RIGHT_MID, -8, 0);

    return label_value;
}

/**
 * @brief 创建设置页中的通用操作按钮。
 *
 * @param parent 父对象。
 * @param text 按钮文字。
 * @param bg 背景色。
 * @param fg 文字色。
 * @param cb 点击回调，可为 NULL。
 *
 * @return 创建的按钮对象。
 */
static lv_obj_t *settings_create_action_button(lv_obj_t *parent,
                                               const char *text,
                                               lv_color_t bg,
                                               lv_color_t fg,
                                               lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    make_clean_obj(btn, bg, LV_OPA_COVER);
    lv_obj_set_size(btn, LV_PCT(42), 30);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text ? text : "");
    label_set_color(label, fg);
    label_set_font(label, ui_font_status());
    lv_obj_center(label);

    return btn;
}

/* -------------------------------------------------------------------------- */
/* Settings page: home                                                        */
/* -------------------------------------------------------------------------- */

static void create_settings_home(lv_obj_t *parent)
{
    s_settings_home = lv_obj_create(parent);
    make_clean_obj(s_settings_home, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_settings_home, LV_PCT(100), 198);
    lv_obj_align(s_settings_home, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_set_style_pad_top(s_settings_home, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_settings_home, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_settings_home, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_settings_home, 4, LV_PART_MAIN);

    lv_obj_set_flex_flow(s_settings_home, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_settings_home,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_set_scroll_dir(s_settings_home, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_settings_home, LV_SCROLLBAR_MODE_OFF);

    const app_settings_t *cfg = app_settings_get();

    s_label_setting_fmo_value = settings_create_row(
        s_settings_home,
        "FMO地址",
        cfg ? cfg->fmo_host : "--",
        settings_fmo_open_event_cb
    );

    s_label_setting_callsign_value = settings_create_row(
        s_settings_home,
        "本机呼号",
        (cfg && cfg->owner_callsign[0]) ?
            cfg->owner_callsign : APP_DEFAULT_OWNER_CALLSIGN,
        settings_callsign_open_event_cb
    );

    char vol_buf[16];
    snprintf(vol_buf, sizeof(vol_buf), "%u%%", cfg ? cfg->audio_volume : 0);

    s_label_setting_volume_value = settings_create_row(
        s_settings_home,
        "音量",
        vol_buf,
        settings_volume_open_event_cb
    );

    char bl_buf[16];
    snprintf(bl_buf, sizeof(bl_buf), "%u%%", cfg ? cfg->backlight_percent : 80);

    s_label_setting_backlight_value = settings_create_row(
        s_settings_home,
        "背光",
        bl_buf,
        settings_backlight_open_event_cb
    );

    s_label_setting_rotate_value = settings_create_row(
        s_settings_home,
        "屏幕旋转",
        (cfg && cfg->screen_rotate_180) ? "180°" : "正常",
        settings_rotate_toggle_event_cb
    );

    s_label_setting_wifi_value = settings_create_row(
        s_settings_home,
        "WiFi设置",
        cfg ? cfg->wifi_ssid : "--",
        settings_wifi_open_event_cb
    );

    char batt_buf[32];
    snprintf(batt_buf,
             sizeof(batt_buf),
             "%u/%umV",
             cfg ? cfg->battery_empty_mv : 3300,
             cfg ? cfg->battery_full_mv : 4200);

    s_label_setting_battery_value = settings_create_row(
        s_settings_home,
        "电量校准",
        batt_buf,
        settings_battery_open_event_cb
    );

    s_label_setting_station_value = settings_create_row(
        s_settings_home,
        "服务器列表",
        "全部服务器",
        settings_station_open_event_cb
    );

    char qso_buf[24];

    if (cfg && cfg->qso_count_valid) {
        snprintf(qso_buf,
                 sizeof(qso_buf),
                 "%lu",
                 (unsigned long)cfg->qso_count);
    } else {
        snprintf(qso_buf, sizeof(qso_buf), "未同步");
    }

    s_label_setting_qso_value = settings_create_row(
        s_settings_home,
        "QSO同步",
        qso_buf,
        settings_qso_sync_event_cb
    );

    s_label_setting_power_save_value = settings_create_row(
        s_settings_home,
        "节能模式",
        app_power_save_is_manual_enabled() ? "开" : "关",
        settings_power_save_toggle_event_cb
    );

    /*
     * 底部操作按钮：
     * - 返回：关闭设置页
     * - 保存重启：重启设备，使部分配置重新初始化后生效
     */
    lv_obj_t *bottom_row = lv_obj_create(s_settings_home);
    make_clean_obj(bottom_row, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(bottom_row, LV_PCT(94), 34);
    lv_obj_set_flex_flow(bottom_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom_row,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    settings_create_action_button(bottom_row,
                                  "返回",
                                  UI_COLOR_PANEL,
                                  UI_COLOR_WHITE,
                                  settings_close_event_cb);

    settings_create_action_button(bottom_row,
                                  "保存重启",
                                  UI_COLOR_ORANGE,
                                  UI_COLOR_BG,
                                  settings_restart_event_cb);
}

/* -------------------------------------------------------------------------- */
/* Settings page: FMO address page                                             */
/* -------------------------------------------------------------------------- */

static void create_settings_fmo_page(lv_obj_t *parent)
{
    s_settings_fmo_page = lv_obj_create(parent);
    make_clean_obj(s_settings_fmo_page, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_settings_fmo_page, LV_PCT(100), 198);
    lv_obj_align(s_settings_fmo_page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_settings_fmo_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_fmo_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_settings_fmo_page);
    lv_label_set_text(title, "FMO IP地址");
    label_set_color(title, UI_COLOR_ORANGE);
    label_set_font(title, ui_font_status());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_ta_fmo_host = lv_textarea_create(s_settings_fmo_page);
    make_clean_obj(s_ta_fmo_host, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(s_ta_fmo_host, LV_PCT(88), 34);
    lv_obj_align(s_ta_fmo_host, LV_ALIGN_TOP_MID, 0, 28);
    lv_textarea_set_one_line(s_ta_fmo_host, true);
    lv_textarea_set_max_length(s_ta_fmo_host, 63);
    lv_textarea_set_placeholder_text(s_ta_fmo_host, "IP或域名");

    lv_obj_set_style_text_color(s_ta_fmo_host, UI_COLOR_WHITE, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ta_fmo_host, ui_font_status(), LV_PART_MAIN);
    lv_obj_set_style_radius(s_ta_fmo_host, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ta_fmo_host, 6, LV_PART_MAIN);

    lv_obj_t *hint = lv_label_create(s_settings_fmo_page);
    lv_label_set_text(hint, "自动生成 /audio /events /ws");
    label_set_color(hint, UI_COLOR_GRAY);
    label_set_font(hint, ui_font_status());
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 66);

    lv_obj_t *btn_back = settings_create_action_button(
        s_settings_fmo_page,
        "返回",
        UI_COLOR_PANEL,
        UI_COLOR_WHITE,
        settings_back_home_event_cb
    );
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 18, 78);

    lv_obj_t *btn_save = settings_create_action_button(
        s_settings_fmo_page,
        "保存",
        UI_COLOR_ORANGE,
        UI_COLOR_BG,
        settings_fmo_save_event_cb
    );
    lv_obj_align(btn_save, LV_ALIGN_TOP_RIGHT, -18, 78);

    lv_obj_t *btn_restart = settings_create_action_button(
        s_settings_fmo_page,
        "重启生效",
        UI_COLOR_PANEL,
        UI_COLOR_ORANGE,
        settings_restart_event_cb
    );
    lv_obj_set_width(btn_restart, LV_PCT(88));
    lv_obj_align(btn_restart, LV_ALIGN_TOP_MID, 0, 126);
}

/* -------------------------------------------------------------------------- */
/* Settings page: owner callsign page                                          */
/* -------------------------------------------------------------------------- */

static void create_settings_callsign_page(lv_obj_t *parent)
{
    s_settings_callsign_page = lv_obj_create(parent);
    make_clean_obj(s_settings_callsign_page, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_settings_callsign_page, LV_PCT(100), 198);
    lv_obj_align(s_settings_callsign_page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_settings_callsign_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_callsign_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_settings_callsign_page);
    lv_label_set_text(title, "本机呼号");
    label_set_color(title, UI_COLOR_ORANGE);
    label_set_font(title, ui_font_status());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_ta_owner_callsign = lv_textarea_create(s_settings_callsign_page);
    make_clean_obj(s_ta_owner_callsign, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(s_ta_owner_callsign, LV_PCT(88), 34);
    lv_obj_align(s_ta_owner_callsign, LV_ALIGN_TOP_MID, 0, 34);
    lv_textarea_set_one_line(s_ta_owner_callsign, true);
    lv_textarea_set_max_length(s_ta_owner_callsign, 15);
    lv_textarea_set_placeholder_text(s_ta_owner_callsign,
                                     APP_DEFAULT_OWNER_CALLSIGN);

    lv_obj_set_style_text_color(s_ta_owner_callsign,
                                UI_COLOR_WHITE,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ta_owner_callsign,
                               ui_font_status(),
                               LV_PART_MAIN);
    lv_obj_set_style_radius(s_ta_owner_callsign, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ta_owner_callsign, 6, LV_PART_MAIN);

    lv_obj_add_event_cb(s_ta_owner_callsign,
                        settings_ta_text_focus_event_cb,
                        LV_EVENT_FOCUSED,
                        NULL);
    lv_obj_add_event_cb(s_ta_owner_callsign,
                        settings_ta_text_focus_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *hint = lv_label_create(s_settings_callsign_page);
    lv_label_set_text(hint, "用于待机时钟页显示");
    label_set_color(hint, UI_COLOR_GRAY);
    label_set_font(hint, ui_font_status());
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 74);

    lv_obj_t *btn_back = settings_create_action_button(
        s_settings_callsign_page,
        "返回",
        UI_COLOR_PANEL,
        UI_COLOR_WHITE,
        settings_back_home_event_cb
    );
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 18, 104);

    lv_obj_t *btn_save = settings_create_action_button(
        s_settings_callsign_page,
        "保存",
        UI_COLOR_ORANGE,
        UI_COLOR_BG,
        settings_callsign_save_event_cb
    );
    lv_obj_align(btn_save, LV_ALIGN_TOP_RIGHT, -18, 104);
}

/* -------------------------------------------------------------------------- */
/* Settings page: volume page                                                  */
/* -------------------------------------------------------------------------- */

static void create_settings_volume_page(lv_obj_t *parent)
{
    s_settings_volume_page = lv_obj_create(parent);
    make_clean_obj(s_settings_volume_page, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_settings_volume_page, LV_PCT(100), 198);
    lv_obj_align(s_settings_volume_page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_settings_volume_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_volume_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_settings_volume_page);
    lv_label_set_text(title, "音量设置");
    label_set_color(title, UI_COLOR_ORANGE);
    label_set_font(title, ui_font_status());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_label_volume_value = lv_label_create(s_settings_volume_page);
    lv_label_set_text(s_label_volume_value, "--%");
    label_set_color(s_label_volume_value, UI_COLOR_WHITE);
    label_set_font(s_label_volume_value, ui_font_cn());
    lv_obj_align(s_label_volume_value, LV_ALIGN_TOP_MID, 0, 34);

    s_slider_volume = lv_slider_create(s_settings_volume_page);
    lv_obj_set_size(s_slider_volume, LV_PCT(84), 16);
    lv_obj_align(s_slider_volume, LV_ALIGN_TOP_MID, 0, 70);
    lv_slider_set_range(s_slider_volume, 0, 100);
    lv_slider_set_value(s_slider_volume, 60, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(s_slider_volume,
                              UI_COLOR_PANEL,
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_slider_volume,
                            LV_OPA_COVER,
                            LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_volume,
                              UI_COLOR_ORANGE,
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_slider_volume,
                            LV_OPA_COVER,
                            LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_volume,
                              UI_COLOR_WHITE,
                              LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_slider_volume,
                            LV_OPA_COVER,
                            LV_PART_KNOB);

    lv_obj_add_event_cb(s_slider_volume,
                        settings_volume_slider_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        NULL);

    lv_obj_t *btn_back = settings_create_action_button(
        s_settings_volume_page,
        "返回",
        UI_COLOR_PANEL,
        UI_COLOR_WHITE,
        settings_back_home_event_cb
    );
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 18, 104);

    lv_obj_t *btn_save = settings_create_action_button(
        s_settings_volume_page,
        "保存",
        UI_COLOR_ORANGE,
        UI_COLOR_BG,
        settings_volume_save_event_cb
    );
    lv_obj_align(btn_save, LV_ALIGN_TOP_RIGHT, -18, 104);
}

/* -------------------------------------------------------------------------- */
/* Settings page: backlight page                                               */
/* -------------------------------------------------------------------------- */

static void create_settings_backlight_page(lv_obj_t *parent)
{
    s_settings_backlight_page = lv_obj_create(parent);
    make_clean_obj(s_settings_backlight_page, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_settings_backlight_page, LV_PCT(100), 198);
    lv_obj_align(s_settings_backlight_page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_settings_backlight_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_backlight_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_settings_backlight_page);
    lv_label_set_text(title, "背光设置");
    label_set_color(title, UI_COLOR_ORANGE);
    label_set_font(title, ui_font_status());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_label_backlight_value = lv_label_create(s_settings_backlight_page);
    lv_label_set_text(s_label_backlight_value, "--%");
    label_set_color(s_label_backlight_value, UI_COLOR_WHITE);
    label_set_font(s_label_backlight_value, ui_font_cn());
    lv_obj_align(s_label_backlight_value, LV_ALIGN_TOP_MID, 0, 34);

    s_slider_backlight = lv_slider_create(s_settings_backlight_page);
    lv_obj_set_size(s_slider_backlight, LV_PCT(84), 16);
    lv_obj_align(s_slider_backlight, LV_ALIGN_TOP_MID, 0, 70);
    lv_slider_set_range(s_slider_backlight, 5, 100);
    lv_slider_set_value(s_slider_backlight, 80, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(s_slider_backlight,
                              UI_COLOR_PANEL,
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_slider_backlight,
                            LV_OPA_COVER,
                            LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_backlight,
                              UI_COLOR_ORANGE,
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_slider_backlight,
                            LV_OPA_COVER,
                            LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_backlight,
                              UI_COLOR_WHITE,
                              LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_slider_backlight,
                            LV_OPA_COVER,
                            LV_PART_KNOB);

    lv_obj_add_event_cb(s_slider_backlight,
                        settings_backlight_slider_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        NULL);

    lv_obj_t *btn_back = settings_create_action_button(
        s_settings_backlight_page,
        "返回",
        UI_COLOR_PANEL,
        UI_COLOR_WHITE,
        settings_back_home_event_cb
    );
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 18, 104);

    lv_obj_t *btn_save = settings_create_action_button(
        s_settings_backlight_page,
        "保存",
        UI_COLOR_ORANGE,
        UI_COLOR_BG,
        settings_backlight_save_event_cb
    );
    lv_obj_align(btn_save, LV_ALIGN_TOP_RIGHT, -18, 104);
}

/* -------------------------------------------------------------------------- */
/* Settings page: WiFi page                                                    */
/* -------------------------------------------------------------------------- */

static void create_settings_wifi_page(lv_obj_t *parent)
{
    s_settings_wifi_page = lv_obj_create(parent);
    make_clean_obj(s_settings_wifi_page, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_settings_wifi_page, LV_PCT(100), 198);
    lv_obj_align(s_settings_wifi_page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_settings_wifi_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_wifi_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_settings_wifi_page);
    lv_label_set_text(title, "WiFi设置");
    label_set_color(title, UI_COLOR_ORANGE);
    label_set_font(title, ui_font_status());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_ta_wifi_ssid = lv_textarea_create(s_settings_wifi_page);
    make_clean_obj(s_ta_wifi_ssid, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(s_ta_wifi_ssid, LV_PCT(88), 30);
    lv_obj_align(s_ta_wifi_ssid, LV_ALIGN_TOP_MID, 0, 26);
    lv_textarea_set_one_line(s_ta_wifi_ssid, true);
    lv_textarea_set_max_length(s_ta_wifi_ssid, 31);
    lv_textarea_set_placeholder_text(s_ta_wifi_ssid, "SSID");

    lv_obj_set_style_text_color(s_ta_wifi_ssid, UI_COLOR_WHITE, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ta_wifi_ssid, ui_font_status(), LV_PART_MAIN);
    lv_obj_set_style_radius(s_ta_wifi_ssid, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ta_wifi_ssid, 6, LV_PART_MAIN);

    lv_obj_add_event_cb(s_ta_wifi_ssid,
                        settings_ta_text_focus_event_cb,
                        LV_EVENT_FOCUSED,
                        NULL);
    lv_obj_add_event_cb(s_ta_wifi_ssid,
                        settings_ta_text_focus_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    s_ta_wifi_password = lv_textarea_create(s_settings_wifi_page);
    make_clean_obj(s_ta_wifi_password, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(s_ta_wifi_password, LV_PCT(88), 30);
    lv_obj_align(s_ta_wifi_password, LV_ALIGN_TOP_MID, 0, 62);
    lv_textarea_set_one_line(s_ta_wifi_password, true);
    lv_textarea_set_password_mode(s_ta_wifi_password, true);
    lv_textarea_set_max_length(s_ta_wifi_password, 63);
    lv_textarea_set_placeholder_text(s_ta_wifi_password, "密码");

    lv_obj_set_style_text_color(s_ta_wifi_password,
                                UI_COLOR_WHITE,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ta_wifi_password,
                               ui_font_status(),
                               LV_PART_MAIN);
    lv_obj_set_style_radius(s_ta_wifi_password, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ta_wifi_password, 6, LV_PART_MAIN);

    lv_obj_add_event_cb(s_ta_wifi_password,
                        settings_ta_text_focus_event_cb,
                        LV_EVENT_FOCUSED,
                        NULL);
    lv_obj_add_event_cb(s_ta_wifi_password,
                        settings_ta_text_focus_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *btn_scan = settings_create_action_button(
        s_settings_wifi_page,
        "扫描",
        UI_COLOR_PANEL,
        UI_COLOR_ORANGE,
        settings_wifi_scan_open_event_cb
    );
    lv_obj_set_size(btn_scan, LV_PCT(88), 26);
    lv_obj_align(btn_scan, LV_ALIGN_TOP_MID, 0, 94);

    lv_obj_t *btn_back = settings_create_action_button(
        s_settings_wifi_page,
        "返回",
        UI_COLOR_PANEL,
        UI_COLOR_WHITE,
        settings_back_home_event_cb
    );
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 18, 136);

    lv_obj_t *btn_save = settings_create_action_button(
        s_settings_wifi_page,
        "保存",
        UI_COLOR_ORANGE,
        UI_COLOR_BG,
        settings_wifi_save_event_cb
    );
    lv_obj_align(btn_save, LV_ALIGN_TOP_RIGHT, -18, 136);

    lv_obj_t *hint = lv_label_create(s_settings_wifi_page);
    lv_label_set_text(hint, "保存后重启生效");
    label_set_color(hint, UI_COLOR_GRAY);
    label_set_font(hint, ui_font_status());
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 162);
}

/* -------------------------------------------------------------------------- */
/* Settings page: WiFi scan page                                               */
/* -------------------------------------------------------------------------- */

static void create_settings_wifi_scan_page(lv_obj_t *parent)
{
    s_settings_wifi_scan_page = lv_obj_create(parent);
    make_clean_obj(s_settings_wifi_scan_page, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_settings_wifi_scan_page, LV_PCT(100), 198);
    lv_obj_align(s_settings_wifi_scan_page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_settings_wifi_scan_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_wifi_scan_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_settings_wifi_scan_page);
    lv_label_set_text(title, "扫描WiFi");
    label_set_color(title, UI_COLOR_ORANGE);
    label_set_font(title, ui_font_status());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    s_label_wifi_scan_status = lv_label_create(s_settings_wifi_scan_page);
    lv_label_set_text(s_label_wifi_scan_status, "--");
    label_set_color(s_label_wifi_scan_status, UI_COLOR_GRAY);
    label_set_font(s_label_wifi_scan_status, ui_font_status());
    lv_obj_align(s_label_wifi_scan_status, LV_ALIGN_TOP_RIGHT, -10, 4);

    for (int i = 0; i < WIFI_SCAN_MENU_MAX_ITEMS; i++) {
        s_wifi_scan_item_btns[i] = lv_btn_create(s_settings_wifi_scan_page);
        make_clean_obj(s_wifi_scan_item_btns[i],
                       UI_COLOR_PANEL,
                       LV_OPA_COVER);

        lv_obj_set_size(s_wifi_scan_item_btns[i], LV_PCT(90), 22);
        lv_obj_align(s_wifi_scan_item_btns[i],
                     LV_ALIGN_TOP_MID,
                     0,
                     24 + i * 24);
        lv_obj_set_style_radius(s_wifi_scan_item_btns[i], 4, LV_PART_MAIN);

        lv_obj_add_event_cb(s_wifi_scan_item_btns[i],
                            settings_wifi_scan_item_event_cb,
                            LV_EVENT_CLICKED,
                            NULL);

        s_wifi_scan_item_labels[i] =
            lv_label_create(s_wifi_scan_item_btns[i]);

        lv_label_set_text(s_wifi_scan_item_labels[i], "--");
        label_set_color(s_wifi_scan_item_labels[i], UI_COLOR_WHITE);
        label_set_font(s_wifi_scan_item_labels[i], ui_font_status());

        lv_obj_set_width(s_wifi_scan_item_labels[i], LV_PCT(96));
        lv_label_set_long_mode(s_wifi_scan_item_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_wifi_scan_item_labels[i],
                                    LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_center(s_wifi_scan_item_labels[i]);

        lv_obj_add_flag(s_wifi_scan_item_btns[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *btn_back = lv_btn_create(s_settings_wifi_scan_page);
    make_clean_obj(btn_back, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(btn_back, 70, 24);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_obj_set_style_radius(btn_back, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back,
                        settings_wifi_scan_back_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "返回");
    label_set_color(lbl_back, UI_COLOR_WHITE);
    label_set_font(lbl_back, ui_font_status());
    lv_obj_center(lbl_back);

    lv_obj_t *btn_refresh = lv_btn_create(s_settings_wifi_scan_page);
    make_clean_obj(btn_refresh, UI_COLOR_ORANGE, LV_OPA_COVER);
    lv_obj_set_size(btn_refresh, 70, 24);
    lv_obj_align(btn_refresh, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    lv_obj_set_style_radius(btn_refresh, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_refresh,
                        settings_wifi_scan_open_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *lbl_refresh = lv_label_create(btn_refresh);
    lv_label_set_text(lbl_refresh, "刷新");
    label_set_color(lbl_refresh, UI_COLOR_BG);
    label_set_font(lbl_refresh, ui_font_status());
    lv_obj_center(lbl_refresh);
}

/* -------------------------------------------------------------------------- */
/* Settings page: battery calibration page                                     */
/* -------------------------------------------------------------------------- */

static void create_settings_battery_page(lv_obj_t *parent)
{
    s_settings_battery_page = lv_obj_create(parent);
    make_clean_obj(s_settings_battery_page, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_settings_battery_page, LV_PCT(100), 198);
    lv_obj_align(s_settings_battery_page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_settings_battery_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_battery_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_settings_battery_page);
    lv_label_set_text(title, "电量校准");
    label_set_color(title, UI_COLOR_ORANGE);
    label_set_font(title, ui_font_status());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *label_empty = lv_label_create(s_settings_battery_page);
    lv_label_set_text(label_empty, "空电");
    label_set_color(label_empty, UI_COLOR_WHITE);
    label_set_font(label_empty, ui_font_status());
    lv_obj_align(label_empty, LV_ALIGN_TOP_LEFT, 18, 32);

    s_ta_battery_empty = lv_textarea_create(s_settings_battery_page);
    make_clean_obj(s_ta_battery_empty, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(s_ta_battery_empty, 82, 28);
    lv_obj_align(s_ta_battery_empty, LV_ALIGN_TOP_LEFT, 62, 26);
    lv_textarea_set_one_line(s_ta_battery_empty, true);
    lv_textarea_set_max_length(s_ta_battery_empty, 4);
    lv_textarea_set_placeholder_text(s_ta_battery_empty, "3300");

    lv_obj_set_style_text_color(s_ta_battery_empty,
                                UI_COLOR_WHITE,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ta_battery_empty,
                               ui_font_status(),
                               LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ta_battery_empty, 5, LV_PART_MAIN);

    lv_obj_add_event_cb(s_ta_battery_empty,
                        settings_ta_number_focus_event_cb,
                        LV_EVENT_FOCUSED,
                        NULL);
    lv_obj_add_event_cb(s_ta_battery_empty,
                        settings_ta_number_focus_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *label_full = lv_label_create(s_settings_battery_page);
    lv_label_set_text(label_full, "满电");
    label_set_color(label_full, UI_COLOR_WHITE);
    label_set_font(label_full, ui_font_status());
    lv_obj_align(label_full, LV_ALIGN_TOP_LEFT, 168, 32);

    s_ta_battery_full = lv_textarea_create(s_settings_battery_page);
    make_clean_obj(s_ta_battery_full, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(s_ta_battery_full, 82, 28);
    lv_obj_align(s_ta_battery_full, LV_ALIGN_TOP_LEFT, 212, 26);
    lv_textarea_set_one_line(s_ta_battery_full, true);
    lv_textarea_set_max_length(s_ta_battery_full, 4);
    lv_textarea_set_placeholder_text(s_ta_battery_full, "4200");

    lv_obj_set_style_text_color(s_ta_battery_full,
                                UI_COLOR_WHITE,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ta_battery_full,
                               ui_font_status(),
                               LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ta_battery_full, 5, LV_PART_MAIN);

    lv_obj_add_event_cb(s_ta_battery_full,
                        settings_ta_number_focus_event_cb,
                        LV_EVENT_FOCUSED,
                        NULL);
    lv_obj_add_event_cb(s_ta_battery_full,
                        settings_ta_number_focus_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *label_offset = lv_label_create(s_settings_battery_page);
    lv_label_set_text(label_offset, "偏移");
    label_set_color(label_offset, UI_COLOR_WHITE);
    label_set_font(label_offset, ui_font_status());
    lv_obj_align(label_offset, LV_ALIGN_TOP_LEFT, 18, 70);

    s_ta_battery_offset = lv_textarea_create(s_settings_battery_page);
    make_clean_obj(s_ta_battery_offset, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(s_ta_battery_offset, 82, 28);
    lv_obj_align(s_ta_battery_offset, LV_ALIGN_TOP_LEFT, 62, 64);
    lv_textarea_set_one_line(s_ta_battery_offset, true);
    lv_textarea_set_max_length(s_ta_battery_offset, 5);
    lv_textarea_set_placeholder_text(s_ta_battery_offset, "0");

    lv_obj_set_style_text_color(s_ta_battery_offset,
                                UI_COLOR_WHITE,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ta_battery_offset,
                               ui_font_status(),
                               LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ta_battery_offset, 5, LV_PART_MAIN);

    lv_obj_add_event_cb(s_ta_battery_offset,
                        settings_ta_number_focus_event_cb,
                        LV_EVENT_FOCUSED,
                        NULL);
    lv_obj_add_event_cb(s_ta_battery_offset,
                        settings_ta_number_focus_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *unit = lv_label_create(s_settings_battery_page);
    lv_label_set_text(unit, "单位:mV  偏移可正负");
    label_set_color(unit, UI_COLOR_GRAY);
    label_set_font(unit, ui_font_status());
    lv_obj_align(unit, LV_ALIGN_TOP_RIGHT, -18, 70);

    lv_obj_t *btn_back = settings_create_action_button(
        s_settings_battery_page,
        "返回",
        UI_COLOR_PANEL,
        UI_COLOR_WHITE,
        settings_back_home_event_cb
    );
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 18, 100);

    lv_obj_t *btn_save = settings_create_action_button(
        s_settings_battery_page,
        "保存",
        UI_COLOR_ORANGE,
        UI_COLOR_BG,
        settings_battery_save_event_cb
    );
    lv_obj_align(btn_save, LV_ALIGN_TOP_RIGHT, -18, 100);
}

/* -------------------------------------------------------------------------- */
/* Settings page: station list page                                            */
/* -------------------------------------------------------------------------- */

static void create_settings_station_page(lv_obj_t *parent)
{
    s_settings_station_page = lv_obj_create(parent);
    make_clean_obj(s_settings_station_page, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_settings_station_page, LV_PCT(100), 198);
    lv_obj_align(s_settings_station_page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_settings_station_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_station_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_settings_station_page);
    lv_label_set_text(title, "所有站点");
    label_set_color(title, UI_COLOR_ORANGE);
    label_set_font(title, ui_font_status());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    s_label_station_page_status = lv_label_create(s_settings_station_page);
    lv_label_set_text(s_label_station_page_status, "--");
    label_set_color(s_label_station_page_status, UI_COLOR_GRAY);
    label_set_font(s_label_station_page_status, ui_font_status());
    lv_obj_align(s_label_station_page_status, LV_ALIGN_TOP_RIGHT, -10, 4);

    for (int i = 0; i < STATION_MENU_PAGE_SIZE; i++) {
        s_station_item_uids[i] = -1;

        s_station_item_btns[i] = lv_btn_create(s_settings_station_page);
        make_clean_obj(s_station_item_btns[i],
                       UI_COLOR_PANEL,
                       LV_OPA_COVER);

        lv_obj_set_size(s_station_item_btns[i], LV_PCT(90), 22);
        lv_obj_align(s_station_item_btns[i],
                     LV_ALIGN_TOP_MID,
                     0,
                     24 + i * 24);
        lv_obj_set_style_radius(s_station_item_btns[i], 4, LV_PART_MAIN);

        lv_obj_add_event_cb(s_station_item_btns[i],
                            settings_station_item_event_cb,
                            LV_EVENT_CLICKED,
                            NULL);

        s_station_item_labels[i] = lv_label_create(s_station_item_btns[i]);
        lv_label_set_text(s_station_item_labels[i], "--");
        label_set_color(s_station_item_labels[i], UI_COLOR_WHITE);
        label_set_font(s_station_item_labels[i], ui_font_cn());

        lv_obj_set_width(s_station_item_labels[i], LV_PCT(96));
        lv_label_set_long_mode(s_station_item_labels[i],
                               LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(s_station_item_labels[i],
                                    LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_center(s_station_item_labels[i]);

        lv_obj_add_flag(s_station_item_btns[i], LV_OBJ_FLAG_HIDDEN);
    }

    /*
     * 底部操作按钮：上一页 / 刷新 / 下一页。
     */
    s_station_btn_prev = lv_btn_create(s_settings_station_page);
    make_clean_obj(s_station_btn_prev, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(s_station_btn_prev, 58, 24);
    lv_obj_align(s_station_btn_prev, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_obj_set_style_radius(s_station_btn_prev, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_station_btn_prev,
                        settings_station_prev_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *lbl_prev = lv_label_create(s_station_btn_prev);
    lv_label_set_text(lbl_prev, "上一页");
    label_set_color(lbl_prev, UI_COLOR_WHITE);
    label_set_font(lbl_prev, ui_font_status());
    lv_obj_center(lbl_prev);

    lv_obj_t *btn_refresh = lv_btn_create(s_settings_station_page);
    make_clean_obj(btn_refresh, UI_COLOR_ORANGE, LV_OPA_COVER);
    lv_obj_set_size(btn_refresh, 58, 24);
    lv_obj_align(btn_refresh, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_radius(btn_refresh, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_refresh,
                        settings_station_refresh_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *lbl_refresh = lv_label_create(btn_refresh);
    lv_label_set_text(lbl_refresh, "刷新");
    label_set_color(lbl_refresh, UI_COLOR_BG);
    label_set_font(lbl_refresh, ui_font_status());
    lv_obj_center(lbl_refresh);

    s_station_btn_next = lv_btn_create(s_settings_station_page);
    make_clean_obj(s_station_btn_next, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(s_station_btn_next, 58, 24);
    lv_obj_align(s_station_btn_next, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    lv_obj_set_style_radius(s_station_btn_next, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_station_btn_next,
                        settings_station_next_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *lbl_next = lv_label_create(s_station_btn_next);
    lv_label_set_text(lbl_next, "下一页");
    label_set_color(lbl_next, UI_COLOR_WHITE);
    label_set_font(lbl_next, ui_font_status());
    lv_obj_center(lbl_next);
}

/* -------------------------------------------------------------------------- */
/* Settings page: keyboard                                                     */
/* -------------------------------------------------------------------------- */

static void create_settings_keyboard(lv_obj_t *parent)
{
#if LV_USE_KEYBOARD
    s_settings_keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(s_settings_keyboard, LV_PCT(100), 82);
    lv_obj_align(s_settings_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_settings_keyboard, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_bg_color(s_settings_keyboard,
                              UI_COLOR_PANEL,
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_settings_keyboard,
                            LV_OPA_COVER,
                            LV_PART_MAIN);

    /*
     * 点击键盘上的确认/回车/OK 时隐藏键盘。
     */
    lv_obj_add_event_cb(s_settings_keyboard,
                        settings_keyboard_event_cb,
                        LV_EVENT_READY,
                        NULL);

    /*
     * 点击键盘上的取消/关闭时隐藏键盘。
     */
    lv_obj_add_event_cb(s_settings_keyboard,
                        settings_keyboard_event_cb,
                        LV_EVENT_CANCEL,
                        NULL);
#else
    s_settings_keyboard = NULL;
    (void)parent;
#endif
}

/* -------------------------------------------------------------------------- */
/* Settings page: root container                                               */
/* -------------------------------------------------------------------------- */

static void create_settings_page(lv_obj_t *parent)
{
    s_settings_page = lv_obj_create(parent);
    make_clean_obj(s_settings_page, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_settings_page, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_settings_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_settings_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_page, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 顶部栏。
     */
    lv_obj_t *top = lv_obj_create(s_settings_page);
    make_clean_obj(top, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(top, LV_PCT(100), 42);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_back = lv_btn_create(top);
    make_clean_obj(btn_back, UI_COLOR_BG, LV_OPA_TRANSP);
    lv_obj_set_size(btn_back, 44, 34);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(btn_back,
                        settings_close_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *img_back = lv_img_create(btn_back);
    lv_img_set_src(img_back, &ui_icon_back);
    img_set_color(img_back, UI_COLOR_WHITE);
    lv_obj_center(img_back);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "设置");
    label_set_color(title, UI_COLOR_WHITE);
    label_set_font(title, ui_font_cn());
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /*
     * 右上角版本号。
     */
    lv_obj_t *label_version = lv_label_create(top);
    lv_label_set_text(label_version, APP_VERSION_TEXT);
    label_set_color(label_version, UI_COLOR_ORANGE);
    label_set_font(label_version, ui_font_status());
    lv_obj_align(label_version, LV_ALIGN_RIGHT_MID, -8, 0);

    /*
     * 子页面统一挂载在设置页根对象下。
     */
    create_settings_home(s_settings_page);
    create_settings_fmo_page(s_settings_page);
    create_settings_callsign_page(s_settings_page);
    create_settings_volume_page(s_settings_page);
    create_settings_backlight_page(s_settings_page);
    create_settings_wifi_page(s_settings_page);
    create_settings_wifi_scan_page(s_settings_page);
    create_settings_battery_page(s_settings_page);
    create_settings_station_page(s_settings_page);
    create_settings_keyboard(s_settings_page);

    settings_show_home();
}

/* -------------------------------------------------------------------------- */
/* Idle clock: creation helpers                                                */
/* -------------------------------------------------------------------------- */

static void app_ui_idle_clock_attach_events(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }

    /*
     * 必须设置 CLICKABLE，才能收到 CLICKED / LONG_PRESSED 事件。
     */
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    /*
     * 普通点击：
     * - 普通待机时钟页：退出时钟页
     * - 省电时钟页：不退出，只提示状态
     */
    lv_obj_add_event_cb(obj,
                        app_ui_idle_clock_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    /*
     * 长按：
     * - 手动省电时钟页：退出手动省电
     * - 低电量省电时钟页：不允许退出
     */
    lv_obj_add_event_cb(obj,
                        app_ui_idle_clock_long_press_event_cb,
                        LV_EVENT_LONG_PRESSED,
                        NULL);

    /*
     * 有些触摸驱动更容易触发 LONG_PRESSED_REPEAT。
     * 长按回调内部会根据状态判断，重复触发也不会改变逻辑。
     */
    lv_obj_add_event_cb(obj,
                        app_ui_idle_clock_long_press_event_cb,
                        LV_EVENT_LONG_PRESSED_REPEAT,
                        NULL);
}

static void create_idle_clock_page(lv_obj_t *parent)
{
    s_idle_clock_page = lv_obj_create(parent);
    make_clean_obj(s_idle_clock_page, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_idle_clock_page, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_idle_clock_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_idle_clock_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_idle_clock_page, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 整个待机时钟页都响应点击和长按。
     */
    app_ui_idle_clock_attach_events(s_idle_clock_page);

    /*
     * 上部 60%：显示本机呼号。
     */
    s_idle_clock_top_area = lv_obj_create(s_idle_clock_page);
    make_clean_obj(s_idle_clock_top_area, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_idle_clock_top_area, LV_PCT(100), LV_PCT(60));
    lv_obj_align(s_idle_clock_top_area, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(s_idle_clock_top_area, LV_OBJ_FLAG_SCROLLABLE);

    app_ui_idle_clock_attach_events(s_idle_clock_top_area);

    s_label_idle_clock_callsign = lv_label_create(s_idle_clock_top_area);

    const app_settings_t *cfg = app_settings_get();
    const char *idle_call =
        (cfg && cfg->owner_callsign[0]) ?
            cfg->owner_callsign : APP_DEFAULT_OWNER_CALLSIGN;

    label_set_color(s_label_idle_clock_callsign, UI_COLOR_ORANGE);
    label_set_font(s_label_idle_clock_callsign, ui_font_clock());

    lv_obj_set_style_text_letter_space(s_label_idle_clock_callsign,
                                       2,
                                       LV_PART_MAIN);
    lv_obj_set_style_text_align(s_label_idle_clock_callsign,
                                LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    /*
     * 待机时钟呼号占满宽度。
     * 动态 letter_space 尽量让字符均匀铺开。
     * 极端超宽时使用 DOT 省略。
     */
    lv_obj_set_width(s_label_idle_clock_callsign, LV_PCT(100));
    lv_label_set_long_mode(s_label_idle_clock_callsign, LV_LABEL_LONG_DOT);

    idle_clock_callsign_apply_fit_style(idle_call);
    lv_label_set_text(s_label_idle_clock_callsign, idle_call);

    lv_obj_align(s_label_idle_clock_callsign, LV_ALIGN_CENTER, 0, 8);

    app_ui_idle_clock_attach_events(s_label_idle_clock_callsign);

    /*
     * 下部 40%：显示时间。
     */
    s_idle_clock_bottom_area = lv_obj_create(s_idle_clock_page);
    make_clean_obj(s_idle_clock_bottom_area, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_idle_clock_bottom_area, LV_PCT(100), LV_PCT(40));
    lv_obj_align(s_idle_clock_bottom_area, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(s_idle_clock_bottom_area, LV_OBJ_FLAG_SCROLLABLE);

    app_ui_idle_clock_attach_events(s_idle_clock_bottom_area);

    s_label_idle_clock_time = lv_label_create(s_idle_clock_bottom_area);
    lv_label_set_text(s_label_idle_clock_time, "--:--");
    label_set_color(s_label_idle_clock_time, UI_COLOR_WHITE);
    label_set_font(s_label_idle_clock_time, ui_font_EN_BIG());

    lv_obj_set_style_text_letter_space(s_label_idle_clock_time,
                                       5,
                                       LV_PART_MAIN);
    lv_obj_set_style_text_align(s_label_idle_clock_time,
                                LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    lv_obj_set_width(s_label_idle_clock_time, LV_PCT(100));
    lv_label_set_long_mode(s_label_idle_clock_time, LV_LABEL_LONG_DOT);
    lv_obj_align(s_label_idle_clock_time, LV_ALIGN_CENTER, 0, 4);

    app_ui_idle_clock_attach_events(s_label_idle_clock_time);
}

static void app_ui_refresh_idle_clock_callsign(void)
{
    if (!s_label_idle_clock_callsign) {
        return;
    }

    const app_settings_t *cfg = app_settings_get();

    const char *idle_call =
        (cfg && cfg->owner_callsign[0]) ?
            cfg->owner_callsign : APP_DEFAULT_OWNER_CALLSIGN;

    idle_clock_callsign_apply_fit_style(idle_call);

    set_label_text_safe(s_label_idle_clock_callsign,
                        idle_call,
                        APP_DEFAULT_OWNER_CALLSIGN);
}

/* -------------------------------------------------------------------------- */
/* Settings page: owner callsign events                                        */
/* -------------------------------------------------------------------------- */

static void settings_callsign_open_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    const app_settings_t *cfg = app_settings_get();

    settings_hide_keyboard();

    if (s_settings_home) {
        lv_obj_add_flag(s_settings_home, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_fmo_page) {
        lv_obj_add_flag(s_settings_fmo_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_callsign_page) {
        lv_obj_clear_flag(s_settings_callsign_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_volume_page) {
        lv_obj_add_flag(s_settings_volume_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_backlight_page) {
        lv_obj_add_flag(s_settings_backlight_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_wifi_page) {
        lv_obj_add_flag(s_settings_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_wifi_scan_page) {
        lv_obj_add_flag(s_settings_wifi_scan_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_battery_page) {
        lv_obj_add_flag(s_settings_battery_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_settings_station_page) {
        lv_obj_add_flag(s_settings_station_page, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_ta_owner_callsign) {
        lv_textarea_set_text(
            s_ta_owner_callsign,
            (cfg && cfg->owner_callsign[0]) ?
                cfg->owner_callsign : APP_DEFAULT_OWNER_CALLSIGN
        );
    }

    if (s_settings_keyboard && s_ta_owner_callsign) {
        lv_keyboard_set_textarea(s_settings_keyboard, s_ta_owner_callsign);

#if LV_USE_KEYBOARD
#if defined(LV_KEYBOARD_MODE_TEXT_UPPER)
        lv_keyboard_set_mode(s_settings_keyboard,
                             LV_KEYBOARD_MODE_TEXT_UPPER);
#else
        lv_keyboard_set_mode(s_settings_keyboard,
                             LV_KEYBOARD_MODE_TEXT_LOWER);
#endif
#endif

        lv_obj_clear_flag(s_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_settings_keyboard);
    }
}

static void settings_callsign_save_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    settings_hide_keyboard();

    if (!s_ta_owner_callsign) {
        return;
    }

    const char *callsign = lv_textarea_get_text(s_ta_owner_callsign);

    if (!callsign || callsign[0] == '\0') {
        app_ui_update_status("呼号不能为空");
        return;
    }

    if (strlen(callsign) >= 16) {
        app_ui_update_status("呼号过长");
        return;
    }

    esp_err_t ret = app_settings_set_owner_callsign(callsign);

    if (ret == ESP_OK) {
        app_ui_update_status("呼号已保存");

        /*
         * 本机呼号用于待机时钟页，保存后立即刷新。
         */
        app_ui_refresh_idle_clock_callsign();

        settings_refresh_home_values();
        settings_show_home();
    } else {
        app_ui_update_status("呼号保存失败");
    }
}

/* -------------------------------------------------------------------------- */
/* Main screen: top status area                                                */
/* -------------------------------------------------------------------------- */

static void create_top_status_area(lv_obj_t *parent)
{
    /*
     * 顶部居中：日期时间。
     */
    s_label_datetime = lv_label_create(parent);
    lv_label_set_text(s_label_datetime, "--:--");
    label_set_color(s_label_datetime, UI_COLOR_GRAY);
    label_set_font(s_label_datetime, ui_font_status());

    /*
     * 宽度给足，方便显示 YYYY-MM-DD HH:MM。
     */
    lv_obj_set_width(s_label_datetime, 180);
    lv_obj_set_style_text_align(s_label_datetime,
                                LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_label_datetime, LV_LABEL_LONG_DOT);

    /*
     * 顶部居中显示。
     */
    lv_obj_align(s_label_datetime, LV_ALIGN_TOP_MID, 0, 5);

    /*
     * 右上角：WiFi + 电池。
     */
    s_top_status_group = lv_obj_create(parent);
    make_clean_obj(s_top_status_group, UI_COLOR_BG, LV_OPA_TRANSP);
    lv_obj_set_size(s_top_status_group, 54, 24);
    lv_obj_align(s_top_status_group, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_clear_flag(s_top_status_group, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(s_top_status_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_top_status_group,
                          LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_set_style_pad_all(s_top_status_group, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_top_status_group, 5, LV_PART_MAIN);

    /*
     * WiFi 图标。
     */
    s_img_wifi_icon = lv_img_create(s_top_status_group);
    lv_img_set_src(s_img_wifi_icon, &ui_icon_wifi);
    img_set_color(s_img_wifi_icon, UI_COLOR_RED);

    /*
     * 电池图标。
     */
    s_img_battery_icon = lv_img_create(s_top_status_group);
    lv_img_set_src(s_img_battery_icon, &ui_icon_battery);
    img_set_color(s_img_battery_icon, UI_COLOR_WHITE);

    app_ui_datetime_update();
}

static void create_speaking_pill(lv_obj_t *parent)
{
    /*
     * 左上角 speaking 状态胶囊。
     * 默认隐藏；通联中显示绿色 RX。
     */
    s_speaking_pill = lv_obj_create(parent);
    make_clean_obj(s_speaking_pill, UI_COLOR_GREEN, LV_OPA_COVER);

    lv_obj_set_size(s_speaking_pill, 32, 14);
    lv_obj_align(s_speaking_pill, LV_ALIGN_TOP_LEFT, 7, 6);

    lv_obj_set_style_radius(s_speaking_pill, 8, LV_PART_MAIN);
    lv_obj_clear_flag(s_speaking_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_speaking_pill, LV_OBJ_FLAG_HIDDEN);

    s_label_speaking = lv_label_create(s_speaking_pill);

    /*
     * RX 比 ON AIR 更短，更适合小胶囊。
     */
    lv_label_set_text(s_label_speaking, "RX");
    label_set_color(s_label_speaking, UI_COLOR_BG);
    label_set_font(s_label_speaking, ui_font_status());
    lv_obj_center(s_label_speaking);
}

/* -------------------------------------------------------------------------- */
/* Main screen: current call and info area                                     */
/* -------------------------------------------------------------------------- */

static void create_top_area(lv_obj_t *parent)
{
    /*
     * 上部：当前呼号。
     * 右上角状态栏和左上角 RX 胶囊独立创建，覆盖在 root 上。
     */
    s_top_area = lv_obj_create(parent);
    make_clean_obj(s_top_area, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_top_area, LV_PCT(100), 112);
    lv_obj_align(s_top_area, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(s_top_area, LV_OBJ_FLAG_SCROLLABLE);

    s_label_current_call = lv_label_create(s_top_area);
    lv_label_set_text(s_label_current_call, "--");
    label_set_color(s_label_current_call, UI_COLOR_ORANGE);
    label_set_font(s_label_current_call, ui_font_EN_BIG());

    lv_obj_set_style_text_letter_space(s_label_current_call,
                                       2,
                                       LV_PART_MAIN);
    lv_obj_set_style_text_align(s_label_current_call,
                                LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    /*
     * 当前呼号占满屏幕宽度。
     * 动态 letter_space 会尽量让字符均匀铺开。
     * 极端超宽时使用 DOT 省略。
     */
    lv_obj_set_width(s_label_current_call, LV_PCT(100));
    lv_label_set_long_mode(s_label_current_call, LV_LABEL_LONG_DOT);

    /*
     * 稍微下移，避免和右上角日期状态栏冲突。
     */
    lv_obj_align(s_label_current_call, LV_ALIGN_CENTER, 0, 14);
}

static void create_info_row_area(lv_obj_t *parent)
{
    /*
     * 当前呼号下方、站点条上方的信息区域：
     * - 左侧：上次通联呼号
     * - 右侧：QSO 数量
     */
    s_info_row_area = lv_obj_create(parent);
    make_clean_obj(s_info_row_area, UI_COLOR_BG, LV_OPA_COVER);

    lv_obj_set_size(s_info_row_area, LV_PCT(92), 52);
    lv_obj_align(s_info_row_area, LV_ALIGN_TOP_MID, 0, 124);
    lv_obj_clear_flag(s_info_row_area, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 橙色细外框。
     */
    lv_obj_set_style_border_width(s_info_row_area, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_info_row_area,
                                  UI_COLOR_ORANGE,
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_info_row_area,
                                LV_OPA_COVER,
                                LV_PART_MAIN);
    lv_obj_set_style_radius(s_info_row_area, 3, LV_PART_MAIN);

    /*
     * 左侧：上次通联呼号。
     */
    s_label_last_call = lv_label_create(s_info_row_area);
    lv_label_set_text(s_label_last_call, "--");
    label_set_color(s_label_last_call, UI_COLOR_WHITE);
    label_set_font(s_label_last_call, ui_font_28());

    /*
     * 给右侧 QSO 区域留空间。
     */
    lv_obj_set_width(s_label_last_call, LV_PCT(60));
    lv_obj_set_style_text_align(s_label_last_call,
                                LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_label_last_call, LV_LABEL_LONG_DOT);
    lv_obj_align(s_label_last_call, LV_ALIGN_LEFT_MID, 8, 0);

    /*
     * 右侧：QSO 数量区域。
     */
    lv_obj_t *qso_box = lv_obj_create(s_info_row_area);
    make_clean_obj(qso_box, UI_COLOR_BG, LV_OPA_TRANSP);
    lv_obj_set_size(qso_box, 104, 42);
    lv_obj_align(qso_box, LV_ALIGN_RIGHT_MID, -7, 0);
    lv_obj_clear_flag(qso_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(qso_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(qso_box,
                          LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_set_style_pad_all(qso_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(qso_box, 5, LV_PART_MAIN);

    s_img_qso_icon = lv_img_create(qso_box);
    lv_img_set_src(s_img_qso_icon, &ui_icon_qso);
    img_set_color(s_img_qso_icon, UI_COLOR_ORANGE);

    s_label_qso_count = lv_label_create(qso_box);
    lv_label_set_text(s_label_qso_count, "0");
    label_set_color(s_label_qso_count, UI_COLOR_WHITE);
    label_set_font(s_label_qso_count, ui_font_cn());
}

/* -------------------------------------------------------------------------- */
/* Main screen: station bar and station popup                                  */
/* -------------------------------------------------------------------------- */

static void create_card_area(lv_obj_t *parent)
{
    /*
     * 橙色站点条：
     * 作为卡片下半部分，只显示当前站点名称。
     */
    s_card_area = lv_obj_create(parent);
    make_clean_obj(s_card_area, UI_COLOR_ORANGE, LV_OPA_COVER);

    lv_obj_add_flag(s_card_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_card_area,
                        main_station_popup_open_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_set_size(s_card_area, LV_PCT(92), 30);
    lv_obj_align(s_card_area, LV_ALIGN_TOP_MID, 0, 174);

    lv_obj_set_style_radius(s_card_area, 3, LV_PART_MAIN);
    lv_obj_clear_flag(s_card_area, LV_OBJ_FLAG_SCROLLABLE);

    s_station_text_box = lv_obj_create(s_card_area);
    make_clean_obj(s_station_text_box, UI_COLOR_ORANGE, LV_OPA_TRANSP);

    lv_obj_add_flag(s_station_text_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_station_text_box,
                        main_station_popup_open_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_set_size(s_station_text_box, LV_PCT(96), LV_PCT(100));
    lv_obj_align(s_station_text_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_station_text_box, LV_OBJ_FLAG_SCROLLABLE);

    s_label_station = lv_label_create(s_station_text_box);
    lv_label_set_text(s_label_station, "--");

    label_set_color(s_label_station, UI_COLOR_BG);
    label_set_font(s_label_station, ui_font_cn());

    lv_obj_add_flag(s_label_station, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_label_station,
                        main_station_popup_open_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_set_width(s_label_station, LV_PCT(100));
    lv_obj_set_style_text_align(s_label_station,
                                LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_label_station, 0, LV_PART_MAIN);

    /*
     * 站点名一行显示，超出省略。
     */
    lv_label_set_long_mode(s_label_station, LV_LABEL_LONG_DOT);

    lv_obj_align(s_label_station, LV_ALIGN_CENTER, 0, 0);
}

static void create_main_station_popup(lv_obj_t *parent)
{
    s_main_station_popup = lv_obj_create(parent);
    make_clean_obj(s_main_station_popup, UI_COLOR_BG, LV_OPA_COVER);

    lv_obj_set_size(s_main_station_popup, 270, 198);
    lv_obj_align(s_main_station_popup, LV_ALIGN_CENTER, 0, 4);
    lv_obj_add_flag(s_main_station_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_main_station_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_border_width(s_main_station_popup, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_main_station_popup,
                                  UI_COLOR_ORANGE,
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_main_station_popup,
                                LV_OPA_COVER,
                                LV_PART_MAIN);
    lv_obj_set_style_radius(s_main_station_popup, 4, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_main_station_popup);
    lv_label_set_text(title, "收藏站点");
    label_set_color(title, UI_COLOR_ORANGE);
    label_set_font(title, ui_font_status());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    s_label_main_station_popup_status =
        lv_label_create(s_main_station_popup);
    lv_label_set_text(s_label_main_station_popup_status, "--");
    label_set_color(s_label_main_station_popup_status, UI_COLOR_GRAY);
    label_set_font(s_label_main_station_popup_status, ui_font_status());
    lv_obj_align(s_label_main_station_popup_status,
                 LV_ALIGN_TOP_RIGHT,
                 -8,
                 4);

    for (int i = 0; i < STATION_MENU_PAGE_SIZE; i++) {
        s_main_station_item_uids[i] = -1;

        s_main_station_item_btns[i] = lv_btn_create(s_main_station_popup);
        make_clean_obj(s_main_station_item_btns[i],
                       UI_COLOR_PANEL,
                       LV_OPA_COVER);

        lv_obj_set_size(s_main_station_item_btns[i], LV_PCT(90), 22);
        lv_obj_align(s_main_station_item_btns[i],
                     LV_ALIGN_TOP_MID,
                     0,
                     24 + i * 24);
        lv_obj_set_style_radius(s_main_station_item_btns[i],
                                4,
                                LV_PART_MAIN);

        lv_obj_add_event_cb(s_main_station_item_btns[i],
                            main_station_popup_item_event_cb,
                            LV_EVENT_CLICKED,
                            NULL);

        s_main_station_item_labels[i] =
            lv_label_create(s_main_station_item_btns[i]);

        lv_label_set_text(s_main_station_item_labels[i], "--");
        label_set_color(s_main_station_item_labels[i], UI_COLOR_WHITE);
        label_set_font(s_main_station_item_labels[i], ui_font_cn());

        lv_obj_set_width(s_main_station_item_labels[i], LV_PCT(96));
        lv_label_set_long_mode(s_main_station_item_labels[i],
                               LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_main_station_item_labels[i],
                                    LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_center(s_main_station_item_labels[i]);

        lv_obj_add_flag(s_main_station_item_btns[i], LV_OBJ_FLAG_HIDDEN);
    }

    /*
     * 底部按钮：关闭 / 上一页 / 刷新 / 下一页。
     */
    lv_obj_t *btn_close = lv_btn_create(s_main_station_popup);
    make_clean_obj(btn_close, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(btn_close, 54, 24);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_obj_set_style_radius(btn_close, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_close,
                        main_station_popup_close_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "关闭");
    label_set_color(lbl_close, UI_COLOR_WHITE);
    label_set_font(lbl_close, ui_font_status());
    lv_obj_center(lbl_close);

    s_main_station_btn_prev = lv_btn_create(s_main_station_popup);
    make_clean_obj(s_main_station_btn_prev, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(s_main_station_btn_prev, 54, 24);
    lv_obj_align(s_main_station_btn_prev, LV_ALIGN_BOTTOM_MID, -58, -4);
    lv_obj_set_style_radius(s_main_station_btn_prev, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_main_station_btn_prev,
                        main_station_popup_prev_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *lbl_prev = lv_label_create(s_main_station_btn_prev);
    lv_label_set_text(lbl_prev, "上一页");
    label_set_color(lbl_prev, UI_COLOR_WHITE);
    label_set_font(lbl_prev, ui_font_status());
    lv_obj_center(lbl_prev);

    lv_obj_t *btn_refresh = lv_btn_create(s_main_station_popup);
    make_clean_obj(btn_refresh, UI_COLOR_ORANGE, LV_OPA_COVER);
    lv_obj_set_size(btn_refresh, 54, 24);
    lv_obj_align(btn_refresh, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_radius(btn_refresh, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_refresh,
                        main_station_popup_refresh_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *lbl_refresh = lv_label_create(btn_refresh);
    lv_label_set_text(lbl_refresh, "刷新");
    label_set_color(lbl_refresh, UI_COLOR_BG);
    label_set_font(lbl_refresh, ui_font_status());
    lv_obj_center(lbl_refresh);

    s_main_station_btn_next = lv_btn_create(s_main_station_popup);
    make_clean_obj(s_main_station_btn_next, UI_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_size(s_main_station_btn_next, 54, 24);
    lv_obj_align(s_main_station_btn_next, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    lv_obj_set_style_radius(s_main_station_btn_next, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_main_station_btn_next,
                        main_station_popup_next_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *lbl_next = lv_label_create(s_main_station_btn_next);
    lv_label_set_text(lbl_next, "下一页");
    label_set_color(lbl_next, UI_COLOR_WHITE);
    label_set_font(lbl_next, ui_font_status());
    lv_obj_center(lbl_next);
}

/* -------------------------------------------------------------------------- */
/* Main screen: QSO sync popup                                                 */
/* -------------------------------------------------------------------------- */

static void create_qso_sync_popup(lv_obj_t *parent)
{
    s_qso_sync_popup = lv_obj_create(parent);
    make_clean_obj(s_qso_sync_popup, UI_COLOR_BG, LV_OPA_COVER);

    lv_obj_set_size(s_qso_sync_popup, 220, 92);
    lv_obj_align(s_qso_sync_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_qso_sync_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_qso_sync_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_border_width(s_qso_sync_popup, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_qso_sync_popup,
                                  UI_COLOR_ORANGE,
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_qso_sync_popup,
                                LV_OPA_COVER,
                                LV_PART_MAIN);
    lv_obj_set_style_radius(s_qso_sync_popup, 6, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_qso_sync_popup);
    lv_label_set_text(title, "QSO同步");
    label_set_color(title, UI_COLOR_ORANGE);
    label_set_font(title, ui_font_status());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    s_label_qso_sync_popup = lv_label_create(s_qso_sync_popup);
    lv_label_set_text(s_label_qso_sync_popup, "--");
    label_set_color(s_label_qso_sync_popup, UI_COLOR_WHITE);
    label_set_font(s_label_qso_sync_popup, ui_font_cn());

    lv_obj_set_width(s_label_qso_sync_popup, LV_PCT(90));
    lv_obj_set_style_text_align(s_label_qso_sync_popup,
                                LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_label_qso_sync_popup, LV_LABEL_LONG_DOT);
    lv_obj_align(s_label_qso_sync_popup, LV_ALIGN_CENTER, 0, 14);
}

/* -------------------------------------------------------------------------- */
/* Main screen: bottom area                                                    */
/* -------------------------------------------------------------------------- */

static void create_bottom_area(lv_obj_t *parent)
{
    /*
     * 底部：
     * - 左侧：节能按钮
     * - 中间：状态信息
     * - 右侧：静音 + 设置按钮
     */
    s_bottom_area = lv_obj_create(parent);
    make_clean_obj(s_bottom_area, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_bottom_area, LV_PCT(100), LV_PCT(15));
    lv_obj_align(s_bottom_area, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(s_bottom_area, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 左侧节能按钮。
     * 当前只显示图标，不显示文字 label。
     */
    s_btn_power_save = lv_btn_create(s_bottom_area);
    make_clean_obj(s_btn_power_save, UI_COLOR_BG, LV_OPA_TRANSP);
    lv_obj_set_size(s_btn_power_save, 36, 30);
    lv_obj_align(s_btn_power_save, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_clear_flag(s_btn_power_save, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 去掉按钮所有可见背景/边框/阴影，只保留点击区域。
     */
    lv_obj_set_style_bg_opa(s_btn_power_save, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_btn_power_save, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_btn_power_save, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_btn_power_save, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_power_save, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_btn_power_save, 0, LV_PART_MAIN);

    lv_obj_add_event_cb(s_btn_power_save,
                        power_save_button_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    s_img_power_save_icon = lv_img_create(s_btn_power_save);
    lv_img_set_src(s_img_power_save_icon, &ui_icon_power_save);
    img_set_color(s_img_power_save_icon, UI_COLOR_ORANGE);
    lv_obj_center(s_img_power_save_icon);

    /*
     * 底部状态信息居中。
     * 左侧节能按钮约 60px，右侧两个按钮约 78px。
     * 这里给状态文字 170px，居中对齐。
     */
    s_label_bottom_status = lv_label_create(s_bottom_area);
    lv_label_set_text(s_label_bottom_status, "--");
    label_set_color(s_label_bottom_status, UI_COLOR_WHITE);
    label_set_font(s_label_bottom_status, ui_font_status());

    lv_obj_set_width(s_label_bottom_status, 170);
    lv_obj_set_style_text_align(s_label_bottom_status,
                                LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_label_bottom_status, LV_LABEL_LONG_DOT);
    lv_obj_align(s_label_bottom_status, LV_ALIGN_CENTER, 0, 0);

    /*
     * 静音按钮。
     * 放在设置按钮左侧。
     */
    s_btn_mute = lv_btn_create(s_bottom_area);
    make_clean_obj(s_btn_mute, UI_COLOR_BG, LV_OPA_TRANSP);
    lv_obj_set_size(s_btn_mute, 30, 30);
    lv_obj_align(s_btn_mute, LV_ALIGN_RIGHT_MID, -42, 0);

    lv_obj_set_style_radius(s_btn_mute, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_btn_mute, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_btn_mute, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_btn_mute, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_btn_mute, 0, LV_PART_MAIN);

    lv_obj_add_event_cb(s_btn_mute,
                        mute_button_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    s_img_mute_icon = lv_img_create(s_btn_mute);
    lv_img_set_src(s_img_mute_icon, &ui_icon_mute);
    img_set_color(s_img_mute_icon, UI_COLOR_GRAY);
    lv_obj_center(s_img_mute_icon);

    mute_button_update_style();

    /*
     * 设置按钮。
     */
    s_btn_settings = lv_btn_create(s_bottom_area);
    make_clean_obj(s_btn_settings, UI_COLOR_BG, LV_OPA_TRANSP);
    lv_obj_set_size(s_btn_settings, 36, 30);
    lv_obj_align(s_btn_settings, LV_ALIGN_RIGHT_MID, -6, 0);

    lv_obj_set_style_bg_opa(s_btn_settings, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_btn_settings, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_btn_settings, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_btn_settings, 0, LV_PART_MAIN);

    lv_obj_add_event_cb(s_btn_settings,
                        settings_open_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    s_img_settings_icon = lv_img_create(s_btn_settings);
    lv_img_set_src(s_img_settings_icon, &ui_icon_settings);
    img_set_color(s_img_settings_icon, UI_COLOR_WHITE);
    lv_obj_center(s_img_settings_icon);

    /*
     * 根据当前节能状态刷新按钮样式。
     */
    power_save_button_update_style();
}

/* -------------------------------------------------------------------------- */
/* Idle clock: runtime logic                                                   */
/* -------------------------------------------------------------------------- */

static void app_ui_idle_clock_update(void)
{
    if (!s_idle_clock_active || !s_label_idle_clock_time) {
        return;
    }

    char buf[24];

    app_ui_format_clock_text(buf, sizeof(buf));
    lv_label_set_text(s_label_idle_clock_time, buf);
}

static void app_ui_show_idle_clock(void)
{
    if (s_idle_clock_active) {
        return;
    }

    if (!s_idle_clock_page) {
        return;
    }

    /*
     * 进入待机时钟页时，关闭设置页和键盘，回到主界面模式。
     */
    settings_hide_keyboard();

    if (s_settings_page) {
        lv_obj_add_flag(s_settings_page, LV_OBJ_FLAG_HIDDEN);
    }

    s_idle_clock_active = true;

    app_ui_idle_clock_update();

    lv_obj_clear_flag(s_idle_clock_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_idle_clock_page);

    ESP_LOGI(TAG, "idle clock show");
}

static void app_ui_hide_idle_clock(void)
{
    if (!s_idle_clock_active) {
        return;
    }

    s_idle_clock_active = false;

    if (s_idle_clock_page) {
        lv_obj_add_flag(s_idle_clock_page, LV_OBJ_FLAG_HIDDEN);
    }

    /*
     * 重置 LVGL inactive time，避免刚退出又立刻进入时钟页。
     */
#if LVGL_VERSION_MAJOR >= 8
    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
        lv_disp_trig_activity(disp);
    }
#else
    lv_disp_trig_activity(NULL);
#endif

    ESP_LOGI(TAG, "idle clock hide");
}

void app_ui_enter_power_save_clock(void)
{
    s_power_save_clock_active = true;

    settings_hide_keyboard();

    if (s_settings_page) {
        lv_obj_add_flag(s_settings_page, LV_OBJ_FLAG_HIDDEN);
    }

    /*
     * 刷新呼号，避免用户刚修改后没有同步到待机时钟页。
     */
    app_ui_refresh_idle_clock_callsign();

    app_ui_show_idle_clock();

    app_ui_update_status("省电模式");

    power_save_button_update_style();
}

void app_ui_exit_power_save_clock(void)
{
    s_power_save_clock_active = false;

    app_ui_hide_idle_clock();

    app_ui_update_status("正常模式");

    power_save_button_update_style();
}

bool app_ui_is_power_save_clock(void)
{
    return s_power_save_clock_active;
}

static void app_ui_idle_clock_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    /*
     * 省电模式下，普通点击不退出。
     */
    if (s_power_save_clock_active) {
        if (app_power_save_has_reason(APP_POWER_SAVE_REASON_LOW_BATTERY)) {
            app_ui_update_status("低电量省电");
        } else {
            app_ui_update_status("长按退出省电");
        }

        return;
    }

    /*
     * 普通待机时钟页，点击返回主界面。
     */
    app_ui_hide_idle_clock();
}

static void app_ui_idle_clock_long_press_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    ESP_LOGI(TAG,
             "idle clock long press, power_save=%d manual=%d low_batt=%d",
             s_power_save_clock_active ? 1 : 0,
             app_power_save_has_reason(APP_POWER_SAVE_REASON_MANUAL) ? 1 : 0,
             app_power_save_has_reason(APP_POWER_SAVE_REASON_LOW_BATTERY) ? 1 : 0);

    if (!s_power_save_clock_active) {
        return;
    }

    /*
     * 低电量触发的省电模式不允许手动退出。
     */
    if (app_power_save_has_reason(APP_POWER_SAVE_REASON_LOW_BATTERY)) {
        app_ui_update_status("低电量省电中");
        return;
    }

    /*
     * 如果当前不是手动省电，不处理。
     * 这样可以防止 LONG_PRESSED_REPEAT 重复触发造成状态混乱。
     */
    if (!app_power_save_has_reason(APP_POWER_SAVE_REASON_MANUAL)) {
        return;
    }

    /*
     * 只关闭手动省电原因。
     * 具体退出流程由 app_power_save 模块统一处理。
     */
    app_power_save_set_reason(APP_POWER_SAVE_REASON_MANUAL, false);
}

/* -------------------------------------------------------------------------- */
/* UI creation                                                                 */
/* -------------------------------------------------------------------------- */

void app_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    /*
     * 清空屏幕，避免旧 UI 或 LVGL 默认对象残留。
     */
    lv_obj_clean(scr);

    /*
     * 屏幕本身设置黑色。
     * 这可以避免橘色圆角卡片四角露白。
     */
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    s_root = lv_obj_create(scr);
    make_clean_obj(s_root, UI_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 主界面基础区域。
     */
    create_top_area(s_root);
    create_info_row_area(s_root);
    create_card_area(s_root);
    create_bottom_area(s_root);

    /*
     * 顶部覆盖元素放后面创建，避免被 top_area 覆盖。
     */
    create_speaking_pill(s_root);
    create_top_status_area(s_root);

    /*
     * 主界面弹窗与覆盖页面。
     */
    create_main_station_popup(s_root);
    create_settings_page(s_root);
    create_idle_clock_page(s_root);
    create_qso_sync_popup(s_root);

    ESP_LOGI(TAG, "app ui created");

    const app_settings_t *cfg = app_settings_get();

    /*
     * 应用背光设置。
     */
    if (cfg) {
        app_ui_set_backlight_percent(cfg->backlight_percent);
    } else {
        app_ui_set_backlight_percent(80);
    }

    /*
     * 应用屏幕旋转设置。
     */
    if (cfg) {
        app_ui_apply_screen_rotation(cfg->screen_rotate_180);
    }

    /*
     * 初始化主界面默认显示。
     */
    app_ui_update_talker(NULL);
    app_ui_update_last_call(NULL);
    app_ui_update_station(NULL);
    app_ui_update_battery(0, false);
    app_ui_update_wifi_rssi(-127);
    app_ui_update_voice_level(0);

    if (cfg && cfg->qso_count_valid) {
        app_ui_update_qso_count(cfg->qso_count);
    } else {
        app_ui_update_qso_count(0);
    }

    /*
     * 右上角日期时间刷新。
     */
    lv_timer_create(app_ui_datetime_timer_cb, 1000, NULL);

    /*
     * 普通待机检测 timer。
     */
    if (!s_idle_timer) {
        s_idle_timer = lv_timer_create(app_ui_idle_timer_cb,
                                       APP_UI_IDLE_TIMER_PERIOD_MS,
                                       NULL);
    }

    /*
     * 待机时钟页时间刷新 timer。
     */
    if (!s_idle_clock_timer) {
        s_idle_clock_timer = lv_timer_create(app_ui_idle_clock_timer_cb,
                                             1000,
                                             NULL);
    }

    /*
     * 开机默认静音：
     * Audio WS 不自动连接。
     */
    if (cfg && cfg->audio_volume > 0 && cfg->audio_volume <= 100) {
        s_audio_volume_before_mute = cfg->audio_volume;
    } else {
        s_audio_volume_before_mute = DEFAULT_AUDIO_VOLUME;
    }

    s_audio_muted = true;
    mute_button_update_style();
}

/* -------------------------------------------------------------------------- */
/* QSO sync popup                                                              */
/* -------------------------------------------------------------------------- */

static void qso_sync_popup_auto_close_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    app_ui_qso_sync_popup_close();
}

void app_ui_qso_sync_popup_show(const char *text, uint32_t auto_close_ms)
{
    if (!s_qso_sync_popup || !s_label_qso_sync_popup) {
        return;
    }

    const char *msg = (text && text[0]) ? text : "--";

    set_label_text_safe(s_label_qso_sync_popup, msg, "--");

    lv_obj_clear_flag(s_qso_sync_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_qso_sync_popup);

    /*
     * 如果已有自动关闭 timer，先删除。
     */
    if (s_qso_sync_popup_timer) {
        lv_timer_del(s_qso_sync_popup_timer);
        s_qso_sync_popup_timer = NULL;
    }

    if (auto_close_ms > 0) {
        s_qso_sync_popup_timer = lv_timer_create(qso_sync_popup_auto_close_cb,
                                                 auto_close_ms,
                                                 NULL);

        /*
         * 只执行一次。
         */
        lv_timer_set_repeat_count(s_qso_sync_popup_timer, 1);
    }
}

void app_ui_qso_sync_popup_close(void)
{
    if (s_qso_sync_popup_timer) {
        lv_timer_del(s_qso_sync_popup_timer);
        s_qso_sync_popup_timer = NULL;
    }

    if (s_qso_sync_popup) {
        lv_obj_add_flag(s_qso_sync_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

/* -------------------------------------------------------------------------- */
/* Public update interfaces: status and calls                                  */
/* -------------------------------------------------------------------------- */

void app_ui_update_status(const char *text)
{
    if (!s_label_bottom_status) {
        return;
    }

    const char *new_text = (text && text[0]) ? text : "..";
    const char *old_text = lv_label_get_text(s_label_bottom_status);

    if (old_text && strcmp(old_text, new_text) == 0) {
        return;
    }

    lv_label_set_text(s_label_bottom_status, new_text);
}

void app_ui_update_talker(const char *talker)
{
    /*
     * 兼容旧接口：
     * 只有 talker 有内容时才认为正在说话。
     * 避免 app_ui_update_talker(NULL) 导致开机显示 RX。
     */
    app_ui_update_talker_state(
        talker,
        (talker && talker[0]) ? true : false
    );
}

void app_ui_update_talker_state(const char *talker, bool active)
{
    if (!s_label_current_call) {
        return;
    }

    const char *text = (talker && talker[0]) ? talker : "^o^";

    current_call_apply_fit_style(text);
    set_label_text_safe(s_label_current_call, text, "..");

    /*
     * 正在说话：
     * - 当前呼号橙色
     * - 左上角 RX 胶囊显示
     *
     * 未说话：
     * - 当前呼号灰色
     * - 左上角 RX 胶囊隐藏
     */
    if (active) {
        label_set_color(s_label_current_call, UI_COLOR_ORANGE);

        if (s_speaking_pill) {
            lv_obj_clear_flag(s_speaking_pill, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        label_set_color(s_label_current_call, UI_COLOR_GRAY);

        if (s_speaking_pill) {
            lv_obj_add_flag(s_speaking_pill, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void app_ui_update_last_call(const char *callsign)
{
    set_label_text_safe(s_label_last_call, callsign, "--");
}

void app_ui_update_station(const char *station)
{
    set_label_text_safe(s_label_station, station, "--");

    /*
     * 站点名当前使用一行省略模式。
     * 文本更新后重新对齐，保证短文本和省略文本都居中。
     */
    if (s_station_text_box && s_label_station) {
        lv_obj_update_layout(s_station_text_box);
        lv_obj_update_layout(s_label_station);
        lv_obj_align(s_label_station, LV_ALIGN_CENTER, 0, 0);
    }
}

void app_ui_update_qso_count(uint32_t count)
{
    char buf[16];

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)count);

    /*
     * 主界面 QSO 数量始终显示数字。
     */
    if (s_label_qso_count) {
        const char *old_text = lv_label_get_text(s_label_qso_count);
        if (!old_text || strcmp(old_text, buf) != 0) {
            lv_label_set_text(s_label_qso_count, buf);
        }
    }

    /*
     * 设置页 QSO 行：
     * - 未同步：显示“未同步”
     * - 已同步：显示数量
     */
    if (s_label_setting_qso_value) {
        const app_settings_t *cfg = app_settings_get();

        char setting_buf[24];

        if (cfg && cfg->qso_count_valid) {
            snprintf(setting_buf,
                     sizeof(setting_buf),
                     "%lu",
                     (unsigned long)cfg->qso_count);
        } else {
            snprintf(setting_buf, sizeof(setting_buf), "未同步");
        }

        const char *old_text = lv_label_get_text(s_label_setting_qso_value);
        if (!old_text || strcmp(old_text, setting_buf) != 0) {
            lv_label_set_text(s_label_setting_qso_value, setting_buf);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Public update interfaces: WiFi and battery                                  */
/* -------------------------------------------------------------------------- */

void app_ui_update_wifi_rssi(int rssi_dbm)
{
    s_wifi_rssi = rssi_dbm;

    int percent = wifi_rssi_to_percent(rssi_dbm);

    if (s_img_wifi_icon) {
        if (percent >= 60) {
            img_set_color(s_img_wifi_icon, UI_COLOR_GREEN);
        } else if (percent >= 30) {
            img_set_color(s_img_wifi_icon, UI_COLOR_ORANGE);
        } else {
            img_set_color(s_img_wifi_icon, UI_COLOR_RED);
        }
    }
}

void app_ui_update_battery(uint8_t percent, bool charging)
{
    if (percent > 100) {
        percent = 100;
    }

    s_battery_percent = percent;
    s_battery_charging = charging;

    /*
     * 只保留电池图标三色显示：
     * - 充电：绿色
     * - 低电：红色
     * - 中电：橙色
     * - 正常：绿色
     */
    if (s_img_battery_icon) {
        if (charging) {
            img_set_color(s_img_battery_icon, UI_COLOR_GREEN);
        } else if (percent <= 15) {
            img_set_color(s_img_battery_icon, UI_COLOR_RED);
        } else if (percent <= 35) {
            img_set_color(s_img_battery_icon, UI_COLOR_ORANGE);
        } else {
            img_set_color(s_img_battery_icon, UI_COLOR_GREEN);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Public update interfaces: audio level and backlight                         */
/* -------------------------------------------------------------------------- */

void app_ui_update_voice_level(uint8_t level)
{
    /*
     * 顶部音频波动条已取消。
     * 保留接口，避免 audio_ws/ui_async 等旧代码调用出错。
     */
    (void)level;
}

void app_ui_set_voice_level_pending(uint8_t level)
{
    /*
     * 顶部音频波动条已取消。
     * 保留接口，避免旧代码调用出错。
     */
    (void)level;
}

void app_ui_set_backlight_percent(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    /*
     * 记录目标亮度。
     * 如果当前处于待机或熄屏状态，未来可根据状态计算实际输出值。
     */
    s_backlight_target_percent = percent;

    app_ui_backlight_apply_raw(percent);
}

/* -------------------------------------------------------------------------- */
/* Idle clock: timeout and timers                                              */
/* -------------------------------------------------------------------------- */

static uint32_t app_ui_get_idle_clock_timeout_ms(void)
{
    const app_settings_t *cfg = app_settings_get();

    if (!cfg) {
        return APP_UI_IDLE_CLOCK_DEFAULT_MS;
    }

    if (!cfg->idle_image_enabled) {
        /*
         * 禁用待机时钟。
         */
        return UINT32_MAX;
    }

    uint32_t timeout = cfg->idle_image_timeout_ms;

    /*
     * 防止配置过小导致频繁进入待机时钟页。
     */
    if (timeout < 10000) {
        timeout = 10000;
    }

    return timeout;
}

static void app_ui_idle_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    uint32_t timeout = app_ui_get_idle_clock_timeout_ms();

    if (timeout == UINT32_MAX) {
        return;
    }

    uint32_t inactive_ms = lv_disp_get_inactive_time(NULL);

    /*
     * 如果时钟页已经显示，用户触摸后 inactive_ms 会变小。
     * 这里做一次兜底恢复。
     */
    if (s_idle_clock_active) {
        /*
         * 省电模式下不允许普通触摸退出。
         */
        if (s_power_save_clock_active) {
            return;
        }

        if (inactive_ms < 1000) {
            app_ui_hide_idle_clock();
        }

        return;
    }

    if (inactive_ms >= timeout) {
        app_ui_show_idle_clock();
    }
}

static void app_ui_idle_clock_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    app_ui_idle_clock_update();
}

/* -------------------------------------------------------------------------- */
/* Compatibility interfaces                                                    */
/* -------------------------------------------------------------------------- */

void app_ui_update_wifi_status(bool connected, int rssi)
{
    if (connected) {
        app_ui_update_wifi_rssi(rssi);
    } else {
        app_ui_update_wifi_rssi(-127);
    }
}

void app_ui_set_backlight(uint8_t percent)
{
    app_ui_set_backlight_percent(percent);
}

void app_ui_update_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }

    /*
     * 新版主界面暂时不单独显示音量。
     * 保留该接口用于兼容旧 main.c 或其他模块。
     */
    ESP_LOGI(TAG, "volume percent: %u", volume);
}
