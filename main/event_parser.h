/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file event_parser.h
 * @brief Event WebSocket JSON 解析接口。
 *
 * 本模块负责解析 Event WebSocket 收到的 JSON 消息。
 *
 * 当前主要处理：
 * - qso/callsign：当前通联呼号与 isSpeaking 状态；
 * - qso/getListResponse：QSO 列表响应，用于 QSO 数量同步。
 */

#ifndef EVENT_PARSER_H
#define EVENT_PARSER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 处理 Event WebSocket JSON。
 *
 * @param json JSON 字符串指针。
 * @param len JSON 字节长度。
 */
void event_parser_handle_json(const char *json, int len);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_PARSER_H */
