#pragma once
#include <cstring>
inline int okou_enter_bootloader() { return 0; }
inline unsigned previewWifiJoins = 0;
inline bool previewWifiHidden = false;
inline bool okou_wifi_join(const char* ssid, const char* password, bool hidden)
{
    ++previewWifiJoins;
    previewWifiHidden = hidden;
    return std::strcmp(ssid, hidden ? "hidden" : "Test Wi-Fi") == 0 && std::strcmp(password, "secret") == 0;
}
inline const char* okou_wifi_join_state() { return "saved"; }
