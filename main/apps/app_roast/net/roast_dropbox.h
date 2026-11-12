// SPDX-License-Identifier: MIT
// Dropbox authorization for the setup screen (internal to roast_net). Ported from e7h4n/coffee-watch.
#pragma once
#include <stdbool.h>
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

/// Implemented by roast_net.c: a state change to show on screen and signal to the browser.
void rn_dropbox_changed(const char *state, const char *error);

#ifdef __cplusplus
}
#endif
