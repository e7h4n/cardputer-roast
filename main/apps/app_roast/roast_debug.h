#pragma once
#include <hal/keyboard/keyboard.h>
#include <string>

namespace roast {
// USB-serial remote (keys + canvas screenshots), started once at boot and polled from the main loop.
void DebugStart();
void DebugPollMain();
extern bool g_debugShift;
class RoasterLink;
extern RoasterLink* g_activeLink;  // set by the Roast app while it owns a link
extern bool g_debugQuiet;  // remote tests asked for silence: no sound at all, including alarms
}  // namespace roast
