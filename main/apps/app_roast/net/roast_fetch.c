// SPDX-License-Identifier: MIT
#include "roast_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

#define TAG "rn-fetch"
#define CHUNK 4096
#define MAX_HOPS 5

static void seterr(char *err, size_t n, const char *fmt, int code)
{
    if (err && n) snprintf(err, n, fmt, code);
}

static bool redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// GET with redirects (github.com/.../releases/latest/download/x -> .../download/<tag>/x -> a signed ~900-character
// URL on release-assets.githubusercontent.com), as esp_https_ota does it: on a redirect set the new location, drain
// the body, open again. Returns an open client positioned at the 200 body, or NULL.
static esp_http_client_handle_t open_get(const char *url, int64_t *length, char *err, size_t n)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .buffer_size       = 2048,
        .buffer_size_tx    = 2048,  // the request line carries the signed URL
        .user_agent        = "cardputer-roast",
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        seterr(err, n, "update: out of memory", 0);
        return NULL;
    }
    char drain[256];
    for (int hop = 0; hop <= MAX_HOPS; hop++) {
        if (esp_http_client_open(h, 0) != ESP_OK) {
            seterr(err, n, "update: GitHub unreachable", 0);
            break;
        }
        int64_t len = esp_http_client_fetch_headers(h);
        int status  = esp_http_client_get_status_code(h);
        if (len < 0 && status <= 0) {
            seterr(err, n, "update: no reply from GitHub", 0);
            break;
        }
        if (redirect(status)) {
            if (hop == MAX_HOPS || esp_http_client_set_redirection(h) != ESP_OK) {
                seterr(err, n, "update: too many redirects", 0);
                break;
            }
            while (esp_http_client_read(h, drain, sizeof drain) > 0) {
            }
            continue;
        }
        if (status != 200) {
            seterr(err, n, status == 404 ? "update: no release found (HTTP %d)" : "update: GitHub replied HTTP %d", status);
            break;
        }
        *length = len;
        return h;
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return NULL;
}

bool rn_fetch_text(const char *url, char *out, size_t cap, char *err, size_t n)
{
    int64_t len = 0;
    esp_http_client_handle_t h = open_get(url, &len, err, n);
    if (!h) return false;
    size_t used = 0;
    int r       = 0;
    while (used < cap - 1 && (r = esp_http_client_read(h, out + used, (int)(cap - 1 - used))) > 0) used += (size_t)r;
    out[used] = 0;
    bool ok   = r >= 0 && esp_http_client_is_complete_data_received(h);
    if (!ok) seterr(err, n, used >= cap - 1 ? "update: reply too long" : "update: connection dropped", 0);
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return ok;
}

bool rn_fetch_file(const char *url, const char *local, uint32_t max, uint8_t sha256[32], uint32_t *size,
                   rn_fetch_progress_cb cb, void *ctx, char *err, size_t n)
{
    uint8_t *buf = malloc(CHUNK);
    FILE *f      = buf ? fopen(local, "wb") : NULL;
    if (!buf || !f) {
        seterr(err, n, buf ? "update: cannot write the card" : "update: out of memory", 0);
        free(buf);
        return false;
    }
    int64_t len = 0;
    esp_http_client_handle_t h = open_get(url, &len, err, n);
    bool ok = false;
    if (h) {
        uint32_t total = len > 0 ? (uint32_t)len : max, done = 0;
        mbedtls_sha256_context sc;
        mbedtls_sha256_init(&sc);
        mbedtls_sha256_starts(&sc, 0);
        bool wrote = true, big = len > (int64_t)max;
        int r = 0;
        while (!big && (r = esp_http_client_read(h, (char *)buf, CHUNK)) > 0) {
            done += (uint32_t)r;
            if (done > max) {
                big = true;
                break;
            }
            mbedtls_sha256_update(&sc, buf, (size_t)r);
            if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
                wrote = false;
                break;
            }
            if (cb) cb(done, total, ctx);
        }
        mbedtls_sha256_finish(&sc, sha256);
        mbedtls_sha256_free(&sc);
        *size = done;
        if (big)
            seterr(err, n, "update: image larger than expected", 0);
        else if (!wrote)
            seterr(err, n, "update: card write failed", 0);
        else if (r < 0 || !esp_http_client_is_complete_data_received(h))
            seterr(err, n, "update: connection dropped", 0);
        else
            ok = true;
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
    }
    if (fclose(f) != 0) ok = false;
    free(buf);
    if (!ok) remove(local);
    ESP_LOGI(TAG, "%s -> %s: %s", url, local, ok ? "ok" : (err ? err : "failed"));
    return ok;
}
