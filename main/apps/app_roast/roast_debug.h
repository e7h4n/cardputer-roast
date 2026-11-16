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
/// Test only (USB console `boot roast`): restart straight into Roast once, skipping the Wi-Fi/Dropbox requirement and
/// the boot sync, so roasting can be tested on a device without Dropbox.
void DebugBootRoast();
}  // namespace roast
