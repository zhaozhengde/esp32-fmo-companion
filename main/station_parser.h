/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file station_parser.h
 * @brief Station WebSocket JSON 解析与请求构造接口。
 *
 * 本模块负责：
 * - 解析当前站点响应；
 * - 解析普通站点列表响应；
 * - 解析收藏/置顶站点列表响应；
 * - 解析站点切换响应；
 * - 构造 station 相关 WebSocket 请求 JSON；
 * - 提供站点缓存读取接口。
 */

#ifndef STATION_PARSER_H
#define STATION_PARSER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Public macros                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 站点列表缓存最大数量。
 */
#define STATION_LIST_MAX_ITEMS   16

/**
 * @brief 站点名称最大长度。
 */
#define STATION_NAME_MAX_LEN     64

/* -------------------------------------------------------------------------- */
/* Public types                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 单个站点信息。
 */
typedef struct {
    /**
     * @brief 站点 UID。
     */
    int uid;

    /**
     * @brief 站点名称。
     */
    char name[STATION_NAME_MAX_LEN];

    /**
     * @brief 当前条目是否有效。
     */
    bool valid;
} station_item_t;

/**
 * @brief 站点缓存。
 */
typedef struct {
    /**
     * @brief 当前站点。
     */
    station_item_t current;

    /**
     * @brief 普通站点列表缓存。
     */
    station_item_t list[STATION_LIST_MAX_ITEMS];

    /**
     * @brief 普通站点列表有效数量。
     */
    int list_count;

    /**
     * @brief 收藏/置顶站点列表缓存。
     */
    station_item_t pinned_list[STATION_LIST_MAX_ITEMS];

    /**
     * @brief 收藏/置顶站点列表有效数量。
     */
    int pinned_count;
} station_cache_t;

/* -------------------------------------------------------------------------- */
/* JSON parser                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 处理 Station WebSocket JSON。
 *
 * @param json JSON 字符串。
 * @param len JSON 字节长度。
 */
void station_parser_handle_json(const char *json, int len);

/* -------------------------------------------------------------------------- */
/* Request builders                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief 构造请求当前站点 JSON。
 *
 * @param buf 输出缓冲区。
 * @param buf_size 输出缓冲区大小。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t station_build_get_current(char *buf, int buf_size);

/**
 * @brief 构造请求普通站点列表 JSON。
 *
 * @param buf 输出缓冲区。
 * @param buf_size 输出缓冲区大小。
 * @param start 起始位置。
 * @param count 请求数量。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t station_build_get_list_range(char *buf,
                                       int buf_size,
                                       int start,
                                       int count);

/**
 * @brief 构造请求收藏/置顶站点列表 JSON。
 *
 * 示例：
 *
 * @code
 * {
 *   "type":"station",
 *   "subType":"getPinnedList",
 *   "data":{"start":0,"count":8}
 * }
 * @endcode
 *
 * @param buf 输出缓冲区。
 * @param buf_size 输出缓冲区大小。
 * @param start 起始位置。
 * @param count 请求数量。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t station_build_get_pinned_list_range(char *buf,
                                              int buf_size,
                                              int start,
                                              int count);

/**
 * @brief 构造设置当前站点 JSON。
 *
 * 示例：
 *
 * @code
 * {
 *   "type":"station",
 *   "subType":"setCurrent",
 *   "data":{"uid":441}
 * }
 * @endcode
 *
 * @param buf 输出缓冲区。
 * @param buf_size 输出缓冲区大小。
 * @param uid 目标站点 UID。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t station_build_set_current(char *buf, int buf_size, int uid);

/* -------------------------------------------------------------------------- */
/* Cache access                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 获取内部站点缓存指针。
 *
 * @return 站点缓存只读指针。
 *
 * @note
 * 返回的是内部静态对象指针，调用方不应修改。
 */
const station_cache_t *station_cache_get(void);

/**
 * @brief 获取当前站点。
 *
 * @param out 输出站点信息。
 *
 * @return true 表示当前站点有效。
 */
bool station_cache_get_current(station_item_t *out);

/**
 * @brief 获取普通站点列表缓存。
 *
 * @param out 输出数组。
 * @param max_items 输出数组最大数量。
 *
 * @return 实际复制的站点数量。
 */
int station_cache_get_list(station_item_t *out, int max_items);

/**
 * @brief 获取收藏/置顶站点列表缓存。
 *
 * @param out 输出数组。
 * @param max_items 输出数组最大数量。
 *
 * @return 实际复制的站点数量。
 */
int station_cache_get_pinned_list(station_item_t *out, int max_items);

#ifdef __cplusplus
}
#endif

#endif /* STATION_PARSER_H */
