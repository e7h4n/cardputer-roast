/*
 * The external screen: Prokuon Cap TFT V2 for the Cardputer ADV, a 2.8" ILI9341 (320x240 landscape) wired to the EXT
 * header: SCK 40, MOSI 14 (the SD card's bus, SPI2), CS 5, DC 6, RST 3; the backlight is tied to 3.3 V. Write only
 * (no MISO wire). The SD card keeps the same bus: the panel takes the bus lock for every write (use_lock), and the
 * SD driver sets the bus up again for its own transfers. Init after the SD bus (Hal::spi_init): M5GFX would otherwise
 * create the bus with a 1-byte transfer limit.
 */
#pragma once
#include <M5GFX.h>
#include "Panel_ILI9341.hpp"  // from LovyanGFX (FreeBSD); M5GFX does not ship it

class ExtDisplay : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus;
    lgfx::Panel_ILI9341 _panel;

public:
    ExtDisplay()
    {
        auto b       = _bus.config();
        b.spi_host   = SPI2_HOST;
        b.spi_mode   = 0;
        b.freq_write = 20000000;
        b.freq_read  = 10000000;
        b.spi_3wire  = false;
        b.use_lock   = true;
        b.pin_sclk   = 40;
        b.pin_mosi   = 14;
        b.pin_miso   = -1;
        b.pin_dc     = 6;
        _bus.config(b);
        _panel.setBus(&_bus);
        auto p            = _panel.config();
        p.pin_cs          = 5;
        p.pin_rst         = 3;
        p.pin_busy        = -1;
        p.memory_width    = p.panel_width  = 240;
        p.memory_height   = p.panel_height = 320;
        p.offset_x        = p.offset_y     = 0;
        p.offset_rotation = 2;  // as mounted in the Cap TFT V2 (confirmed with the dual-screen test firmware)
        p.invert          = false;
        p.rgb_order       = false;
        p.readable        = false;
        p.bus_shared      = true;  // the SD card is on the same bus
        _panel.config(p);
        setPanel(&_panel);
    }
};
