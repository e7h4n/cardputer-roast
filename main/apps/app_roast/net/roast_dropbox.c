// SPDX-License-Identifier: MIT
/*
 * Dropbox authorization, ported from e7h4n/coffee-watch (firmware/demo/main/dropbox_setup.c) without its brew-record
 * uploads. The setup page sends the user's own App key, App secret and an authorization code (authorize URL with
 * token_access_type=offline and no redirect URI); this task exchanges the code for a refresh token, stores the
 * account in NVS, and proves it with an upload whose Dropbox content_hash must match ours.
 *
 * HTTPS: ESP-IDF certificate bundle, no redirects, SNTP first so certificates can be checked. Tokens, the secret and
 * the code are never logged or returned over GATT; private buffers are wiped after use. The 4 KB response buffer and
 * the access token are heap allocated while the setup screen is open (the device has no PSRAM).
 */
#include "roast_dropbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

#define TAG "rn-dropbox"
#define NVS_NS "rn_dropbox"
#define TOKEN_URL "https://api.dropboxapi.com/oauth2/token"
#define UPLOAD_URL "https://content.dropboxapi.com/2/files/upload"
#define WRITE_SCOPE "files.content.write"
#define CHECK_PATH "/cardputer-roast/connection-check.json"  // its own file: the app folder may be shared with a watch
#define RESPONSE_MAX 4096  // token and upload replies are ~1.5 KB
#define TOKEN_MAX 2049
#define HASH_BLOCK (4 * 1024 * 1024)

typedef struct {
    char key[CW_DROPBOX_KEY_MAX + 1], secret[CW_DROPBOX_SECRET_MAX + 1], refresh[512];
} account_t;
typedef struct {
    char state[24], error[40];
    bool saved;
    uint32_t request;
} public_t;
typedef enum { UPLOAD_OK, UPLOAD_FAILED, UPLOAD_CONFLICT, UPLOAD_RESPONSE_INVALID, UPLOAD_HASH_MISMATCH } upload_result_t;
typedef struct {
    bool quit;
    cw_dropbox_command_t command;
} item_t;

static account_t account;
static public_t view = {.state = "unconfigured"};
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t commands;
static SemaphoreHandle_t finished;
static TaskHandle_t task;
static atomic_bool busy, have_credentials;
static char *response, *access_token;
static bool sntp_ready;

static void set_state(const char *state, const char *error)
{
    portENTER_CRITICAL(&lock);
    snprintf(view.state, sizeof view.state, "%s", state);
    snprintf(view.error, sizeof view.error, "%s", error ? error : "");
    view.saved = atomic_load(&have_credentials);
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "state=%s error=%s", state, error ? error : "none");
    rn_dropbox_changed(state, error);
}

static void wipe_session(void)
{
    if (access_token) mbedtls_platform_zeroize(access_token, TOKEN_MAX);
    if (response) mbedtls_platform_zeroize(response, RESPONSE_MAX);
}

static bool store(bool erase)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t rc = erase ? nvs_erase_all(h) : nvs_set_blob(h, "account", &account, sizeof account);
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h);
    if (rc == ESP_OK) atomic_store(&have_credentials, !erase);
    return rc == ESP_OK;
}

static bool load(void)
{
    nvs_handle_t h;
    size_t n = sizeof account;
    bool ok  = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "account", &account, &n) == ESP_OK && n == sizeof account;
        nvs_close(h);
    }
    if (!ok) memset(&account, 0, sizeof account);
    account.key[CW_DROPBOX_KEY_MAX] = account.secret[CW_DROPBOX_SECRET_MAX] = account.refresh[sizeof account.refresh - 1] = 0;
    bool saved = ok && account.key[0] && account.secret[0] && account.refresh[0];
    atomic_store(&have_credentials, saved);
    return saved;
}

bool rn_dropbox_saved(void)
{
    nvs_handle_t h;
    size_t n = 0;
    bool ok  = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "account", NULL, &n) == ESP_OK && n == sizeof(account_t);
        nvs_close(h);
    }
    return ok;
}

static bool text_field(cJSON *json, const char *key, char *out, size_t size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsString(v) || !v->valuestring[0] || strlen(v->valuestring) >= size) return false;
    memcpy(out, v->valuestring, strlen(v->valuestring) + 1);
    return true;
}

static bool clock_ready(void)
{
    if (time(NULL) > 1735689600) return true;  // after 2025-01-01: already set
    if (!sntp_ready) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.start             = false;
        if (esp_netif_sntp_init(&config) != ESP_OK) return false;
        sntp_ready = true;
    }
    esp_netif_sntp_start();
    return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK && time(NULL) > 1735689600;
}

