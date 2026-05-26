/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file station_parser.c
 * @brief Station WebSocket JSON 解析与请求构造实现。
 */

#include "station_parser.h"

/* Standard library headers ------------------------------------------------- */
#include <stdio.h>
#include <string.h>

/* Third-party headers ------------------------------------------------------ */
#include "cJSON.h"

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_log.h"

/* Project headers ---------------------------------------------------------- */
#include "ui_async.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "station_parser";

/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief 站点缓存。
 *
 * 当前未加锁，默认由解析任务更新、UI 线程读取。
 * 如后续出现并发访问问题，可增加互斥锁或复制快照机制。
 */
static station_cache_t s_station_cache = {0};

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static void station_item_clear(station_item_t *item);
static void station_item_set(station_item_t *item,
                             int uid,
                             const char *name);

static void station_cache_clear_list(station_item_t *list,
                                     int max_items,
                                     int *count);

static void station_cache_parse_list(cJSON *json_list,
                                     station_item_t *dst_list,
                                     int max_items,
                                     int *out_count,
                                     const char *log_prefix);


/* -------------------------------------------------------------------------- */
/* Station item helpers                                                       */
/* -------------------------------------------------------------------------- */

static void station_item_clear(station_item_t *item)
{
    if (!item) {
        return;
    }

    item->uid = -1;
    item->name[0] = '\0';
    item->valid = false;
}

static void station_item_set(station_item_t *item,
                             int uid,
                             const char *name)
{
    if (!item) {
        return;
    }

    item->uid = uid;
    item->valid = true;

    if (name) {
        strncpy(item->name, name, sizeof(item->name) - 1);
        item->name[sizeof(item->name) - 1] = '\0';
    } else {
        item->name[0] = '\0';
    }
}

static void station_cache_clear_list(station_item_t *list,
                                     int max_items,
                                     int *count)
{
    if (!list || max_items <= 0) {
        return;
    }

    for (int i = 0; i < max_items; i++) {
        station_item_clear(&list[i]);
    }

    if (count) {
        *count = 0;
    }
}

/* -------------------------------------------------------------------------- */
/* Station list parsing                                                       */
/* -------------------------------------------------------------------------- */

static void station_cache_parse_list(cJSON *json_list,
                                     station_item_t *dst_list,
                                     int max_items,
                                     int *out_count,
                                     const char *log_prefix)
{
    station_cache_clear_list(dst_list, max_items, out_count);

    if (!cJSON_IsArray(json_list)) {
        return;
    }

    int arr_size = cJSON_GetArraySize(json_list);
    int valid_count = 0;

    for (int i = 0; i < arr_size && valid_count < max_items; i++) {
        cJSON *item = cJSON_GetArrayItem(json_list, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }

        cJSON *uid = cJSON_GetObjectItem(item, "uid");
        cJSON *name = cJSON_GetObjectItem(item, "name");

        if (!cJSON_IsString(name)) {
            continue;
        }

        station_item_set(&dst_list[valid_count],
                         cJSON_IsNumber(uid) ? uid->valueint : -1,
                         name->valuestring);

        ESP_LOGI(TAG,
                 "%s[%d]: uid=%d name=%s",
                 log_prefix ? log_prefix : "station",
                 valid_count,
                 dst_list[valid_count].uid,
                 dst_list[valid_count].name);

        valid_count++;
    }

    if (out_count) {
        *out_count = valid_count;
    }
}

/* -------------------------------------------------------------------------- */
/* Public JSON parser                                                         */
/* -------------------------------------------------------------------------- */

