#pragma once
#include <cstring>
inline int eh_enter_bootloader() { return 0; }
inline unsigned previewWifiJoins = 0;
inline bool previewWifiHidden = false;
inline bool previewWifiSaved = false;
inline bool eh_wifi_join(const char* ssid, const char* password, bool hidden)
{
    ++previewWifiJoins;
    previewWifiHidden = hidden;
    previewWifiSaved = std::strcmp(ssid, hidden ? "hidden" : "Guest Wi-Fi") == 0 && std::strcmp(password, "secret") == 0;
    return true;
}
inline const char* eh_wifi_join_state() { return previewWifiSaved ? "saved" : "failed"; }