static int http(const char *url, const char *body, size_t length, const char *type, bool authorized, const char *api_arg)
{
    mbedtls_platform_zeroize(response, RESPONSE_MAX);
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach,
                                    .disable_auto_redirect = true, .buffer_size = 2048, .buffer_size_tx = 4096};
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return -1;
    esp_http_client_set_method(h, HTTP_METHOD_POST);
    if (type) esp_http_client_set_header(h, "Content-Type", type);
    if (api_arg) esp_http_client_set_header(h, "Dropbox-API-Arg", api_arg);
    if (authorized) {
        char *auth = malloc(strlen(access_token) + 8);
        if (!auth) {
            esp_http_client_cleanup(h);
            return -1;
        }
        sprintf(auth, "Bearer %s", access_token);
        esp_http_client_set_header(h, "Authorization", auth);
        mbedtls_platform_zeroize(auth, strlen(auth));
        free(auth);
    }
    int status = -1;
    if (esp_http_client_open(h, (int)length) == ESP_OK) {
        bool sent = true;
        for (size_t offset = 0; offset < length;) {
            int written = esp_http_client_write(h, body + offset, (int)(length - offset));
            if (written <= 0) {
                sent = false;
                break;
            }
            offset += (size_t)written;
        }
        if (sent && esp_http_client_fetch_headers(h) >= 0) {
            int used = 0, got = 0;
            while (used < RESPONSE_MAX - 1 && (got = esp_http_client_read(h, response + used, RESPONSE_MAX - 1 - used)) > 0)
                used += got;
            response[used] = 0;
            if (got >= 0 && esp_http_client_is_complete_data_received(h)) status = esp_http_client_get_status_code(h);
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status;
}

// application/x-www-form-urlencoded from NULL-terminated name/value pairs; every value is percent-encoded.
static char *form(const char *name, ...)
{
    va_list a;
    size_t cap = 1;
    va_start(a, name);
    for (const char *n = name; n; n = va_arg(a, const char *)) {
        const char *v = va_arg(a, const char *);
        cap += strlen(n) + 2 + 3 * strlen(v);
    }
    va_end(a);
    char *out = calloc(1, cap), *p = out;
    if (!out) return NULL;
    static const char hex[] = "0123456789ABCDEF";
    va_start(a, name);
    for (const char *n = name; n; n = va_arg(a, const char *)) {
        const char *v = va_arg(a, const char *);
        if (p != out) *p++ = '&';
        p += sprintf(p, "%s=", n);
        for (const unsigned char *s = (const unsigned char *)v; *s; s++) {
            if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' ||
                *s == '_' || *s == '.' || *s == '~') {
                *p++ = *s;
            } else {
                *p++ = '%';
                *p++ = hex[*s >> 4];
                *p++ = hex[*s & 15];
            }
        }
    }
    va_end(a);
    *p = 0;
    return out;
}

static int post_form(char *body)
{
    if (!body) return -1;
    int status = http(TOKEN_URL, body, strlen(body), "application/x-www-form-urlencoded", false, NULL);
    mbedtls_platform_zeroize(body, strlen(body));
    free(body);
    return status;
}

static bool has_scope(cJSON *json, const char *want)
{
    const cJSON *scope = cJSON_GetObjectItemCaseSensitive(json, "scope");
    if (!cJSON_IsString(scope)) return false;
    const char *s = scope->valuestring;
    size_t n      = strlen(want);
    while (*s) {
        while (*s == ' ') s++;
        const char *end = strchr(s, ' ');
        size_t len      = end ? (size_t)(end - s) : strlen(s);
        if (len == n && !memcmp(s, want, n)) return true;
        if (!end) break;
        s = end + 1;
    }
    return false;
}

static void free_private_json(cJSON *j)
{
    if (!j) return;
    for (cJSON *c = j->child; c; c = c->next)
        if (c->valuestring) mbedtls_platform_zeroize(c->valuestring, strlen(c->valuestring));
    cJSON_Delete(j);
}

