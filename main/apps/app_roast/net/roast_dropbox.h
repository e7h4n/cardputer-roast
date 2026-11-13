// SPDX-License-Identifier: MIT
// Dropbox authorization for the setup screen (internal to roast_net). Ported from e7h4n/coffee-watch.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dropbox_packet.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Allocates the HTTP buffers and starts the Dropbox task; publishes "saved" or "unconfigured".
bool rn_dropbox_start(void);
/// Ends the task (after a request in flight) and frees its memory.
void rn_dropbox_stop(void);
/// Queues a command from the setup page. False when another request is still running.
bool rn_dropbox_submit(const cw_dropbox_command_t *command);
bool rn_dropbox_busy(void);
/// Status JSON for the Dropbox status characteristic: {"v":2,"state","error","saved","request"}.
int rn_dropbox_status(char *json, int capacity);
/// Credentials are stored (NVS only, nothing started).
bool rn_dropbox_saved(void);

/* ---- Sync client (no setup screen; Wi-Fi must be up). Paths are inside the app folder, UTF-8. ---- */
typedef struct {
    char name[128];
    char hash[65];  // Dropbox content_hash
    uint32_t size;
} rn_dbx_entry_t;
typedef void (*rn_dbx_entry_cb)(const rn_dbx_entry_t *entry, void *ctx);
typedef void (*rn_dbx_progress_cb)(uint32_t done, uint32_t total, void *ctx);

/// Loads the saved account, sets the clock (SNTP) and gets a fresh access token. err: a short reason.
bool rn_dbx_open(char *err, size_t errlen);
void rn_dbx_close(void);
/// Files directly in `folder` (a missing folder lists as empty).
bool rn_dbx_list(const char *folder, rn_dbx_entry_cb cb, void *ctx, char *err, size_t errlen);
/// Downloads to `local` (via `local`.part), checking Dropbox's content_hash.
bool rn_dbx_download(const char *path, const char *local, char *err, size_t errlen);
/// Uploads `local` to `path`, overwriting, streamed from the card; only a matching content_hash counts.
bool rn_dbx_upload(const char *local, const char *path, rn_dbx_progress_cb cb, void *ctx, char *err, size_t errlen);
/// Deletes `path`; already gone counts as success.
bool rn_dbx_delete(const char *path, char *err, size_t errlen);
/// Dropbox content_hash of a local file (hex). False when it cannot be read.
bool rn_dbx_local_hash(const char *local, char out[65]);

/// Implemented by roast_net.c: a state change to show on screen and signal to the browser.
void rn_dropbox_changed(const char *state, const char *error);

#ifdef __cplusplus
}
#endif
