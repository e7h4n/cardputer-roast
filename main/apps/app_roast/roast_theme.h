/*
 * Roast UI palette. One rule: colour = what it controls. The key chip, the value on screen and the chart line of
 * the same function always share a colour. Values are RGB888 and must stay uint32_t (a bare int is RGB565 in M5GFX).
 */
#pragma once
#include <cstdint>

namespace roast::ui {
// surfaces and text
constexpr uint32_t BG    = 0xEEEBE4;  // warm white
constexpr uint32_t CARD  = 0xF9F7F2;
constexpr uint32_t LINE  = 0xD4CFC6;
constexpr uint32_t GRID  = 0xE4E0D8;
constexpr uint32_t FAINT = 0xACA79E;
constexpr uint32_t DIM   = 0x76726A;
constexpr uint32_t INK   = 0x181818;
constexpr uint32_t WHITE = 0xFFFFFF;
// functions
constexpr uint32_t FAN        = 0x266CF0;  // S, ↑↓, fan value and line
constexpr uint32_t HEAT       = 0xF0761C;  // P, ←→, heater value and area
constexpr uint32_t HEAT_AREA  = 0xF8CEA8;
constexpr uint32_t HEAT_TINT  = 0xFDE8D6;
constexpr uint32_t CURVE      = 0x7E54D6;  // C, E, Tab, target, PID
constexpr uint32_t CURVE_TINT = 0xD2C8EC;
constexpr uint32_t DRY        = 0xDEAA22;  // Y: end of drying (starts Maillard)
constexpr uint32_t FC         = 0x965C34;  // F: first crack (starts development)
constexpr uint32_t FCE        = 0x643C22;  // G: end of first crack
constexpr uint32_t DROP       = 0xE2303A;  // D D, X X, alarms
constexpr uint32_t GO         = 0x1C9E58;  // charge, drop window, connected, saved
constexpr uint32_t WARN       = 0xFAC42E;  // attention (ink text on it)
constexpr uint32_t SIM        = 0xFFD640;  // simulator tag
constexpr uint32_t ROR        = 0x96928A;
// roast phases
constexpr uint32_t PH_DRY = 0x9CC87C;
constexpr uint32_t PH_MAI = 0xEABA48;
constexpr uint32_t PH_DEV = 0x965C34;
}  // namespace roast::ui
