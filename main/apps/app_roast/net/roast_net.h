/* Dropbox authorization over authenticated Bluetooth. ESP Hub owns Wi-Fi and OTA.
 * Only the Dropbox setup service starts/stops here. NVS rn_dropbox is retained;
 * no rn_wifi credentials are read or written. Network state is a read-only SDK view.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RN_NET_UNCONFIGURED,
    RN_NET_CONNECTING,
    RN_NET_CONNECTED,
    RN_NET_FAILED,
    RN_NET_DISCONNECTED,
} rn_net_state_t;

typedef struct {
    bool open;          // roast_net_open() succeeded and close() was not called
    bool advertising;   // setup service visible, waiting for a browser
    bool connecting;    // a browser is connected and pairing: its OS dialog asks for the passkey now
    bool paired;        // a browser is connected over an authenticated, encrypted link
    bool saved;         // Wi-Fi credentials are stored
    int setup_error;    // nonzero: Bluetooth setup could not start (NimBLE return code)
    unsigned passkey;   // six digits the browser's pairing dialog asks for
    rn_net_state_t state;
    int reason;         // Wi-Fi disconnect reason or error code
    char ssid[33];
    char ip[16];
    char device_name[24];  // advertised name, e.g. CardputerRoast-3F2A
    char dropbox[24];      // unconfigured, saved, authorizing, verifying, connected, failed
    char dropbox_error[40];
} rn_view_t;

/// Starts the Dropbox Bluetooth setup service. ESP Hub remains connected.
bool roast_net_open(void);
/// Stops Dropbox Bluetooth setup only. Waits for a Dropbox request in flight.
void roast_net_close(void);
/// A Dropbox request (authorize or upload check) is running.
bool roast_net_busy(void);
rn_view_t roast_net_view(void);

/// Wait for an IP address from the ESP Hub SDK; this never changes Wi-Fi configuration.
bool roast_net_wait_for_network(int timeout_ms, char *err, size_t errlen);

/// Saved state, read from NVS without starting anything (for the rules page).
bool roast_net_esphub_network(char ssid[33]);
bool roast_net_saved_dropbox(void);

#ifdef __cplusplus
}
#endif
