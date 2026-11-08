/*
 * Roast UI v2: warm light, geometric, colour-keyed. Roast owns the whole 240x135 screen.
 *  - colour = what it controls (roast_theme.h); key chips, values and chart lines of one function share a colour
 *  - one hero number per screen; secondary numbers are small cards with a colour bar
 *  - the bottom row always shows the keys that work right now; it turns into a full-width colour bar only when
 *    something needs a hand (green = drop window, red = act now, yellow = attention)
 *  - top-right status corner: battery, link dot / SIM tag, mute, missing SD
 */
#include "app_roast.h"
#include "fonts/roast_fonts.h"
#include <cmath>
#include <climits>
#include <cstring>
#include <algorithm>

using namespace roast;
using namespace roast::ui;

namespace {
constexpr int W     = 240;
constexpr int H     = 135;
constexpr int BAR_Y = 118;  // top of the key row / status bar
constexpr int KEY_Y = 121;

using Font  = const lgfx::IFont*;
using Datum = lgfx::textdatum_t;

Font F10() { return font::s10(); }
Font F11() { return font::s11(); }
Font F12() { return font::s12b(); }
Font F14() { return font::s14(); }
Font F16() { return font::s16b(); }
Font F19() { return font::s19b(); }
Font F23() { return font::s23b(); }
Font F47() { return font::s47b(); }

LGFX_Sprite& cv()
{
    return GetHAL().canvas;
}

int tw(const std::string& s, Font f)
{
    return cv().textWidth(s.c_str(), f);
}

int text(int x, int y, const std::string& s, Font f, uint32_t c, Datum d = Datum::top_left)
{
    cv().setFont(f);
    cv().setTextDatum(d);
    cv().setTextColor(c);
    cv().drawString(s.c_str(), x, y);
    return tw(s, f);
}

// Shorten to fit, dropping whole UTF-8 characters and appending "…".
std::string clipTo(const std::string& s, int maxPx, Font f)
{
    if (tw(s, f) <= maxPx) return s;
    std::string r = s;
    while (!r.empty() && tw(r + "…", f) > maxPx) {
        size_t i = r.size() - 1;
        while (i > 0 && ((unsigned char)r[i] & 0xC0) == 0x80) i--;
        r.erase(i);
    }
    return r + "…";
}

std::string fmt1(float v)
{
    if (std::isnan(v)) return "--";
    char b[16];
    snprintf(b, sizeof(b), "%.1f", v);
    return b;
}

std::string fmt0(float v)
{
    if (std::isnan(v)) return "--";
    char b[16];
    snprintf(b, sizeof(b), "%.0f", v);
    return b;
}

std::string signed1(float v)
{
    if (std::isnan(v)) return "--";
    char b[16];
    snprintf(b, sizeof(b), "%+.1f", v);
    return b;
}

std::string mmss(int s)
{
    return FormatMmSs(std::max(0, s));
}

bool lightColour(uint32_t c)
{
    return c == DRY || c == WARN || c == SIM || c == ui::WHITE || c == CARD || c == FAINT;
}

void dot(int cx, int cy, int r, uint32_t c)
{
    cv().fillSmoothCircle(cx, cy, r, c);
}

void ring(int cx, int cy, int r, uint32_t c, int width = 1)
{
    for (int i = 0; i < width; i++) cv().drawCircle(cx, cy, r - i, c);
}

void check(int cx, int cy, uint32_t c, int s = 3)
{
    cv().drawWideLine(cx - s, cy, cx - 1, cy + s - 1, 0.9f, c);
    cv().drawWideLine(cx - 1, cy + s - 1, cx + s + 1, cy - s + 1, 0.9f, c);
}

void rbox(int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    cv().fillSmoothRoundRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, r, c);
}

int pill(int x, int y, const std::string& s, uint32_t c)
{
    int w = tw(s, F10()) + 8;
    rbox(x, y, x + w, y + 11, 5, c);
    text(x + w / 2, y + 6, s, F10(), lightColour(c) ? INK : ui::WHITE, Datum::middle_center);
    return x + w;
}

// ---- key chips: "D D" = press twice, drawn as two chips
struct Key {
    const char* k;
    uint32_t col;
    const char* label;
};

int chipW(const std::string& k)
{
    return std::max(13, tw(k, F10()) + 7);
}

