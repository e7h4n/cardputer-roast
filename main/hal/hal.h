/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "keyboard/keyboard.h"
#include "ext_display.h"
#include <M5Unified.hpp>
#include <M5GFX.h>
#include <cstdint>
#include <string>

class Hal {
public:
    void init();
    void update();

    /* --------------------------------- System --------------------------------- */
    void delay(std::uint32_t ms)
    {
        m5gfx::delay(ms);
    }
    std::uint32_t millis()
    {
        return m5gfx::millis();
    }
    void feedTheDog();

    /* --------------------------------- Display -------------------------------- */
    M5GFX& display     = M5.Display;
    LGFX_Sprite canvas = LGFX_Sprite(&M5.Display);  // one band of the internal display

    lgfx::LovyanGFX& drawingSurface()
    {
        if (canvas.getBuffer()) return canvas;
        return display;
    }
    LGFX_Sprite& renderBuffer() { return canvas; }
    inline void pushCanvas(int top)
    {
        if (canvas.getBuffer()) canvas.pushSprite(0, top);
    }
    // The Cap TFT V2's 320x240 screen (ext_display.h); drawing to it without the cap attached is harmless.
    ExtDisplay ext;

    /* ---------------------------------- Input --------------------------------- */
    m5::Button_Class& homeButton = M5.BtnA;
    Keyboard keyboard;

    /* --------------------------------- SD Card -------------------------------- */
    // Read-only description of the inserted card (filesystem type, size, root entries).
    std::string sdCardInspect();
    // Erase the card and create a single FAT32 partition (32 KB clusters), then mount it.
    bool sdCardFormat(std::string& msg);
    // Mount the card if needed, without writing a probe file.
    bool sdCardMount()
    {
        if (!_is_sd_card_mounted) sd_card_init();
        return _is_sd_card_mounted;
    }

private:
    bool _is_sd_card_mounted = false;

    void display_init();
    void keyboard_init();
    void nvs_init();
    void spi_init();
    void ext_init();
    void sd_card_init();
};

Hal& GetHAL();