void station_parser_handle_json(const char *json, int len)
{
    if (!json || len <= 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *subType = cJSON_GetObjectItem(root, "subType");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *code = cJSON_GetObjectItem(root, "code");

    if (!cJSON_IsString(type) || !cJSON_IsString(subType)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "station") != 0) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(subType->valuestring, "getCurrentResponse") == 0) {
        if (cJSON_IsNumber(code) &&
            code->valueint == 0 &&
            cJSON_IsObject(data)) {

            cJSON *uid = cJSON_GetObjectItem(data, "uid");
            cJSON *name = cJSON_GetObjectItem(data, "name");

            if (cJSON_IsString(name)) {
                station_item_set(&s_station_cache.current,
                                 cJSON_IsNumber(uid) ? uid->valueint : -1,
                                 name->valuestring);

                ui_async_update_station(name->valuestring);

                ESP_LOGI(TAG,
                         "Current station: uid=%d, name=%s",
                         s_station_cache.current.uid,
                         s_station_cache.current.name);
            }
        }
    } else if (strcmp(subType->valuestring, "getListResponse") == 0) {
        if (cJSON_IsNumber(code) &&
            code->valueint == 0 &&
            cJSON_IsObject(data)) {

            cJSON *list = cJSON_GetObjectItem(data, "list");
            cJSON *count = cJSON_GetObjectItem(data, "count");

            int cnt = cJSON_IsNumber(count) ? count->valueint : 0;

            ESP_LOGI(TAG, "Station list response count=%d", cnt);

            station_cache_parse_list(list,
                                     s_station_cache.list,
                                     STATION_LIST_MAX_ITEMS,
                                     &s_station_cache.list_count,
                                     "List");

            ui_async_station_list_updated();
        }
    } else if (strcmp(subType->valuestring,
                      "getPinnedListResponse") == 0) {
        if (cJSON_IsNumber(code) &&
            code->valueint == 0 &&
            cJSON_IsObject(data)) {

            cJSON *list = cJSON_GetObjectItem(data, "list");
            cJSON *count = cJSON_GetObjectItem(data, "count");

            int cnt = cJSON_IsNumber(count) ? count->valueint : 0;

            ESP_LOGI(TAG, "Pinned station list response count=%d", cnt);

            station_cache_parse_list(list,
                                     s_station_cache.pinned_list,
                                     STATION_LIST_MAX_ITEMS,
                                     &s_station_cache.pinned_count,
                                     "Pinned");

            ui_async_station_list_updated();
        }
    } else if (strcmp(subType->valuestring, "setCurrentResponse") == 0) {
        int result = -1;

        if (cJSON_IsObject(data)) {
            cJSON *result_item = cJSON_GetObjectItem(data, "result");

            if (cJSON_IsNumber(result_item)) {
                result = result_item->valueint;
            }
        }

        if (cJSON_IsNumber(code) &&
            code->valueint == 0 &&
            result == 0) {
            ESP_LOGI(TAG, "set current station success");
            ui_async_update_status("站点已切换");
        } else {
            ESP_LOGW(TAG, "set current station failed");
            ui_async_update_status("站点切换失败");
        }
    }

    cJSON_Delete(root);
}

/* -------------------------------------------------------------------------- */
/* Request builders                                                           */
/* -------------------------------------------------------------------------- */

esp_err_t station_build_get_current(char *buf, int buf_size)
{
    if (!buf || buf_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int n = snprintf(buf,
                     buf_size,
                     "{\"type\":\"station\",\"subType\":\"getCurrent\",\"data\":{}}");

    return (n > 0 && n < buf_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t station_build_get_list_range(char *buf,
                                       int buf_size,
                                       int start,
                                       int count)
{
    if (!buf || buf_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (start < 0) {
        start = 0;
    }

    if (count <= 0) {
        count = 1;
    }

    int n = snprintf(
        buf,
        buf_size,
        "{\"type\":\"station\",\"subType\":\"getListRange\",\"data\":{\"start\":%d,\"count\":%d}}",
        start,
        count
    );

    return (n > 0 && n < buf_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t station_build_get_pinned_list_range(char *buf,
                                              int buf_size,
                                              int start,
                                              int count)
{
    if (!buf || buf_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (start < 0) {
        start = 0;
    }

    if (count <= 0) {
        count = 8;
    }

    int n = snprintf(
        buf,
        buf_size,
        "{\"type\":\"station\",\"subType\":\"getPinnedList\",\"data\":{\"start\":%d,\"count\":%d}}",
        start,
        count
    );

    return (n > 0 && n < buf_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t station_build_set_current(char *buf, int buf_size, int uid)
{
    if (!buf || buf_size <= 0 || uid < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int n = snprintf(
        buf,
        buf_size,
        "{\"type\":\"station\",\"subType\":\"setCurrent\",\"data\":{\"uid\":%d}}",
        uid
    );

    return (n > 0 && n < buf_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* -------------------------------------------------------------------------- */
/* Cache access                                                               */
/* -------------------------------------------------------------------------- */

const station_cache_t *station_cache_get(void)
{
    return &s_station_cache;
}

bool station_cache_get_current(station_item_t *out)
{
    if (!out) {
        return false;
    }

    if (!s_station_cache.current.valid) {
        return false;
    }

    *out = s_station_cache.current;

    return true;
}

int station_cache_get_list(station_item_t *out, int max_items)
{
    if (!out || max_items <= 0) {
        return 0;
    }

    int n = s_station_cache.list_count;

    if (n > max_items) {
        n = max_items;
    }

    for (int i = 0; i < n; i++) {
        out[i] = s_station_cache.list[i];
    }

    return n;
}

int station_cache_get_pinned_list(station_item_t *out, int max_items)
{
    if (!out || max_items <= 0) {
        return 0;
    }

    int n = s_station_cache.pinned_count;

    if (n > max_items) {
        n = max_items;
    }

    for (int i = 0; i < n; i++) {
        out[i] = s_station_cache.pinned_list[i];
    }

    return n;
}
