#pragma once
#include <keymap.h>
#include <mooncake_log_signal.h>

class Keyboard {
public:
    struct KeyEvent_t {
        bool state             = false;
        bool isModifier        = false;
        KeScanCode_t keyCode   = KEY_NONE;
        const char* keyName    = "";
        uint8_t extraModifiers = 0;
    };
    mclog::Signal<const KeyEvent_t&> onKeyEvent;
    uint8_t mask = 0;
    uint8_t getModifierMask() { return mask; }
};
