#pragma once
struct eh_ap_t { char ssid[33]; };
inline int eh_wifi_scan(eh_ap_t* aps, int max, unsigned)
{
    if (max < 2) return -1;
    aps[0] = {"Test Wi-Fi"};
    aps[1] = {"Guest Wi-Fi"};
    return 2;
}
