/*
 * Wi-Fi and Dropbox setup over Bluetooth, compatible with the Coffee Watch setup page
 * (https://e7h4n.github.io/coffee-watch/). Ported from e7h4n/coffee-watch (MIT): same GATT service, packets and
 * status JSON, so the page pairs a Cardputer exactly like a watch.
 *
 * Everything runs only while the setup screen is open: roast_net_open() starts Wi-Fi (joining the saved network)
 * and a NimBLE peripheral that advertises the setup service and shows a six-digit passkey; roast_net_close() stops
 * both and frees their memory. The roaster link must be closed first: both use NimBLE, and the device has no PSRAM.
 *
 * Credentials (Wi-Fi password, Dropbox App key/secret and refresh token) live in NVS namespaces "rn_wifi" and
 * "rn_dropbox"; they are never logged or returned over GATT.
 */
#pragma once
#include <stdbool.h>
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

/// Starts Wi-Fi and the Bluetooth setup service. False when NimBLE or Wi-Fi could not start.
bool roast_net_open(void);
/// Stops the setup service and Wi-Fi and frees their memory. Waits for a Dropbox request in flight.
void roast_net_close(void);
/// A Dropbox request (authorize or upload check) is running.
bool roast_net_busy(void);
rn_view_t roast_net_view(void);

/// Saved state, read from NVS without starting anything (for the rules page).
bool roast_net_saved_wifi(char ssid[33]);
bool roast_net_saved_dropbox(void);

#ifdef __cplusplus
}
#endif
