#pragma once
#include <algorithm>
namespace roast {
// Cardputer ADV exposes voltage, not a fuel gauge. Reject absent/invalid ADC readings.
inline int BatteryPercent(int millivolts)
{
    if (millivolts < 2500 || millivolts > 4600) return -1;
    return std::clamp((millivolts - 3300) * 100 / 800, 0, 100);
}
}
