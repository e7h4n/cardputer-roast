// Desktop preview stand-in for the Cardputer HAL: a real M5GFX sprite as the screen, a fake clock, no hardware.
#pragma once
#include <lgfx/v1/LGFX_Sprite.hpp>
#include <lgfx/v1/lgfx_fonts.hpp>
#include <hal/keyboard/keyboard.h>
#include <cstdint>
#include <string>

using LGFX_Sprite = lgfx::LGFX_Sprite;

struct FakeDisplay {
    void setBrightness(int) {}
    void fillScreen(uint32_t) {}
};
struct FakeButton {
    bool clicked = false;
    bool wasClicked()
    {
        bool c  = clicked;
        clicked = false;
        return c;
    }
};

class Hal {
public:
    LGFX_Sprite canvas;
    LGFX_Sprite ext;  // the external 320x240 screen (firmware: hal/ext_display.h)
    FakeDisplay display;
    FakeButton homeButton;
    Keyboard keyboard;
    bool sd = true;
    uint32_t millis();
    void delay(uint32_t) {}
    lgfx::LovyanGFX& drawingSurface() { return canvas; }
    void releaseCanvas() {}
    void pushCanvas() {}
    bool sdCardMount() { return sd; }
    void setRoastBleActive(bool) {}
    bool bleKeyboardInited() { return false; }
};
Hal& GetHAL();

struct FakePower {
    int level = 87;
    int getBatteryLevel() { return level; }
};
struct FakeM5 {
    FakePower Power;
};
extern FakeM5 M5;