std::vector<std::string> splitKeys(const char* k)
{
    std::vector<std::string> v;
    std::string s = k;
    size_t p      = 0;
    while (true) {
        size_t q = s.find(' ', p);
        v.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return v;
}

int keysWidth(const std::vector<Key>& items, Font lf)
{
    int x = 3;
    for (auto& it : items) {
        for (auto& one : splitKeys(it.k)) x += chipW(one) + 2;
        x += it.label ? tw(it.label, lf) + 1 + (lf == F11() ? 9 : 6) : 4;
    }
    return x;
}

int chip(int x, int y, const std::string& k, uint32_t col, bool hollow = false)
{
    int w = chipW(k);
    if (hollow) {
        rbox(x, y, x + w - 1, y + 12, 6, CARD);
        cv().drawRoundRect(x, y, w, 13, 6, col);
    } else {
        rbox(x, y, x + w - 1, y + 12, 6, col);
    }
    text(x + w / 2, y + 7, k, F10(), hollow ? col : (lightColour(col) ? INK : ui::WHITE), Datum::middle_center);
    return x + w + 2;
}

// Keys in priority order. Shrinks labels first, then drops trailing labels, so nothing is ever clipped.
void keybar(std::vector<Key> items)
{
    cv().drawFastHLine(0, BAR_Y, W, LINE);
    Font lf = F11();
    if (keysWidth(items, lf) > W - 2) lf = F10();
    for (int i = (int)items.size() - 1; i >= 0 && keysWidth(items, lf) > W - 2; i--) items[i].label = nullptr;
    int x = 3;
    for (auto& it : items) {
        for (auto& one : splitKeys(it.k)) x = chip(x, KEY_Y, one, it.col);
        if (it.label)
            x += text(x + 1, KEY_Y + 7, it.label, lf, INK, Datum::middle_left) + (lf == F11() ? 9 : 6);
        else
            x += 2;
    }
}

// A double-press is armed: the first chip is filled, the second hollow, and the row says what happens next.
void confirmRow(const char* k, const char* what)
{
    cv().fillRect(0, BAR_Y, W, H - BAR_Y, CARD);
    cv().drawFastHLine(0, BAR_Y, W, DROP);
    int x = chip(3, KEY_Y, k, DROP);
    x     = chip(x, KEY_Y, k, DROP, true);
    text(x + 2, KEY_Y + 7, what, F11(), INK, Datum::middle_left);
}

// Full-width status bar with optional key dots on the right (e.g. "D D").
void bar(uint32_t col, const std::string& s, const char* keys = nullptr, uint32_t fg = ui::WHITE)
{
    cv().fillRect(0, BAR_Y, W, H - BAR_Y, col);
    int right = W - 4;
    if (keys) {
        auto ks = splitKeys(keys);
        right   = W - 4 - (int)ks.size() * 16;
        int x   = right + 2;
        for (auto& one : ks) {
            dot(x + 7, BAR_Y + 9, 7, fg == ui::WHITE ? ui::WHITE : INK);
            text(x + 7, BAR_Y + 9, one, F10(), col, Datum::middle_center);
            x += 16;
        }
    }
    text(5, BAR_Y + 9, clipTo(s, right - 8, F14()), F14(), fg, Datum::middle_left);
}

void metric(int x, int y, uint32_t col, const char* label, const std::string& val, bool emph = false,
            const char* sub = nullptr)
{
    cv().fillRect(x, y + 1, 3, 22, col);
    text(x + 7, y - 1, label, F11(), DIM);
    int w = text(x + 7, y + 9, val, emph ? F16() : F14(), col);
    if (sub && *sub) text(x + 10 + w, y + 13, sub, F10(), DIM);
}

// Roast timeline: drying (sage) -> Maillard (yellow) -> development (brown), planned marks as violet ticks.
struct Strip {
    int x0, x1;
    float span;
    int X(float s) const
    {
        return x0 + (int)((x1 - x0) * std::min(1.0f, std::max(0.0f, s / span)));
    }
};

Strip phaseStrip(int y, int tNow, int dryT, int fcT, float span, int planFc, int planEnd, bool ticks = true)
{
    Strip st{4, W - 5, span};
    rbox(st.x0, y, st.x1, y + 4, 2, LINE);
    int b = dryT >= 0 ? dryT : tNow;
    if (b > 0) rbox(st.X(0), y, std::max(st.X(0) + 2, st.X(b)), y + 4, 2, PH_DRY);
    if (dryT >= 0) {
        int e = fcT >= 0 ? fcT : tNow;
        if (e > dryT) cv().fillRect(st.X(dryT), y, std::max(1, st.X(e) - st.X(dryT)), 5, PH_MAI);
    }
    if (fcT >= 0 && tNow > fcT) cv().fillRect(st.X(fcT), y, std::max(1, st.X(tNow) - st.X(fcT)), 5, PH_DEV);
    if (ticks) {
        for (int s : {planFc, planEnd})
            if (s > tNow && s > 0) cv().drawFastVLine(st.X(s), y - 2, 9, CURVE);
    }
    dot(st.X(tNow), y + 2, 3, INK);
    return st;
}

void darken(int y0, int y1)
{
    // dim what is behind a sheet: RGB565 stored byte-swapped in the sprite buffer
    auto* buf = (uint16_t*)cv().getBuffer();
    if (!buf) return;
    for (int y = std::max(0, y0); y < std::min(H, y1); y++) {
        for (int x = 0; x < W; x++) {
            uint16_t v = buf[y * W + x];
            v          = (uint16_t)((v >> 8) | (v << 8));
            uint16_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            v          = (uint16_t)(((r * 9 / 16) << 11) | ((g * 9 / 16) << 5) | (b * 9 / 16));
            buf[y * W + x] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
}

uint32_t markColour(Mark m)
{
    switch (m) {
        case Mark::Dry:
            return DRY;
        case Mark::FCs:
            return FC;
        case Mark::FCe:
            return FCE;
        default:
            return DROP;
    }
}
}  // namespace

/* ================================================================== frame */
void AppRoast::render()
{
    cv().fillScreen(BG);
    switch (_screen) {
        case Screen::Connect:
            renderConnect();
            break;
        case Screen::Prep:
            renderPrep();
            break;
        case Screen::Roast:
            renderRoast();
            break;
        case Screen::Cool:
            renderCool();
            break;
        case Screen::Summary:
            renderSummary();
            break;
        case Screen::Curves:
            renderCurves();
            break;
        case Screen::Editor:
            renderEditor();
            break;
        case Screen::Settings:
            renderSettings();
            break;
        case Screen::Help:
            renderHelp();
            break;
    }
    renderStatus();
    if (_batch.active) renderBatch();
    if (_input.active) renderInput();
    renderToast();
    GetHAL().pushCanvas();
}

bool AppRoast::confirmPending(int key) const
{
    return _pendingKey == key && GetHAL().millis() < _pendingUntil;
}

/* ========================================================= status corner */
void AppRoast::renderStatus()
{
    int x       = W - 3;
    uint32_t bc = _batPct < 0 ? DIM : (_batPct <= 15 ? DROP : (_batPct <= 30 ? HEAT : INK));
    cv().drawRect(x - 16, 3, 14, 8, bc);
    cv().fillRect(x - 2, 5, 2, 4, bc);
    if (_batPct > 0) cv().fillRect(x - 14, 5, std::max(1, 10 * std::min(100, _batPct) / 100), 4, bc);
    std::string pct = _batPct < 0 ? "--" : std::to_string(_batPct) + "%";
    x -= 19 + text(x - 19, 1, pct, F11(), bc, Datum::top_right);
    if (_muted) {  // speaker with a red slash
        x -= 13;
        cv().fillRect(x, 4, 3, 5, DIM);
        cv().fillTriangle(x + 2, 6, x + 7, 1, x + 7, 11, DIM);
        cv().drawLine(x - 1, 11, x + 9, 1, DROP);
    }
    if (!_sdOk) {
        int w = tw("SD", F10()) + 8;
        x -= w + 4;
        pill(x, 1, "SD", DROP);
    }
    if (_link && _link->isSimulated()) {
        int w = tw("SIM", F10()) + 8;
        x -= w + 4;
        pill(x, 1, "SIM", SIM);
    } else {
        x -= 8;
        uint32_t c = _linkState == LinkState::Ready ? GO : (_linkState == LinkState::Lost ? DROP : FAINT);
        dot(x, 6, 3, c);
    }
}

/* =============================================================== connect */
void AppRoast::renderConnect()
{
    text(4, 1, "Roast", F16(), INK);
    int cx = 34, cy = 54;
    const uint32_t rings[3] = {0xE2DED6, 0xD6D1C8, LINE};
    const int rr[3]         = {24, 17, 10};
    int pulse               = (GetHAL().millis() / 500) % 3;
    for (int i = 0; i < 3; i++) ring(cx, cy, rr[i], i == 2 - pulse ? FAINT : rings[i], 2);
    dot(cx, cy, 4, INK);
    std::string top = "Searching for", big = "E-Meter…BLE";
    uint32_t bigc = INK;
    if (_linkState == LinkState::Connecting) {
        top = "Connecting";
        big = _link ? _link->deviceName() : "";
    } else if (_linkState == LinkState::Error) {
        top  = "BLE error";
        big  = "Enter to retry";
        bigc = DROP;
    } else if (_linkState == LinkState::Ready) {
        top = "Connected";
        big = _link ? _link->deviceName() : "";
    }
    text(70, 33, top, F11(), DIM);
    text(70, 46, clipTo(big, W - 74, F19()), F19(), bigc);
    std::string err = _link ? _link->lastError() : "";
    if (!err.empty()) {
        text(70, 70, clipTo(err, W - 74, F10()), F10(), DROP);
    } else {
        text(70, 70, "Close myERoast on the phone", F11(), DIM);
        text(70, 82, "first: one connection only", F11(), DIM);
    }
    if (_link) {
        auto res = _link->scanResults();
        if (!res.empty()) {
            auto& r = res.front();
            rbox(4, 96, W - 5, 112, 4, CARD);
            text(10, 104, clipTo(r.name, 120, F11()), F11(), INK, Datum::middle_left);
            int bars = r.rssi > -60 ? 4 : r.rssi > -70 ? 3 : r.rssi > -80 ? 2 : 1;
            for (int i = 0; i < 4; i++) {
                int h = (i + 1) * 2 + 1;
                cv().fillRect(W - 36 + i * 5, 108 - h, 3, h, i < bars ? INK : LINE);
            }
            text(W - 42, 104, std::to_string(r.rssi) + " dBm", F10(), DIM, Datum::middle_right);
        }
    }
    keybar({{"Enter", INK, "retry"}, {"D", SIM, "simulator"}, {"H", FAINT, "keys"}, {"Esc", FAINT, "exit"}});
}

/* ================================================================== prep */
void AppRoast::renderPrep()
{
    if (_view == 1 && _hasCurve) {
        renderChart(0, 14, W - 1, BAR_Y - 1, true);
        dot(7, 7, 3, CURVE);
        text(14, 0, clipTo(_curve.name, W - 70, F14()), F14(), INK);
        keybar({{"Tab", CURVE, "back"}, {"E", CURVE, "edit"}});
        return;
    }
    // curve line
    if (_hasCurve) {
        dot(7, 7, 3, CURVE);
        text(14, 0, clipTo(_curve.name, W - 70, F14()), F14(), INK);
        std::string sum;
        int fcT = _curve.tagTime(PointTag::FCs);
        if (fcT >= 0) sum += "FC " + mmss(fcT) + " · ";
        sum += "drop " + mmss(_curve.endTime()) + " · " + fmt0(_curve.at(_curve.endTime())) + "°";
        text(14, 16, sum, F11(), DIM);
    } else {
        dot(7, 7, 3, HEAT);
        text(14, 0, "No curve", F14(), INK);
        text(14, 16, "heat by hand with ←→ after charge", F11(), DIM);
    }
    // hero: bean temperature
    std::string bt = _snap.has(4) ? fmt1(_snap.bt()) : "--";
    bool warm      = _snap.has(4) && _snap.bt() >= _rules.coldBT;
    int bw         = text(4, 30, bt, F47(), warm ? HEAT : INK);
    if (8 + bw + tw("warm", F11()) < 156) {
        text(8 + bw, 37, "°C", F14(), DIM);
        text(8 + bw, 53, warm ? "warm" : "bean", F11(), warm ? HEAT : DIM);
    }
    // fan card
    bool on   = _phase == Phase::FanOn;
    float fan = (_snap.has(12) && on) ? _snap.fan() : _rules.startFan;
    rbox(160, 31, W - 5, 76, 5, CARD);
    dot(169, 39, 3, FAN);
    text(176, 33, on ? "fan on" : "fan preset", F11(), DIM);
    text(167, 45, fmt1(fan) + "%", F19(), FAN);
    text(167, 69, on ? "no heat yet" : "↑↓ to adjust", F10(), DIM, Datum::middle_left);
    // step rail: curve -> batch -> fan -> charge
    struct Step {
        const char* label;
        std::string value;
        bool done;
        uint32_t col;
    } steps[4] = {
        {"curve", _hasCurve ? clipTo(_curve.name, 64, F10()) : (_noCurve ? "manual" : "none"), _hasCurve || _noCurve,
         _hasCurve ? CURVE : HEAT},
        {"batch", fmt0(_rules.greenG) + " g", _rules.greenG > 0, INK},
        {"fan", fmt0(fan) + "%", on, FAN},
        {"charge", "Enter", false, GO},
    };
    int cur = on ? 3 : 2;
    const int y = 88, x0 = 12, x1 = W - 14;
    cv().drawFastHLine(x0, y, x1 - x0, LINE);
    for (int i = 0; i < 4; i++) {
        int cx  = x0 + (x1 - x0) * i / 3;
        auto& s = steps[i];
        if (s.done) {
            dot(cx, y, 6, s.col);
            check(cx, y, ui::WHITE);
        } else if (i == cur) {
            dot(cx, y, 6, BG);
            ring(cx, y, 6, s.col, 2);
            dot(cx, y, 2, s.col);
        } else {
            dot(cx, y, 5, BG);
            ring(cx, y, 5, FAINT);
        }
        Datum d  = i == 0 ? Datum::top_left : (i == 3 ? Datum::top_right : Datum::top_center);
        int tx   = cx + (i == 0 ? -6 : (i == 3 ? 6 : 0));
        bool hot = s.done || i == cur;
        text(tx, y + 9, s.label, F10(), hot ? INK : DIM, d);
        text(tx, y + 19, s.value, F10(), DIM, d);
    }
    if (confirmPending(KEY_X))
        confirmRow("X", "press X again to stop the fan");
    else if (on)
        keybar({{"Enter", GO, "charge"}, {"↑ ↓", FAN, "fan"}, {"X X", DROP, "stop"}});
    else
        keybar({{"S", FAN, "fan on"}, {"C", CURVE, "curve"}, {"W", INK, "batch"}, {"O", INK, "rules"}});
}

/* ============================================================ batch sheet */
void AppRoast::renderBatch()
{
    darken(0, 32);
    rbox(0, 32, W - 1, H + 8, 8, CARD);
    rbox(W / 2 - 14, 35, W / 2 + 14, 37, 1, FAINT);
    text(8, 41, "Batch", F16(), INK);
    text(W - 8, 45, "saved with the log", F10(), DIM, Datum::top_right);
    bool blink = (GetHAL().millis() / 400) % 2;
    auto field = [&](int fx0, int fx1, const char* label, const std::string& v, bool active, const char* unit) {
        text(fx0, 62, label, F11(), DIM);
        rbox(fx0, 74, fx1, 94, 4, ui::WHITE);
        cv().drawRoundRect(fx0, 74, fx1 - fx0 + 1, 21, 4, active ? INK : LINE);
        if (active) cv().drawRoundRect(fx0 + 1, 75, fx1 - fx0 - 1, 19, 3, INK);
        int w = text(fx0 + 6, 84, clipTo(v, fx1 - fx0 - 20, F14()), F14(), INK, Datum::middle_left);
        if (active && blink) cv().fillRect(fx0 + 7 + w, 78, 1, 12, INK);
        if (unit) text(fx1 - 6, 84, unit, F12(), DIM, Datum::middle_right);
    };
    field(8, 112, "green weight", _batch.weight, _batch.field == 0, "g");
    field(124, W - 9, "bean", _batch.bean, _batch.field == 1, nullptr);
    keybar({{"Tab", INK, "next"}, {"Enter", GO, "save"}, {"Esc", FAINT, "cancel"}});
}

/* ================================================================= roast */
void AppRoast::renderRoast()
{
    if (_view == 1) {
        renderChart(0, 14, W - 1, BAR_Y - 1, true);
        int t   = roastT();
        float b = _snap.has(4) ? _snap.bt() : _session.lastBT();
        int x   = 4 + text(4, 0, mmss(t), F14(), INK);
        x += 6 + text(x + 6, 0, fmt1(b) + "°", F14(), linkReady() ? INK : FAINT);
        text(x + 12, 3, "RoR " + fmt1(_session.rorNow()), F11(), DIM);
    } else {
        renderDash();
    }
    if (!renderAlertBar()) renderKeysBar();
}

void AppRoast::renderDash()
{
    int t    = roastT();
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    // clock + mode
    int x = 4 + text(4, 2, mmss(t), F23(), INK);
    pill(x + 6, 8, _manual ? "MAN" : "PID", _manual ? HEAT : CURVE);
    // hero: bean temperature
    bool stale = !linkReady();
    bool alarm = bt >= _rules.maxDropBT;
    text(W - 4, 16, fmt1(bt), F47(), stale ? FAINT : (alarm ? DROP : INK), Datum::top_right);
    // target / deviation
    float tgt = curveTargetAt(curveT());
    if (stale) {
        int age = _snap.lastRxMs ? (int)((GetHAL().millis() - _snap.lastRxMs) / 1000) : 0;
        text(4, 32, "no reading " + mmss(age), F11(), DROP);
        if (_hasCurve) text(4, 45, signed1(bt - tgt) + "°", F14(), FAINT);
    } else if (_manual) {
        text(4, 32, "manual heat", F11(), HEAT);
        if (_hasCurve) text(4, 45, signed1(bt - tgt) + "° vs curve", F11(), DIM);
    } else if (_hasCurve) {
        text(4, 32, "target " + fmt1(tgt), F11(), DIM);
        text(4, 45, signed1(bt - tgt) + "°", F14(), INK);
    } else {
        text(4, 32, "no curve", F11(), DIM);
    }
    // three cards: the two things you can turn, and the rate of rise
    const int y = 70;
    if (_manual) rbox(2, y - 3, 76, y + 25, 4, HEAT_TINT);
    metric(5, y, HEAT, "heat", fmt0(_snap.heat()) + "%", _manual);
    metric(84, y, FAN, "fan", fmt1(_snap.fan()) + "%");
    const char* trend = "";
    auto& ss          = _session.samples();
    if (ss.size() > 16 && ss.back().ror10 != INT16_MIN && ss[ss.size() - 16].ror10 != INT16_MIN) {
        int d = ss.back().ror10 - ss[ss.size() - 16].ror10;
        trend = d < -3 ? "↘" : (d > 3 ? "↗" : "");
    }
    metric(160, y, INK, "RoR", fmt1(_session.rorNow()), false, trend);
    // timeline
    int dry = _session.markT(Mark::Dry), fc = _session.markT(Mark::FCs);
    int planFc  = _hasCurve ? _curve.tagTime(PointTag::FCs) : -1;
    int planEnd = _hasCurve ? _curve.endTime() : -1;
    float span  = std::max({600.0f, (float)planEnd + 30, (float)t + 30});
    const int yb = 99;
    Strip st     = phaseStrip(yb, t, dry, fc, span, planFc, planEnd);
    if (fc >= 0) {
        int dev  = t - fc;
        float dT = bt - _session.markBT(Mark::FCs);
        int wx0 = st.X(fc + _rules.devMin), wx1 = st.X(fc + _rules.devMax);
        cv().drawRect(wx0, yb - 2, std::max(2, wx1 - wx0 + 1), 9, GO);
        char b[64];
        snprintf(b, sizeof(b), "dev %s · %+.1f° · %.0f%%", mmss(dev).c_str(), dT, t > 0 ? 100.0 * dev / t : 0.0);
        text(4, yb + 6, b, F10(), INK);
        text(W - 5, yb + 6, "window " + std::to_string(_rules.devMin) + "–" + std::to_string(_rules.devMax) + " s",
             F10(), GO, Datum::top_right);
    } else {
        if (dry >= 0)
            text(4, yb + 6, "dry " + mmss(dry), F10(), DRY);
        else
            text(4, yb + 6, "Y at end of drying", F10(), DIM);
        if (planFc > 0)
            text(W - 5, yb + 6, "FC expected " + mmss(planFc) + " · in " + mmss(planFc - t), F10(), CURVE,
                 Datum::top_right);
        else
            text(W - 5, yb + 6, "F at first crack", F10(), DIM, Datum::top_right);
    }
}

// Full-width colour bar when the roast needs a hand. Returns false when the normal key row should show instead.
bool AppRoast::renderAlertBar()
{
    if (_phase != Phase::Roasting) return false;
    if (confirmPending(KEY_D)) {
        confirmRow("D", "press D again to drop");
        return true;
    }
    char b[80];
    int t    = roastT();
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    if (!linkReady()) {
        snprintf(b, sizeof(b), "BLE lost · holding %.0f° · retrying", _lastTargetRaw > 0 ? _lastTargetRaw / 10.0 : bt);
        bar(DROP, b);
        return true;
    }
    int fc   = _session.markT(Mark::FCs);
    int dev  = fc >= 0 ? t - fc : -1;
    float dT = fc >= 0 ? bt - _session.markBT(Mark::FCs) : 0;
    if (bt >= _rules.maxDropBT) {
        snprintf(b, sizeof(b), "%.0f° reached · drop now", _rules.maxDropBT);
        bar(DROP, b, "D D");
        return true;
    }
    if (fc >= 0 && dev >= _rules.devMax) {
        snprintf(b, sizeof(b), "dev %s · drop now", mmss(dev).c_str());
        bar(DROP, b, "D D");
        return true;
    }
    if (_heatDropWarned && fc >= 0 && dev <= 75 && !_manual) {
        bar(WARN, "heat falling after FC · P manual", nullptr, INK);
        return true;
    }
    if (_satWarned && _satRun > 0) {
        snprintf(b, sizeof(b), "heat at max %.0f%% for %d s", _snap.heat(), _satRun);
        bar(WARN, b, nullptr, INK);
        return true;
    }
    if (fc >= 0 && dev >= _rules.devMin && dT >= _rules.dtMin) {
        snprintf(b, sizeof(b), "drop window · %s · %+.1f°", mmss(dev).c_str(), dT);
        bar(GO, b, "D D");
        return true;
    }
    if (fc >= 0 && dT >= _rules.dtMax && dev < _rules.devMin) {
        snprintf(b, sizeof(b), "%+.1f° already · drop at %d s", dT, _rules.devMin);
        bar(WARN, b, nullptr, INK);
        return true;
    }
    if (_rorWarned) {
        snprintf(b, sizeof(b), "RoR %.1f is low (<%.0f)", _session.rorNow(), _rules.rorLow);
        bar(WARN, b, nullptr, INK);
        return true;
    }
    return false;
}

void AppRoast::renderKeysBar()
{
    int dry = _session.markT(Mark::Dry), fc = _session.markT(Mark::FCs), fce = _session.markT(Mark::FCe);
    if (_view == 1)
        keybar({{"Tab", CURVE, "numbers"}, {"D D", DROP, "drop"}, {"↑ ↓", FAN, "fan"}});
    else if (_manual)
        keybar({{"← →", HEAT, "heat"}, {"P", CURVE, "PID"}, {"↑ ↓", FAN, "fan"}, {"D D", DROP, nullptr}});
    else if (fc >= 0 && fce < 0)
        keybar({{"G", FCE, "FC end"}, {"D D", DROP, "drop"}, {"↑ ↓", FAN, "fan"}, {"P", HEAT, "manual"}});
    else if (fc >= 0)
        keybar({{"D D", DROP, "drop"}, {"↑ ↓", FAN, "fan"}, {"P", HEAT, "manual"}, {"Tab", CURVE, "chart"}});
    else if (dry >= 0)
        keybar({{"F", FC, "1st crack"}, {"↑ ↓", FAN, "fan"}, {"P", HEAT, "manual"}, {"Tab", CURVE, "chart"}});
    else
        keybar({{"Y", DRY, "dry end"}, {"↑ ↓", FAN, "fan"}, {"P", HEAT, "manual"}, {"Tab", CURVE, "chart"}});
}

/* ================================================================= chart */
void AppRoast::renderChart(int x0, int y0, int x1, int y1, bool withText)
{
    const bool live = _session.active() || _phase == Phase::Done;
    int lastT       = live ? _session.lastT() : 0;
    int tMax        = std::max({_hasCurve ? _curve.endTime() + 30 : 0, lastT + 30, 360});
    tMax            = ((tMax + 59) / 60) * 60;
    const float yMin = 20, yMax = 235;
    const int pl     = 18;  // left gutter for temperature labels
    auto PX = [&](float t) { return x0 + pl + (int)((x1 - x0 - pl) * t / tMax); };
    auto PY = [&](float c) {
        float v = std::max(yMin, std::min(yMax, c));
        return y1 - (int)((y1 - y0) * (v - yMin) / (yMax - yMin));
    };
    for (int s = 60; s < tMax; s += 60) cv().drawFastVLine(PX(s), y0, y1 - y0, GRID);
    for (int c = 50; c < (int)yMax; c += 50) {
        cv().drawFastHLine(x0 + pl, PY(c), x1 - x0 - pl, GRID);
        text(x0 + pl - 2, PY(c), std::to_string(c), F10(), FAINT, Datum::middle_right);
    }
    auto& ss = _session.samples();
    // heater history: orange area along the bottom (16 % of the height at 100 %)
    if (live) {
        for (size_t i = 0; i + 1 < ss.size(); i++) {
            int h = (int)(ss[i].heat10 / 1000.0f * (y1 - y0) * 0.16f);
            if (h > 0) cv().fillRect(PX(ss[i].t), y1 - h, std::max(1, PX(ss[i + 1].t) - PX(ss[i].t)), h, HEAT_AREA);
        }
    }
    // target curve
    if (_hasCurve) {
        int step = std::max(1, tMax / (x1 - x0));
        int px = PX(0), py = PY(_curve.at(0));
        for (int s = step; s <= _curve.endTime(); s += step) {
            int nx = PX(s), ny = PY(_curve.at(s));
            cv().drawLine(px, py, nx, ny, CURVE);
            px = nx;
            py = ny;
        }
    }
    if (live) {
        // rate of rise in the lower part (0..30 °C/min mapped onto 20..155 °C of the axis)
        int rpx = -1, rpy = -1;
        for (auto& s : ss) {
            if (s.ror10 == INT16_MIN) continue;
            int nx = PX(s.t), ny = PY(yMin + std::min(30.0f, s.ror10 / 10.0f) * 4.5f);
            if (rpx >= 0) cv().drawLine(rpx, rpy, nx, ny, ROR);
            rpx = nx;
            rpy = ny;
        }
        // bean temperature, 2 px
        int ppx = -1, ppy = -1;
        for (auto& s : ss) {
            int nx = PX(s.t), ny = PY(s.bt10 / 10.0f);
            if (ppx >= 0) {
                cv().drawLine(ppx, ppy, nx, ny, INK);
                cv().drawLine(ppx, ppy + 1, nx, ny + 1, INK);
            }
            ppx = nx;
            ppy = ny;
        }
        for (int m = 0; m < (int)Mark::Count; m++) {
            int mt = _session.markT((Mark)m);
            if (mt >= 0) dot(PX(mt), PY(_session.markBT((Mark)m)), 3, markColour((Mark)m));
        }
        if (_phase == Phase::Roasting) cv().drawFastVLine(PX(roastT()), y0, y1 - y0, FAINT);
        if (withText) {  // legend, coloured like the keys
            int lx = x0 + pl + 4, ly = y0 + 3;
            const std::pair<uint32_t, const char*> lg[] = {{INK, "bean"}, {CURVE, "target"}, {ROR, "RoR"}, {HEAT, "heat"}};
            for (auto& l : lg) {
                cv().fillRect(lx, ly + 5, 7, 2, l.first);
                lx += 10 + text(lx + 10, ly, l.second, F10(), DIM) + 6;
            }
        }
    } else if (_hasCurve) {
        // curve preview: label the key nodes in their mark colours
        for (auto& p : _curve.pts) {
            uint32_t c = p.tag == PointTag::Dry ? DRY : p.tag == PointTag::FCs ? FC : p.tag == PointTag::Drop ? DROP : 0;
            if (!c) continue;
            int px = PX(p.t), py = PY(_curve.at(p.t));
            dot(px, py, 3, c);
            if (p.tag == PointTag::Dry)
                text(px + 5, py + 3, "dry " + mmss(p.t) + " · " + fmt0(_curve.at(p.t)) + "°", F10(), c);
            else if (p.tag == PointTag::FCs)
                text(px - 4, py - 3, "FC " + mmss(p.t) + " · " + fmt0(_curve.at(p.t)) + "°", F10(), c, Datum::bottom_right);
            else
                text(px, py + 6, "drop " + mmss(p.t), F10(), c, Datum::top_right);
        }
        dot(PX(0), PY(_curve.at(0)), 2, INK);
    }
}

/* ================================================================== cool */
void AppRoast::renderCool()
{
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    if (_view == 1) {
        renderChart(0, 14, W - 1, BAR_Y - 1, true);
        text(4, 0, "Cooling", F14(), INK);
        keybar({{"Tab", CURVE, "numbers"}, {"X X", DROP, "stop fan"}, {"Enter", INK, "finish"}});
        return;
    }
    int since         = _dropMs ? (int)((GetHAL().millis() - _dropMs) / 1000) : 0;
    std::string title = _autoDropped ? "Auto-dropped" : "Cooling";
    int x             = 4 + text(4, 0, title, F16(), _autoDropped ? DROP : INK);
    text(x + 6, 5, "+" + mmss(since) + " since drop", F11(), DIM);
    bool cold = bt < _rules.coldBT;
    int bw    = text(4, 22, fmt1(bt), F47(), cold ? GO : INK);
    if (8 + bw + tw("°C", F14()) < 156) text(8 + bw, 29, "°C", F14(), DIM);
    // cooling rate over the last 30 s of samples
    auto& ss   = _session.samples();
    float rate = NAN;
    if (ss.size() > 31) rate = (ss[ss.size() - 31].bt10 - ss.back().bt10) / 10.0f * 2.0f;
    rbox(160, 24, W - 5, 72, 5, CARD);
    dot(169, 33, 3, FAN);
    text(176, 27, "fan", F11(), DIM);
    text(167, 40, fmt1(_snap.fan()) + "%", F19(), FAN);
    if (!std::isnan(rate) && rate > 0.1f) text(167, 62, "↓ " + fmt0(rate) + "°/min", F10(), FAN);
    // progress from the drop temperature to the cold threshold
    RoastMetrics m = _session.metrics();
    float dropBT   = m.markBT[3] > 0 ? m.markBT[3] : bt;
    float frac     = dropBT > _rules.coldBT ? (dropBT - bt) / (dropBT - _rules.coldBT) : 1.0f;
    frac           = std::max(0.0f, std::min(1.0f, frac));
    const int y = 80, x0 = 4, x1 = W - 5;
    rbox(x0, y, x1, y + 4, 2, LINE);
    rbox(x0, y, x0 + std::max(3, (int)((x1 - x0) * frac)), y + 4, 2, cold ? GO : FAN);
    dot(x0 + (int)((x1 - x0) * frac), y + 2, 3, INK);
    text(x0, y + 7, "drop " + fmt0(dropBT) + "°", F10(), DIM);
    if (cold) {
        text(x1, y + 7, "below " + fmt0(_rules.coldBT) + "° · OK to stop", F10(), GO, Datum::top_right);
    } else {
        std::string eta;
        if (!std::isnan(rate) && rate > 0.5f) eta = " · ~" + mmss((int)((bt - _rules.coldBT) / rate * 60));
        text(x1, y + 7, "stop below " + fmt0(_rules.coldBT) + "°" + eta, F10(), INK, Datum::top_right);
    }
    if (_autoDropped) {
        text(4, 101, clipTo("auto-drop: " + _autoDropWhy, W - 8, F11()), F11(), DROP);
    } else {
        char b[80];
        snprintf(b, sizeof(b), "drop %s · dev %s · %+.1f° · %s%%", mmss(m.markT[3]).c_str(),
                 m.dev >= 0 ? mmss(m.dev).c_str() : "--", m.deltaFC, fmt0(m.devPct).c_str());
        text(4, 101, b, F11(), DIM);
    }
    if (confirmPending(KEY_X))
        confirmRow("X", "press X again to stop the fan");
    else
        keybar({{"X X", DROP, "stop fan"}, {"Enter", INK, "finish"}, {"W", INK, "weight"}, {"Tab", CURVE, nullptr}});
}

/* =============================================================== summary */
void AppRoast::renderSummary()
{
    if (_view == 1) {
        renderChart(0, 14, W - 1, BAR_Y - 1, false);
        text(4, 0, _session.logId(), F14(), INK);
        keybar({{"Tab", CURVE, "summary"}, {"Enter", INK, "done"}});
        return;
    }
    RoastMetrics m  = _session.metrics();
    int x           = 4 + text(4, 0, _session.logId().empty() ? "Roast" : _session.logId(), F16(), INK);
    std::string sub = _bean;
    if (_hasCurve) sub += (sub.empty() ? "" : " · ") + _curve.name;
    text(x + 6, 5, clipTo(sub, W - x - 62, F11()), F11(), DIM);
    // three big numbers
    std::string lossLabel = std::isnan(m.lossPct)
                                ? std::string("loss · press W")
                                : "loss " + fmt0(_session.greenWeight()) + "→" + fmt0(_session.roastedWeight()) + " g";
    struct Stat {
        std::string v, l;
        int x;
    } stats[3] = {{mmss(m.total), "total", 4},
                  {m.dev >= 0 ? fmt1(m.devPct) + "%" : "--", "development", 72},
                  {std::isnan(m.lossPct) ? "--" : fmt1(m.lossPct) + "%", lossLabel, 152}};
    for (auto& s : stats) {
        text(s.x, 19, s.v, F19(), INK);
        text(s.x, 41, clipTo(s.l, (s.x == 152 ? W - 4 : s.x + 76) - s.x, F10()), F10(), DIM);
    }
    // the whole roast on one strip
    const int yb = 58;
    float span   = (float)std::max(60, m.total);
    Strip st     = phaseStrip(yb, m.total, m.markT[0], m.markT[1], span, -1, -1, false);
    dot(st.X(m.total), yb + 2, 3, DROP);
    if (m.markT[0] >= 0) text(4, yb + 7, "dry " + mmss(m.markT[0]) + " · " + fmt0(m.markBT[0]) + "°", F10(), DRY);
    if (m.markT[3] >= 0)
        text(W - 5, yb + 7, "drop " + mmss(m.markT[3]) + " · " + fmt0(m.markBT[3]) + "°", F10(), DROP, Datum::top_right);
    char b[96];
    if (m.markT[1] >= 0) {
        snprintf(b, sizeof(b), "FC %s at %s° · %s° to drop", mmss(m.markT[1]).c_str(), fmt1(m.markBT[1]).c_str(),
                 signed1(m.deltaFC).c_str());
        text(4, 79, b, F11(), FC);
    }
    snprintf(b, sizeof(b), "RoR %s→%s · max heat %s%% · err %s°", fmt1(m.rorAtFC).c_str(), fmt1(m.rorFC30).c_str(),
             fmt0(m.maxHeat).c_str(), fmt1(m.meanAbsErr).c_str());
    text(4, 91, b, F11(), DIM);
    bool updated = _saveMsg.rfind("Updated", 0) == 0;
    if (updated || _saveMsg.rfind("Saved", 0) == 0) {
        dot(8, 111, 4, GO);
        check(8, 111, ui::WHITE, 2);
        text(16, 111, clipTo(std::string(updated ? "updated " : "saved ") + _session.logId(), 140, F10()), F10(), GO,
             Datum::middle_left);
    } else if (!_saveMsg.empty()) {
        text(4, 111, clipTo(_saveMsg, 150, F10()), F10(), DROP, Datum::middle_left);
    }
    if (_fanRunning) {
        int w = tw("fan still on", F10()) + 8;
        pill(W - 4 - w, 105, "fan still on", FAN);
    }
    if (confirmPending(KEY_X))
        confirmRow("X", "press X again to stop the fan");
    else if (_fanRunning)
        keybar({{"Enter", INK, "done"}, {"X X", DROP, "stop"}, {"N", INK, "note"}, {"W", INK, "weight"}});
    else
        keybar({{"Enter", INK, "done"}, {"N", INK, "note"}, {"W", INK, "weight"}, {"Tab", CURVE, "chart"}});
}

/* ================================================================ curves */
void AppRoast::renderCurves()
{
    int n = (int)_curves.size() + 1;  // row 0 = "No curve"
    int x = 4 + text(4, 0, "Curves", F16(), INK);
    text(x + 4, 5, std::to_string(_curves.size()), F11(), DIM);
    int y = 18;
    for (int i = _listTop; i < n && i < _listTop + 3; i++) {
        bool sel = i == _listSel;
        if (sel) {
            rbox(2, y - 1, W - 8, y + 28, 5, CARD);
            cv().drawRoundRect(2, y - 1, W - 9, 30, 5, INK);
        }
        rbox(7, y + 3, 45, y + 25, 3, sel ? ui::WHITE : CARD);
        std::string name, meta;
        bool current = false;
        if (i == 0) {
            name    = "No curve";
            meta    = "heat by hand with ←→";
            current = !_hasCurve && _noCurve;
            text(26, y + 14, "←→", F11(), HEAT, Datum::middle_center);
        } else {
            auto& c = _curves[i - 1];
            name    = c.name;
            current = _hasCurve && c.name == _curve.name;
            int end = std::max(60, c.endTime());
            int px = -1, py = -1;
            for (int s = 0; s <= end; s += std::max(1, end / 36)) {
                int nx = 8 + 36 * s / end, ny = y + 24 - (int)(20 * (c.at(s) - 40) / 190);
                if (px >= 0) cv().drawLine(px, py, nx, ny, CURVE);
                px = nx;
                py = ny;
            }
            meta    = "drop " + mmss(c.endTime()) + " · " + fmt0(c.at(c.endTime())) + "°";
            int fcT = c.tagTime(PointTag::FCs);
            if (fcT >= 0) meta += " · FC " + mmss(fcT);
            if (c.batchG > 0) meta += " · " + std::to_string(c.batchG) + " g";
            if (c.builtin) meta += " · sample";
            if (c.readOnly) meta += " · alog";
        }
        text(52, y + 2, clipTo(name, W - 80, F12()), F12(), INK);
        text(52, y + 16, clipTo(meta, W - 62, F10()), F10(), DIM);
        if (current) {
            dot(W - 17, y + 8, 5, GO);
            check(W - 17, y + 8, ui::WHITE, 2);
        }
        y += 32;
    }
    if (n > 3) {  // scrollbar
        cv().fillRect(W - 4, 18, 2, 96, LINE);
        int top = 18 + 96 * _listTop / n, h = std::max(6, 96 * 3 / n);
        cv().fillRect(W - 4, top, 2, h, INK);
    }
    keybar({{"Enter", GO, "use"}, {"E", CURVE, "edit"}, {"N", CURVE, "new"}, {"Esc", FAINT, "back"}});
}

/* ================================================================ editor */
void AppRoast::renderEditor()
{
    if (_view == 1) {
        renderChart(0, 14, W - 1, BAR_Y - 1, true);
        dot(7, 7, 3, CURVE);
        text(14, 0, clipTo(_curve.name, W - 70, F14()), F14(), INK);
        keybar({{"Tab", CURVE, "back to edit"}, {"S", GO, "save"}});
        return;
    }
    static const char* names[8]   = {"Start", "Dry", "FC start", "FC end", "SC start", "SC end", "Drop", "Total"};
    static const uint32_t cols[8] = {INK, DRY, FC, FCE, FCE, FCE, DROP, INK};
    dot(7, 7, 3, CURVE);
    std::string sub = (_curve.batchG > 0 ? " · " + std::to_string(_curve.batchG) + " g" : std::string()) +
                      std::string(_edDirty ? " · edited" : "");
    std::string title = clipTo(_curve.name, W - 74 - tw(sub, F10()), F14());
    int x             = 14 + text(14, 0, title, F14(), INK);
    text(x + 2, 4, sub, F10(), DIM);
    int y = 17;
    for (int i = 0; i < kEdRows; i++) {
        const EdRow& r = _ed[i];
        bool sel       = i == _edRow;
        if (sel) {
            rbox(1, y - 1, 140, y + 10, 3, CARD);
            cv().fillRect(1, y - 1, 2, 12, CURVE);
        }
        if (r.on)
            dot(9, y + 5, 3, cols[i]);
        else
            ring(9, y + 5, 3, FAINT);
        text(17, y, names[i], F11(), r.on ? INK : FAINT);
        // time
        if (sel && (_edStage == 1 || _edStage == 2)) {
            std::string sec = r.t >= 0 ? ((r.t % 60 < 10 ? "0" : "") + std::to_string(r.t % 60)) : "00";
            std::string a   = _edStage == 1 ? _edBuf : _edMin;
            std::string b2  = _edStage == 2 ? _edBuf : sec;
            int bx0 = 65, cw = std::max(10, tw(a + "0", F11()));
            if (_edStage == 1) {
                rbox(bx0, y - 1, bx0 + cw, y + 10, 2, ui::WHITE);
                cv().drawRoundRect(bx0, y - 1, cw + 1, 12, 2, CURVE);
            }
            int w  = text(bx0 + 2, y, a.empty() && _edStage == 1 ? "_" : a, F11(), _edStage == 1 ? CURVE : INK);
            int sx = bx0 + 2 + std::max(w, cw - 2) + 1;
            sx += text(sx, y, ":", F11(), DIM);
            if (_edStage == 2) {
                int sw = std::max(14, tw(b2 + "0", F11()));
                rbox(sx, y - 1, sx + sw, y + 10, 2, ui::WHITE);
                cv().drawRoundRect(sx, y - 1, sw + 1, 12, 2, CURVE);
                text(sx + 2, y, b2.empty() ? "_" : b2, F11(), CURVE);
            } else {
                text(sx, y, b2, F11(), DIM);
            }
        } else {
            text(67, y, i == 0 ? "0:00" : (r.t >= 0 ? mmss(r.t) : "--"), F11(), r.on ? INK : FAINT);
        }
        // temperature
        std::string cs;
        uint32_t cc = r.on ? INK : FAINT;
        if (i == 7) {
            cs = "";
        } else if (sel && _edStage == 3) {
            int cw = std::max(24, tw(_edBuf + "0", F11()) + 4);
            rbox(138 - cw, y - 1, 139, y + 10, 2, ui::WHITE);
            cv().drawRoundRect(138 - cw, y - 1, cw + 2, 12, 2, CURVE);
            cs = _edBuf.empty() ? "_" : _edBuf;
            cc = CURVE;
        } else if (r.on && !std::isnan(r.c)) {
            cs = fmt1(r.c);
        } else {
            int tt = i == 0 ? 0 : r.t;
            cs     = tt >= 0 ? fmt1(_curve.at(tt)) : "--";
            cc     = r.on ? DIM : FAINT;  // follows the preset curve
        }
        text(137, y, cs, F11(), cc, Datum::top_right);
        y += 12;
    }
    // mini chart with the selected node
    const int cx0 = 145, cy0 = 17, cx1 = W - 4, cy1 = 112;
    rbox(cx0, cy0, cx1, cy1, 4, CARD);
    int end = std::max(60, std::max(_curve.endTime(), _ed[7].t)) + 30;
    auto PX = [&](float t) { return cx0 + 4 + (int)((cx1 - cx0 - 8) * t / end); };
    auto PY = [&](float c) { return cy1 - 4 - (int)((cy1 - cy0 - 8) * (std::max(20.0f, std::min(235.0f, c)) - 20) / 215); };
    for (int s = 60; s < end; s += 60) cv().drawFastVLine(PX(s), cy0 + 3, cy1 - cy0 - 6, GRID);
    int selT = _edRow == 0 ? 0 : _ed[_edRow].t;
    if (_ed[_edRow].on && selT >= 0) cv().drawFastVLine(PX(selT), cy0 + 3, cy1 - cy0 - 6, CURVE_TINT);
    int px = PX(0), py = PY(_curve.at(0));
    for (int s = 5; s <= _curve.endTime(); s += 5) {
        int nx = PX(s), ny = PY(_curve.at(s));
        cv().drawLine(px, py, nx, ny, CURVE);
        px = nx;
        py = ny;
    }
    for (int i = 0; i < 7; i++) {
        if (!_ed[i].on) continue;
        int t = i == 0 ? 0 : _ed[i].t;
        if (t < 0) continue;
        int nx = PX(t), ny = PY(_curve.at(t));
        if (i == _edRow) {
            ring(nx, ny, 5, CURVE);
            dot(nx, ny, 3, cols[i]);
        } else {
            dot(nx, ny, 2, cols[i]);
        }
    }
    if (_edStage == 1)
        keybar({{"0-9", CURVE, "minutes"}, {"Enter", INK, "seconds"}, {"Tab", INK, "temp"}, {"Esc", FAINT, "cancel"}});
    else if (_edStage == 2)
        keybar({{"0-9", CURVE, "seconds"}, {"Enter", INK, _edRow == 7 ? "ok" : "temp"}, {"Esc", FAINT, "cancel"}});
    else if (_edStage == 3)
        keybar({{"0-9", CURVE, "°C"}, {"Enter", GO, "ok"}, {"Esc", FAINT, "cancel"}});
    else
        keybar({{"Enter", CURVE, "edit"}, {"Spc", INK, "on/off"}, {"S", GO, "save"}, {"Tab", CURVE, "chart"}});
}

/* ================================================================= rules */
void AppRoast::renderSettings()
{
    auto rows = ruleRows();
    text(4, 0, "Rules", F16(), INK);
    std::vector<int> sel;
    for (int i = 0; i < (int)rows.size(); i++)
        if (rows[i].label) sel.push_back(i);
    int selRow = sel[std::max(0, std::min((int)sel.size() - 1, _setSel))];
    // first row to draw: scroll so the selected row stays inside y 17..104, with its group header
    int first = 0, need = 0;
    for (int i = selRow; i >= 0; i--) {
        need += rows[i].label ? 13 : 11;
        if (need > 74 + (_setEditing ? -14 : 0)) {
            first = i + 1;
            break;
        }
    }
    if (first > 0 && first < (int)rows.size() && rows[first].label && !rows[first - 1].label) first--;
    auto one = [&](float v, bool isInt) { return isInt ? std::to_string((int)lroundf(v)) : fmt1(v); };
    auto whole = [&](const RuleRow& r) { return r.i[0] != nullptr || r.step >= 1; };
    auto value = [&](const RuleRow& r, bool live) {
        if (r.simToggle) return std::string(_link && _link->isSimulated() ? "on" : "off");
        bool isInt = whole(r);
        float v0   = live ? _setVal[0] : (r.f[0] ? *r.f[0] : (float)*r.i[0]);
        if (r.f[1] || r.i[1]) {
            float v1 = live ? _setVal[1] : (r.f[1] ? *r.f[1] : (float)*r.i[1]);
            return std::string(std::string(r.unit) == "°" ? "+" : "") + one(v0, isInt) + " – " + one(v1, isInt) + r.unit;
        }
        return one(v0, isInt) + r.unit;
    };
    int y = 17;
    for (int i = first; i < (int)rows.size() && y <= 104; i++) {
        auto& r = rows[i];
        if (!r.label) {
            dot(7, y + 5, 3, r.groupColor);
            text(14, y + 1, r.group, F10(), DIM);
            y += 11;
            continue;
        }
        bool isSel = i == selRow;
        bool edit  = isSel && _setEditing;
        if (isSel) {
            rbox(1, y - 1, W - 2, y + (edit ? 25 : 11), 4, CARD);
            if (edit)
                cv().drawRoundRect(1, y - 1, W - 2, 27, 4, INK);
            else
                cv().fillRect(1, y - 1, 2, 13, INK);
        }
        text(14, y, r.label, F12(), INK);
        if (edit) {
            std::string v = _setBuf.empty() ? value(r, true) : _setBuf + "_";
            int vx        = W - 8;
            text(vx, y - 1, "›", F14(), INK, Datum::top_right);
            text(vx - 10, y, v, F12(), INK, Datum::top_right);
            text(vx - 14 - tw(v, F12()), y - 1, "‹", F14(), INK, Datum::top_right);
            bool range = r.f[1] || r.i[1];
            std::string hint = std::string(range ? (_setStage == 0 ? "lower end · " : "upper end · ") : "") + "range " +
                               one(r.lo, whole(r)) + " – " + one(r.hi, whole(r)) + r.unit;
            text(14, y + 13, hint, F10(), DIM);
            y += 14;
        } else {
            text(W - 8, y, value(r, false), F12(), isSel ? INK : DIM, Datum::top_right);
        }
        y += 13;
    }
    if (_setEditing)
        keybar({{"← →", INK, "±"}, {"0-9", INK, "type"}, {"Enter", GO, "ok"}, {"Esc", FAINT, "cancel"}});
    else
        keybar({{"↑ ↓", INK, "select"}, {"Enter", INK, "change"}, {"Esc", FAINT, "back"}});
}

/* ================================================================== help */
void AppRoast::renderHelp()
{
    int x = 4 + text(4, 0, "Keys", F16(), INK);
    text(x + 6, 5, "colour = what it controls", F10(), DIM);
    struct G {
        uint32_t c;
        const char* name;
        const char* keys;
    } groups[] = {
        {FAN, "fan", "S on · ↑↓ ±1 · Aa ±0.1 · X X stop"}, {HEAT, "heat", "P manual/PID · ←→ ±5 · Aa ±1"},
        {DRY, "marks", "Y dry · F 1st crack · G FC end"}, {DROP, "drop", "D D or Space Space · X X stop"},
        {CURVE, "curve", "C library · E edit · Tab chart"},   {INK, "roast", "Enter charge · W batch · O rules"},
        {FAINT, "app", "M mute · H keys · Esc back"},
    };
    int y = 18;
    for (auto& g : groups) {
        dot(8, y + 6, 4, g.c);
        text(16, y, g.name, F12(), INK);
        text(56, y + 2, g.keys, F10(), DIM);
        y += 14;
    }
    keybar({{"Esc", FAINT, "back"}});
}

/* ======================================================== one-field sheet */
void AppRoast::renderInput()
{
    darken(0, 40);
    rbox(0, 40, W - 1, H + 8, 8, CARD);
    rbox(W / 2 - 14, 43, W / 2 + 14, 45, 1, FAINT);
    text(8, 49, _input.title, F16(), INK);
    rbox(8, 70, W - 9, 92, 4, ui::WHITE);
    cv().drawRoundRect(8, 70, W - 16, 23, 4, INK);
    cv().drawRoundRect(9, 71, W - 18, 21, 3, INK);
    bool blink = (GetHAL().millis() / 400) % 2;
    int w      = text(14, 81, clipTo(_input.buf, W - 40, F14()), F14(), INK, Datum::middle_left);
    if (blink) cv().fillRect(15 + w, 75, 1, 12, INK);
    if (!_input.hint.empty()) text(8, 99, _input.hint, F10(), DIM);
    keybar({{"Enter", GO, "ok"}, {"Esc", FAINT, "cancel"}});
}

/* ================================================================= toast */
void AppRoast::renderToast()
{
    if (_toast.empty() || GetHAL().millis() > _toastUntil) return;
    int w = std::min(W - 8, tw(_toast, F11()) + 22);
    int x = (W - w) / 2, y = 101;
    rbox(x, y, x + w, y + 15, 7, INK);
    dot(x + 9, y + 8, 3, _toastColor);
    text(x + 16, y + 8, clipTo(_toast, w - 20, F11()), F11(), ui::WHITE, Datum::middle_left);
}
