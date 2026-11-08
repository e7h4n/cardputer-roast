/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.h>
#include <M5Unified.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/app_roast/app_roast.h>
#include <apps/utils/audio/audio.h>
#include <hal.h>
#include <apps/app_roast/roast_debug.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_debug);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // Roast-only firmware for the community Launcher: Roast owns the whole screen.
    GetHAL().display.setBrightness(255);
    int roastId = GetMooncake().installApp(std::make_unique<AppRoast>());
    GetMooncake().openApp(roastId);

    // Main loop
    audio::set_keyboard_sfx_enable(true);
    roast::DebugStart();
    while (1) {
        GetHAL().feedTheDog();
        GetHAL().update();
        roast::DebugPollMain();
        GetMooncake().update();
    }
}
