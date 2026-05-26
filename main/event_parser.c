/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file event_parser.c
 * @brief Event WebSocket JSON 解析实现。
 *
 * 本模块解析来自 Event WebSocket 或 Station WebSocket 转发的 qso JSON。
 *
 * 当前处理逻辑：
 * - qso/callsign：
 *   - isSpeaking=true：更新当前通联呼号、唤醒待机页、打开音频门控；
 *   - isSpeaking=false：结束当前通联，将当前呼号更新为上次通联；
 *
 * - qso/history：
 *   - 当前忽略，不再用 history 更新“上次通联”；
 *
 * - qso/getListResponse：
 *   - 解析 QSO 列表页码、数量、latest log id 和 log id 列表；
 *   - 转交 audio_ws_qso_count_handle_response() 处理同步状态机。
 */

#include "event_parser.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Third-party headers ------------------------------------------------------ */
#include "cJSON.h"

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_log.h"

/* Project headers ---------------------------------------------------------- */
#include "audio_ws.h"
#include "ui_async.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "event_parser";

/* -------------------------------------------------------------------------- */
/* Private macros                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief 当前通联呼号缓存长度。
 */
#define EVENT_CURRENT_CALLSIGN_MAX_LEN  32

/**
 * @brief QSO getListResponse 中最多提取的 logId 数量。
 *
 * 当前 QSO 同步 pageSize 通常为 20，因此 24 足够使用。
 */
#define EVENT_QSO_LOG_ID_MAX_COUNT      24

/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief 当前正在通联/说话的呼号缓存。
 *
 * 现在不再依赖 qso/history 更新“上次通联”。
 *
 * 当前逻辑：
 *
 * callsign isSpeaking=true:
 * - s_current_qso_callsign = callsign
 *
 * callsign isSpeaking=false:
 * - ui_async_update_last_call(s_current_qso_callsign)
 */
static char s_current_qso_callsign[EVENT_CURRENT_CALLSIGN_MAX_LEN] = {0};

/**
 * @brief 当前是否处于通联活跃状态。
 */
static bool s_current_qso_active = false;

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static const char *json_get_string(cJSON *obj, const char *key);
static bool json_get_bool(cJSON *obj, const char *key, bool def_val);

static void event_parser_cache_current_callsign(const char *callsign);
static void event_parser_finish_current_qso(const char *fallback_callsign);

static void event_parser_handle_callsign_event(const char *callsign,
                                               const char *grid,
                                               bool is_speaking);

static void event_parser_handle_qso_get_list_response(cJSON *root);

/* -------------------------------------------------------------------------- */
/* JSON helpers                                                               */
/* -------------------------------------------------------------------------- */

static const char *json_get_string(cJSON *obj, const char *key)
{
    if (!obj || !key) {
        return NULL;
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);

    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }

    return NULL;
}

static bool json_get_bool(cJSON *obj, const char *key, bool def_val)
{
    if (!obj || !key) {
        return def_val;
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);

    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }

    return def_val;
}

/* -------------------------------------------------------------------------- */
/* Current QSO callsign cache                                                 */
/* -------------------------------------------------------------------------- */

static void event_parser_cache_current_callsign(const char *callsign)
{
    if (!callsign || callsign[0] == '\0') {
        return;
    }

    snprintf(s_current_qso_callsign,
             sizeof(s_current_qso_callsign),
             "%s",
             callsign);
}

static void event_parser_finish_current_qso(const char *fallback_callsign)
{
    const char *last_call = NULL;

    /*
     * 优先使用缓存的当前通联呼号。
     */
    if (s_current_qso_callsign[0]) {
        last_call = s_current_qso_callsign;
    } else if (fallback_callsign && fallback_callsign[0]) {
        /*
         * 兜底：
         * 如果没有缓存，但结束事件里带了 callsign，就使用事件里的。
         */
        last_call = fallback_callsign;
    }

    if (last_call && last_call[0]) {
        ui_async_update_last_call(last_call);

        ESP_LOGI(TAG, "last callsign from current qso=%s", last_call);

        /*
         * 当前呼号变成非活跃状态。
         *
         * 如果希望通联结束后顶部直接显示默认空闲符号，
         * 可以改为：
         *
         * ui_async_update_talker_state(NULL, false);
         */
        ui_async_update_talker_state(last_call, false);
    } else {
        ui_async_update_talker_state(NULL, false);
    }

    s_current_qso_active = false;
}

/* -------------------------------------------------------------------------- */
/* qso/callsign handling                                                      */
/* -------------------------------------------------------------------------- */

