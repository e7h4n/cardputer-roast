// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CW_DROPBOX_KEY_MAX 32
#define CW_DROPBOX_SECRET_MAX 32
#define CW_DROPBOX_CODE_MAX 128
#define CW_DROPBOX_PACKET_MAX (9 + CW_DROPBOX_KEY_MAX + CW_DROPBOX_SECRET_MAX + CW_DROPBOX_CODE_MAX)
typedef struct {uint8_t op;uint32_t request;char key[CW_DROPBOX_KEY_MAX+1],secret[CW_DROPBOX_SECRET_MAX+1],code[CW_DROPBOX_CODE_MAX+1];} cw_dropbox_command_t;
// v2: version, op (1 authorize / 2 forget locally / 3 upload check), app key
// length, app secret length, nonzero LE32 request, authorization code length,
// then ASCII key, secret and code without terminators. Never readable.
bool cw_dropbox_packet_decode(const uint8_t *bytes,size_t size,cw_dropbox_command_t *out);
