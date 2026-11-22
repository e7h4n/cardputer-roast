/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <M5Unified.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/app_roast/app_roast.h>
#include <hal.h>
#include <apps/app_roast/roast_debug.h>
#include "okou_agent.h"
extern "C" {
#include "okou_ble.h"
}
#include "esp_log.h"

using namespace mooncake;

// Potentially slow Hub commands (e.g. scans) must not run in the roaster control loop.
static void hub_commands(void*)
{
    while (true) okou_poll(50);
}


extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_debug);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    okou_config_t hub = OKOU_CONFIG_DEFAULT();
    hub.handlers_in_poll = true;
    ESP_ERROR_CHECK(okou_agent_start(&hub));
    ESP_ERROR_CHECK(xTaskCreate(hub_commands, "roast_hub_cmd", 4096, nullptr, 2, nullptr) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI("roast", "ESP Hub native roast ready: cardputer-adv");

    // Native application in the ESP Hub app slot; Roast owns the whole screen.
    GetHAL().display.setBrightness(255);
    // The SDK releases NimBLE after its boot scan or a Wi-Fi setup session.
    if (okou_ble_host_owned()) {
        GetHAL().display.println("ESP Hub Wi-Fi setup");
        while (okou_ble_host_owned()) {
            GetHAL().feedTheDog();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    int roastId = GetMooncake().installApp(std::make_unique<AppRoast>());
    GetMooncake().openApp(roastId);

    // Main loop
    roast::DebugStart();
    while (1) {
        GetHAL().feedTheDog();
        GetHAL().update();
        roast::DebugPollMain();
        GetMooncake().update();
    }
}
