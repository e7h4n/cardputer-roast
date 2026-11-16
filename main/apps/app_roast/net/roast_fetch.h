// SPDX-License-Identifier: MIT
// Plain HTTPS GET for public files (GitHub release assets), following redirects. Wi-Fi must be up; no Bluetooth (the
// heap does not hold BLE + Wi-Fi + TLS). Certificates: the ESP-IDF bundle.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rn_fetch_progress_cb)(uint32_t done, uint32_t total, void *ctx);

/// A small text file into `out` (NUL-terminated). Fails if it does not fit. err: a short reason.
bool rn_fetch_text(const char *url, char *out, size_t cap, char *err, size_t errlen);

/// Streams `url` into the file `local` (created or truncated; removed on failure), at most `max` bytes. Fills the
/// SHA-256 of what was received (32 bytes) and its size; the caller checks them before using the file.
bool rn_fetch_file(const char *url, const char *local, uint32_t max, uint8_t sha256[32], uint32_t *size,
                   rn_fetch_progress_cb cb, void *ctx, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
