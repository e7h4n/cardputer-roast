#include "roast_fonts.h"
#include "roast_fonts_data.h"
#include <lgfx/v1/misc/DataWrapper.hpp>

namespace roast::font {
namespace {
// Each font keeps its own reader over the flash array, so all eight can be used side by side without reloading.
struct Flash {
    lgfx::PointerWrapper data;
    lgfx::VLWfont font;
    bool loaded = false;
    const lgfx::IFont* get(const uint8_t* src)
    {
        if (!loaded) {
            data.set(src);
            loaded = font.loadFont(&data);
        }
        return &font;
    }
};
Flash f10, f11, f12b, f14, f16b, f19b, f23b, f47b;
}  // namespace

const lgfx::IFont* s10() { return f10.get(kFontSG10); }
const lgfx::IFont* s11() { return f11.get(kFontSG11); }
const lgfx::IFont* s12b() { return f12b.get(kFontSG12B); }
const lgfx::IFont* s14() { return f14.get(kFontSG14); }
const lgfx::IFont* s16b() { return f16b.get(kFontSG16B); }
const lgfx::IFont* s19b() { return f19b.get(kFontSG19B); }
const lgfx::IFont* s23b() { return f23b.get(kFontSG23B); }
const lgfx::IFont* s47b() { return f47b.get(kFontSG47B); }
}  // namespace roast::font
