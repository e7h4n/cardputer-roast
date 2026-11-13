// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {uint8_t op;uint32_t request_id;char ssid[33],password[65];} cw_wifi_packet_t;
// v1: version, operation (1 join / 2 forget), SSID byte length, password byte
// length, nonzero LE32 request ID, UTF-8 SSID, password bytes. Passwords never appear in status packets.
bool cw_wifi_packet_decode(const uint8_t *data,size_t size,cw_wifi_packet_t *out);