// Dropbox token responses use token_type "bearer". The first exchange must also return a refresh token
// (token_access_type=offline) and the write scope.
static const char *token_response(bool initial)
{
    cJSON *json = cJSON_Parse(response);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "token_type");
    const cJSON *ttl  = cJSON_GetObjectItemCaseSensitive(json, "expires_in");
    bool ok = cJSON_IsString(type) && !strcasecmp(type->valuestring, "bearer") && cJSON_IsNumber(ttl) &&
              ttl->valuedouble > 0 && text_field(json, "access_token", access_token, TOKEN_MAX);
    if (ok)
        for (const unsigned char *p = (const unsigned char *)access_token; *p; p++)
            if (*p <= 32 || *p >= 127) {
                ok = false;
                break;
            }
    const char *error = ok ? NULL : "invalid_token_response";
    if (ok && initial) {
        if (!text_field(json, "refresh_token", account.refresh, sizeof account.refresh))
            error = "no_refresh_token";
        else if (!has_scope(json, WRITE_SCOPE))
            error = "missing_scope";
    }
    free_private_json(json);
    mbedtls_platform_zeroize(response, RESPONSE_MAX);
    return error;
}

static bool refresh(void)
{
    int status = post_form(form("grant_type", "refresh_token", "refresh_token", account.refresh, "client_id", account.key,
                                "client_secret", account.secret, NULL));
    return status == 200 && !token_response(false);
}

