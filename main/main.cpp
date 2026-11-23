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
#include "okou_agent_ext.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"

using namespace mooncake;

static const char native_profile[] __attribute__((section(".rodata_custom_desc"), aligned(4))) =
    "cardputer-roast/native/cardputer-adv";

// Potentially slow Hub commands (e.g. scans) must not run in the roaster control loop.
static void hub_commands(void*)
{
    while (!okou_net_has_ip()) vTaskDelay(pdMS_TO_TICKS(100));
    esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t result = esp_netif_sntp_init(&clock);
    if (result != ESP_OK) ESP_LOGW("roast", "SNTP start: %s", esp_err_to_name(result));
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
    ESP_LOGI("roast", "ESP Hub native roast ready: %s", native_profile);

    // Native application in the ESP Hub app slot; Roast owns the whole screen.
    GetHAL().display.setBrightness(255);
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