static void event_parser_handle_callsign_event(const char *callsign,
                                               const char *grid,
                                               bool is_speaking)
{
    if (is_speaking) {
        if (!callsign || callsign[0] == '\0') {
            ESP_LOGW(TAG, "speaking event without callsign");
            return;
        }

        /*
         * 如果上一轮仍处于 active，但新呼号不同，
         * 说明可能没收到上一轮 isSpeaking=false。
         *
         * 此时把旧呼号作为上次通联，避免丢失。
         */
        if (s_current_qso_active &&
            s_current_qso_callsign[0] &&
            strcmp(s_current_qso_callsign, callsign) != 0) {

            ui_async_update_last_call(s_current_qso_callsign);

            ESP_LOGW(TAG,
                     "new speaking before previous end, previous=%s, new=%s",
                     s_current_qso_callsign,
                     callsign);
        }

        event_parser_cache_current_callsign(callsign);
        s_current_qso_active = true;

        /*
         * isSpeaking=true：
         * - 唤醒待机时钟页；
         * - 打开音频门控；
         * - 更新当前呼号为活跃状态。
         */
        ui_async_wake_from_idle();

        audio_ws_set_speaking(true);

        ui_async_update_talker_state(s_current_qso_callsign, true);
        ui_async_update_status("通联中");

        ESP_LOGD(TAG,
                 "speaking callsign=%s grid=%s",
                 callsign,
                 grid ? grid : "");

        return;
    }

    /*
     * isSpeaking=false：
     * 当前通联结束，把当前呼号转为上次通联。
     */
    audio_ws_set_speaking(false);

    event_parser_finish_current_qso(callsign);

    ui_async_update_status("空闲");
}

/* -------------------------------------------------------------------------- */
/* Public JSON entry                                                          */
/* -------------------------------------------------------------------------- */

void event_parser_handle_json(const char *json, int len)
{
    if (!json || len <= 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed, len=%d", len);
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *subType = cJSON_GetObjectItem(root, "subType");
    cJSON *data = cJSON_GetObjectItem(root, "data");

    if (!cJSON_IsString(type) ||
        !cJSON_IsString(subType) ||
        !data) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "qso") == 0) {
        if (strcmp(subType->valuestring, "callsign") == 0 &&
            cJSON_IsObject(data)) {

            const char *callsign = json_get_string(data, "callsign");
            const char *grid = json_get_string(data, "grid");
            bool is_speaking = json_get_bool(data, "isSpeaking", false);

            event_parser_handle_callsign_event(callsign,
                                               grid,
                                               is_speaking);
        } else if (strcmp(subType->valuestring, "history") == 0) {
            /*
             * 不再使用 history 更新“上次通联”。
             *
             * 上次通联现在由 callsign isSpeaking=false 生成。
             * 这样可以减少 history JSON 带来的解析和 UI 更新开销。
             */
            ESP_LOGD(TAG, "ignore qso history");
        } else if (strcmp(subType->valuestring, "getListResponse") == 0) {
            event_parser_handle_qso_get_list_response(root);
        }
    }

    cJSON_Delete(root);
}

/* -------------------------------------------------------------------------- */
/* qso/getListResponse handling                                               */
/* -------------------------------------------------------------------------- */

static void event_parser_handle_qso_get_list_response(cJSON *root)
{
    if (!root) {
        return;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        ESP_LOGW(TAG,
                 "qso getListResponse code=%d",
                 code->valueint);
        return;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        ESP_LOGW(TAG, "qso getListResponse data missing/not object");
        return;
    }

    cJSON *page_item = cJSON_GetObjectItem(data, "page");
    cJSON *page_size_item = cJSON_GetObjectItem(data, "pageSize");
    cJSON *count_item = cJSON_GetObjectItem(data, "count");
    cJSON *list = cJSON_GetObjectItem(data, "list");

    int page = cJSON_IsNumber(page_item) ? page_item->valueint : 0;
    int page_size =
        cJSON_IsNumber(page_size_item) ? page_size_item->valueint : 20;

    int count = -1;

    if (cJSON_IsNumber(count_item)) {
        count = count_item->valueint;
    } else if (cJSON_IsArray(list)) {
        count = cJSON_GetArraySize(list);
    }

    if (count < 0) {
        count = 0;
    }

    /*
     * latest_log_id：
     * 当前页第一条 logId。
     * 对 page=0 来说，它就是最新 QSO 的 logId。
     *
     * log_ids：
     * 当前页所有 logId，用于增量扫描时查找旧 latestLogId。
     */
    int latest_log_id = -1;

    int log_ids[EVENT_QSO_LOG_ID_MAX_COUNT];
    int log_id_count = 0;

    if (cJSON_IsArray(list)) {
        int arr_size = cJSON_GetArraySize(list);

        if (arr_size > EVENT_QSO_LOG_ID_MAX_COUNT) {
            arr_size = EVENT_QSO_LOG_ID_MAX_COUNT;
        }

        for (int i = 0; i < arr_size; i++) {
            cJSON *item = cJSON_GetArrayItem(list, i);
            if (!cJSON_IsObject(item)) {
                continue;
            }

            cJSON *log_id = cJSON_GetObjectItem(item, "logId");
            if (!cJSON_IsNumber(log_id)) {
                continue;
            }

            int id = log_id->valueint;

            if (i == 0) {
                latest_log_id = id;
            }

            log_ids[log_id_count++] = id;
        }
    }

    ESP_LOGD(TAG,
             "qso getListResponse page=%d pageSize=%d count=%d latest=%d ids=%d",
             page,
             page_size,
             count,
             latest_log_id,
             log_id_count);

    audio_ws_qso_count_handle_response(page,
                                       page_size,
                                       count,
                                       latest_log_id,
                                       log_ids,
                                       log_id_count);
}
