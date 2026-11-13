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
struct FakeSpeaker {
    int v = 128;
    int getVolume() { return v; }
    void setVolume(int x) { v = x; }
    template <typename... A>
    void tone(A...) {}
    bool isPlaying() { return false; }
    bool begin() { return true; }
    void end() {}
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
    FakeDisplay display;
    FakeSpeaker speaker;
    FakeButton homeButton;
    Keyboard keyboard;
    bool sd = true;
    uint32_t millis();
    void delay(uint32_t) {}
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