// Dropbox content_hash: SHA-256 of the concatenated SHA-256 digests of each 4 MiB block.
static void content_hash(const uint8_t *data, size_t length, char out[65])
{
    mbedtls_sha256_context all;
    uint8_t digest[32];
    mbedtls_sha256_init(&all);
    mbedtls_sha256_starts(&all, 0);
    for (size_t offset = 0; offset < length; offset += HASH_BLOCK) {
        size_t n = length - offset < HASH_BLOCK ? length - offset : HASH_BLOCK;
        mbedtls_sha256(data + offset, n, digest, 0);
        mbedtls_sha256_update(&all, digest, sizeof digest);
    }
    mbedtls_sha256_finish(&all, digest);
    mbedtls_sha256_free(&all);
    for (unsigned i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", digest[i]);
}

// Uploads to an app-folder path with a Dropbox WriteMode; only a stored content_hash equal to ours counts.
static upload_result_t upload(const char *path, const char *mode, const char *data, size_t length)
{
    char arg[320];
    int n = snprintf(arg, sizeof arg, "{\"path\":\"%s\",\"mode\":%s,\"autorename\":false,\"mute\":true,\"strict_conflict\":true}",
                     path, mode);
    if (n < 0 || (size_t)n >= sizeof arg) return UPLOAD_FAILED;
    char expected[65];
    content_hash((const uint8_t *)data, length, expected);
    int status = http(UPLOAD_URL, data, length, "application/octet-stream", true, arg);
    if (status == 409) return UPLOAD_CONFLICT;
    if (status != 200) return UPLOAD_FAILED;
    cJSON *reply = cJSON_Parse(response);
    char hash[72], stored[96];
    bool ok = text_field(reply, "content_hash", hash, sizeof hash) && text_field(reply, "path_lower", stored, sizeof stored);
    cJSON_Delete(reply);
    if (!ok || strcasecmp(stored, path)) return UPLOAD_RESPONSE_INVALID;  // our paths are ASCII
    return strcmp(hash, expected) ? UPLOAD_HASH_MISMATCH : UPLOAD_OK;
}

static bool verify_dropbox(void)
{
    const char *content = "{\"schema\":1,\"application\":\"cardputer-roast\",\"purpose\":\"connection-check\"}\n";
    return upload(CHECK_PATH, "\"overwrite\"", content, strlen(content)) == UPLOAD_OK;
}

static void begin_auth(const cw_dropbox_command_t *c)
{
    memset(&account, 0, sizeof account);
    snprintf(account.key, sizeof account.key, "%s", c->key);
    snprintf(account.secret, sizeof account.secret, "%s", c->secret);
    set_state("authorizing", NULL);
    if (!clock_ready()) {
        set_state("failed", "clock_sync_failed");
        return;
    }
    int status = post_form(form("code", c->code, "grant_type", "authorization_code", "client_id", account.key,
                                "client_secret", account.secret, NULL));
    if (status != 200) {
        cJSON *j         = cJSON_Parse(response);
        const cJSON *e   = cJSON_GetObjectItemCaseSensitive(j, "error");
        const char *why  = status < 0 ? "dropbox_unreachable"
                           : cJSON_IsString(e) && !strcmp(e->valuestring, "invalid_grant")  ? "code_rejected"
                           : cJSON_IsString(e) && !strcmp(e->valuestring, "invalid_client") ? "client_rejected"
                                                                                            : "authorization_rejected";
        free_private_json(j);
        mbedtls_platform_zeroize(response, RESPONSE_MAX);
        set_state("failed", why);
        return;
    }
    const char *error = token_response(true);
    if (error) {
        set_state("failed", error);
        return;
    }
    if (!store(false)) {
        set_state("failed", "storage_error");
        return;
    }
    set_state("verifying", NULL);
    if (!verify_dropbox()) {
        set_state("failed", "upload_check_failed");
        return;
    }
    set_state("connected", NULL);
}

static void check_upload(void)
{
    if (!load()) {
        set_state("failed", "authorize_first");
        return;
    }
    set_state("verifying", NULL);
    if (!clock_ready())
        set_state("failed", "clock_sync_failed");
    else if (!refresh())
        set_state("failed", "refresh_failed");
    else if (!verify_dropbox())
        set_state("failed", "upload_check_failed");
    else
        set_state("connected", NULL);
}

static void dropbox_task(void *arg)
{
    item_t item;
    for (;;) {
        if (xQueueReceive(commands, &item, portMAX_DELAY) != pdTRUE) continue;
        if (item.quit) break;
        wipe_session();
        cw_dropbox_command_t *c = &item.command;
        portENTER_CRITICAL(&lock);
        view.request = c->request;
        portEXIT_CRITICAL(&lock);
        if (c->op == 1) {
            begin_auth(c);
        } else if (c->op == 2) {  // remove from this device; files and the app's access on Dropbox stay
            if (store(true)) {
                mbedtls_platform_zeroize(&account, sizeof account);
                set_state("unconfigured", NULL);
            } else {
                set_state("failed", "storage_error");
            }
        } else if (c->op == 3) {
            check_upload();
        }
        mbedtls_platform_zeroize(&item, sizeof item);
        wipe_session();
        mbedtls_platform_zeroize(&account, sizeof account);
        if (uxQueueMessagesWaiting(commands) == 0) atomic_store(&busy, false);
    }
    xSemaphoreGive(finished);
    vTaskDelete(NULL);
}

bool rn_dropbox_start(void)
{
    if (task) return true;
    response     = calloc(1, RESPONSE_MAX);
    access_token = calloc(1, TOKEN_MAX);
    commands     = xQueueCreate(4, sizeof(item_t));
    finished     = xSemaphoreCreateBinary();
    atomic_store(&busy, false);
    if (!response || !access_token || !commands || !finished ||
        xTaskCreate(dropbox_task, "rn_dropbox", 8192, NULL, 3, &task) != pdPASS) {
        task = NULL;
        rn_dropbox_stop();
        return false;
    }
    portENTER_CRITICAL(&lock);
    view.request = 0;
    portEXIT_CRITICAL(&lock);
    set_state(load() ? "saved" : "unconfigured", NULL);
    mbedtls_platform_zeroize(&account, sizeof account);
    return true;
}

void rn_dropbox_stop(void)
{
    if (task) {
        item_t quit = {.quit = true};
        xQueueSend(commands, &quit, portMAX_DELAY);  // after a request in flight (bounded by the 15 s HTTP timeouts)
        xSemaphoreTake(finished, portMAX_DELAY);
        task = NULL;
    }
    if (commands) vQueueDelete(commands);
    if (finished) vSemaphoreDelete(finished);
    commands = NULL;
    finished = NULL;
    wipe_session();
    free(response);
    free(access_token);
    response = access_token = NULL;
    mbedtls_platform_zeroize(&account, sizeof account);
    atomic_store(&busy, false);
}

bool rn_dropbox_submit(const cw_dropbox_command_t *c)
{
    if (!task) return false;
    if (atomic_exchange(&busy, true)) return false;  // one request at a time
    item_t item = {.quit = false, .command = *c};
    bool ok     = xQueueSend(commands, &item, 0) == pdTRUE;
    mbedtls_platform_zeroize(&item, sizeof item);
    if (!ok) atomic_store(&busy, false);
    return ok;
}

bool rn_dropbox_busy(void)
{
    return atomic_load(&busy);
}

int rn_dropbox_status(char *out, int capacity)
{
    public_t v;
    portENTER_CRITICAL(&lock);
    v = view;
    portEXIT_CRITICAL(&lock);
    int n = snprintf(out, capacity, "{\"v\":2,\"state\":\"%s\",\"error\":\"%s\",\"saved\":%s,\"request\":%lu}", v.state,
                     v.error, v.saved ? "true" : "false", (unsigned long)v.request);
    return n > 0 && n < capacity ? n : -1;
}
