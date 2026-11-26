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
#include "eh_agent.h"
#include "eh_agent_ext.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"

using namespace mooncake;

static const char native_profile[] __attribute__((section(".rodata_custom_desc"), aligned(4))) =
    "cardputer-roast/native/cardputer-adv";

static void start_sntp_when_online(void*)
{
    while (!eh_net_has_ip()) vTaskDelay(pdMS_TO_TICKS(100));
    esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t result = esp_netif_sntp_init(&clock);
    if (result != ESP_OK) ESP_LOGW("roast", "SNTP start: %s", esp_err_to_name(result));
    vTaskDelete(nullptr);
}

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_debug);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    ESP_ERROR_CHECK(eh_agent_start());
    ESP_ERROR_CHECK(xTaskCreate(start_sntp_when_online, "roast_sntp", 4096, nullptr, 2, nullptr) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI("roast", "ESP Hub native roast ready: %s (SDK %s)", native_profile, EH_SDK_VERSION);

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
