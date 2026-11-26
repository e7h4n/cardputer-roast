#pragma once
struct esp_partition_t {};
inline bool previewBootloaderPresent = true;
inline const esp_partition_t* eh_bootloader_partition()
{
    static const esp_partition_t slot{};
    return previewBootloaderPresent ? &slot : nullptr;
}

struct eh_ap_t { char ssid[33]; };
inline int eh_wifi_scan(eh_ap_t* aps, int max, unsigned)
{
    if (max < 2) return -1;
    aps[0] = {"Test Wi-Fi"};
    aps[1] = {"Guest Wi-Fi"};
    return 2;
}
