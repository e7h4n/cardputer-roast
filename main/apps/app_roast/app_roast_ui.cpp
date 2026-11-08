/*
 * Roast app rendering. Roast owns the whole 240x135 screen; the top-right corner is reserved for the
 * battery gauge (and a SIM tag while the simulator is in use), drawn last by renderStatus().
 */
#include "app_roast.h"
#include <apps/utils/theme.h>
#include <cmath>
#include <climits>
#include <algorithm>

using namespace roast;

namespace {
constexpr int W = 240;
constexpr int H = 135;
constexpr int BAR_Y = H - 14;  // bottom hint bar / roast banner

constexpr uint32_t C_BG     = THEME_COLOR_BG;
constexpr uint32_t C_PANEL  = 0x262626;
constexpr uint32_t C_GRID   = 0x3E3E3E;
constexpr uint32_t C_TEXT   = 0xF2F2F2;
constexpr uint32_t C_DIM    = 0x9A9A9A;
constexpr uint32_t C_ACCENT = 0x99FF00;
constexpr uint32_t C_BT     = 0xFF9A3C;
constexpr uint32_t C_TGT    = 0xB0B0B0;
constexpr uint32_t C_ROR    = 0x4FC3F7;
constexpr uint32_t C_RED    = 0xFF5252;
constexpr uint32_t C_YEL    = 0xFFD54F;
constexpr uint32_t C_GREEN  = 0x66DD77;
constexpr uint32_t C_SEL    = 0x3D4F7A;  // selected row
constexpr uint32_t C_MODAL  = 0x1E1E1E;
constexpr uint32_t C_FIELD  = 0x3A3A3A;
constexpr uint32_t C_TOAST  = 0x111111;
constexpr uint32_t C_CURSOR = 0x707070;

const lgfx::IFont* F10  = &fonts::efontCN_10;
const lgfx::IFont* F12  = &fonts::efontCN_12;
const lgfx::IFont* F16  = &fonts::efontCN_16;
const lgfx::IFont* FB18 = &fonts::FreeSansBold18pt7b;

bool g_sim = false;  // set by render(): the top-right corner also carries a SIM tag

LGFX_Sprite& cv()
{
    return GetHAL().canvas;
}

void text(int x, int y, const std::string& s, uint32_t color, const lgfx::IFont* f = F12,
          textdatum_t datum = textdatum_t::top_left)
{
    cv().setFont(f);
    cv().setTextDatum(datum);
    cv().setTextColor(color);
    cv().drawString(s.c_str(), x, y);
}

int textW(const std::string& s, const lgfx::IFont* f = F12)
{
    return cv().textWidth(s.c_str(), f);
}

// Right edge available to a page's top line (the status corner sits to the right of it).
int topLimit()
{
    return W - (g_sim ? 80 : 50);
}

// Shorten to fit maxPx, dropping whole UTF-8 characters and appending "..".
std::string clipTo(const std::string& s, int maxPx, const lgfx::IFont* f = F12)
{
    if (textW(s, f) <= maxPx) return s;
    std::string r = s;
    while (!r.empty() && textW(r + "..", f) > maxPx) {
        size_t i = r.size() - 1;
        while (i > 0 && ((unsigned char)r[i] & 0xC0) == 0x80) i--;
        r.erase(i);
    }
    return r + "..";
}

void bottomBar(const std::string& s, uint32_t fg = C_DIM, uint32_t bg = C_PANEL)
{
    cv().fillRect(0, BAR_Y, W, H - BAR_Y, bg);
    text(2, BAR_Y + 1, s, fg);
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
}  // namespace

void AppRoast::render()
{
    g_sim = _link && _link->isSimulated();
    cv().fillScreen(C_BG);
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
    if (_input.active) renderInput();
    renderToast();
    GetHAL().pushCanvas();
}

/* ----------------------------- status corner ----------------------------- */
void AppRoast::renderStatus()
{
    // battery: 15x8 body + 2x4 tip at the top-right, percentage to its left
    uint32_t c = _batPct < 0 ? C_DIM : (_batPct <= 15 ? C_RED : (_batPct <= 30 ? C_YEL : C_TEXT));
    int bx = W - 19, by = 3;
    cv().fillRect(bx - 34, 0, W - (bx - 34), 13, C_BG);
    cv().drawRect(bx, by, 15, 8, c);
    cv().fillRect(bx + 15, by + 2, 2, 4, c);
    if (_batPct > 0) cv().fillRect(bx + 2, by + 2, std::max(1, 11 * std::min(100, _batPct) / 100), 4, c);
    std::string pct = _batPct < 0 ? "--" : std::to_string(_batPct) + "%";
    text(bx - 2, 1, pct, c, F12, textdatum_t::top_right);
    if (g_sim) {
        int x = bx - 4 - textW(pct) - 28;
        cv().fillRoundRect(x, 0, 26, 13, 3, C_YEL);
        text(x + 13, 1, "SIM", 0x000000, F12, textdatum_t::top_center);
    }
}

/* -------------------------------- connect -------------------------------- */
void AppRoast::renderConnect()
{
    text(2, 0, "Connect roaster", C_ACCENT, F16);
    if (_blockedByKeyboardApp) {
        text(2, 26, "BLE is used by another app", C_RED);
        text(2, 41, "Restart, then open Roast first", C_TEXT);
        bottomBar("D simulator   Esc exit");
        return;
    }
    std::string st;
    switch (_linkState) {
        case LinkState::Scanning:
            st = "Scanning E-Meter...BLE";
            break;
        case LinkState::Connecting:
            st = "Connecting " + (_link ? _link->deviceName() : std::string());
            break;
        case LinkState::Ready:
            st = "Connected";
            break;
        case LinkState::Error:
            st = "BLE error";
            break;
        default:
            st = "Idle";
    }
    int dots = (GetHAL().millis() / 400) % 4;
    text(2, 26, st + std::string(dots, '.'), C_TEXT);
    if (_link) {
        std::string err = _link->lastError();
        if (!err.empty()) text(2, 42, clipTo(err, W - 4), C_YEL);
        int y = 60;
        for (auto& s : _link->scanResults()) {
            text(2, y, s.name + "  " + std::to_string(s.rssi) + "dBm", C_DIM);
            y += 14;
            if (y > 102) break;
        }
    }
    bottomBar("Phone app off | D sim | Esc exit");
}

/* ---------------------------------- prep --------------------------------- */
void AppRoast::renderPrep()
{
    if (_view == 1 && _hasCurve) {
        renderChart(0, 0, W, BAR_Y - 1, true);
        bottomBar("Curve preview | Tab back");
        return;
    }
    // curve
    if (_hasCurve) {
        text(2, 0, clipTo(_curve.name, topLimit() - 2), C_ACCENT);
        std::string sum;
        int fcT = _curve.tagTime(PointTag::FCs);
        if (fcT >= 0) sum += "FC " + FormatMmSs(fcT) + "/" + fmt0(_curve.tagTemp(PointTag::FCs)) + "  ";
        sum += "End " + FormatMmSs(_curve.endTime()) + "/" + fmt0(_curve.at(_curve.endTime()));
        text(2, 15, sum, C_DIM);
    } else {
        text(2, 0, "No curve (manual heat)", C_YEL);
        text(2, 15, "C pick curve  E new", C_DIM);
    }
    // big BT
    std::string bt = _snap.has(4) ? fmt1(_snap.bt()) : "--";
    text(2, 34, bt, _snap.has(4) && _snap.bt() >= _rules.coldBT ? C_YEL : C_BT, FB18);
    int bx = 5 + textW(bt, FB18);
    text(bx, 36, "°C", C_DIM);
    text(bx, 50, "BT", C_DIM);
    // right column: machine fan while it runs, otherwise the preset that S will use
    bool on   = _phase == Phase::FanOn;
    float fan = (_snap.has(12) && on) ? _snap.fan() : _rules.startFan;
    text(W - 2, 36, "Fan " + fmt1(fan) + "%", C_ROR, F12, textdatum_t::top_right);
    text(W - 2, 50, on ? "Fan on" : "Idle (preset)", on ? C_ACCENT : C_DIM, F12, textdatum_t::top_right);
    // batch
    std::string batch = "Green " + fmt0(_rules.greenG) + "g";
    if (!_bean.empty()) batch += "  " + _bean;
    text(2, 72, clipTo(batch, W - 4), C_TEXT);
    // link line
    cv().fillCircle(6, 96, 3, linkReady() ? C_GREEN : C_RED);
    std::string ln = _link ? _link->deviceName() : "";
    if (_link && _link->isSimulated()) ln += " (R = real)";
    if (!linkReady()) ln += " reconnecting";
    ln += _sdOk ? "   SD ok" : "   no SD";
    text(13, 90, ln, C_DIM);
    // hints
    if (on)
        bottomBar("↑↓ fan   Enter charge   XX stop", C_TEXT);
    else
        bottomBar("S fan C curve W g B bean O set H help");
}

/* --------------------------------- roast --------------------------------- */
void AppRoast::renderRoast()
{
    if (_view == 1) {
        renderChart(0, 0, W, BAR_Y - 1, true);
    } else {
        renderDash();
    }
    renderBanner();
}

void AppRoast::renderDash()
{
    int t    = roastT();
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    // mode + curve
    text(0, 0, _manual ? "MAN" : "PID", _manual ? C_BT : C_ACCENT);
    text(26, 0, clipTo(_hasCurve ? _curve.name : "No curve", topLimit() - 28), C_DIM);
    // time + BT
    text(0, 15, FormatMmSs(t), C_TEXT, FB18);
    text(W - 1, 15, fmt1(bt), linkReady() ? C_BT : C_RED, FB18, textdatum_t::top_right);
    // target / ror
    float tgt = curveTargetAt(curveT());
    std::string l1 = _hasCurve ? "Tgt " + fmt1(tgt) + "  " + signed1(bt - tgt) : "Heat manual";
    text(0, 45, l1, C_TGT);
    text(W - 1, 45, "RoR " + fmt1(_session.rorNow()), C_ROR, F12, textdatum_t::top_right);
    // heater / fan
    text(0, 59, "Heat " + fmt0(_snap.heat()) + "%   Fan " + fmt1(_snap.fan()) + "%", C_TEXT);
    // marks
    int dry = _session.markT(Mark::Dry), fc = _session.markT(Mark::FCs), fce = _session.markT(Mark::FCe);
    std::string m1 = dry >= 0 ? "Dry " + FormatMmSs(dry) + "/" + fmt0(_session.markBT(Mark::Dry)) : "Y = dry";
    std::string m2 = fc >= 0 ? "FC " + FormatMmSs(fc) + "/" + fmt1(_session.markBT(Mark::FCs)) : "F = 1st crack";
    text(0, 73, m1, dry >= 0 ? C_GREEN : C_DIM);
    text(W - 1, 73, m2, fc >= 0 ? C_GREEN : C_DIM, F12, textdatum_t::top_right);
    if (fc >= 0) {
        std::string m3 = fce >= 0 ? "FC end " + FormatMmSs(fce) + "/" + fmt1(_session.markBT(Mark::FCe)) : "G = FC end";
        text(W - 1, 87, m3, fce >= 0 ? C_GREEN : C_DIM, F12, textdatum_t::top_right);
        // development
        int dev  = t - fc;
        float dT = bt - _session.markBT(Mark::FCs);
        char b[64];
        snprintf(b, sizeof(b), "Dev %s  %+.1f°C  %.0f%%", FormatMmSs(dev).c_str(), dT, t > 0 ? 100.0 * dev / t : 0.0);
        text(0, 101, b, C_TEXT);
        // window bar: 0 .. devMax+15 s
        int x0 = 0, x1 = W - 1, y = 116;
        float span = _rules.devMax + 15.0f;
        auto X = [&](float s) { return x0 + (int)((x1 - x0) * std::min(1.0f, s / span)); };
        cv().fillRect(x0, y, x1 - x0, 2, C_GRID);
        cv().fillRect(X(_rules.devMin), y, X(_rules.devMax) - X(_rules.devMin), 2, C_GREEN);
        cv().fillRect(X(_rules.devMax), y, x1 - X(_rules.devMax), 2, C_RED);
        cv().fillRect(X(dev) - 1, y - 2, 3, 5, C_TEXT);
    } else {
        int fcT = _hasCurve ? _curve.tagTime(PointTag::FCs) : -1;
        if (fcT > 0) {
            text(0, 94, "FC expected " + FormatMmSs(fcT) + "  (in " + FormatMmSs(std::max(0, fcT - t)) + ")", C_DIM);
        } else if (dry >= 0) {
            text(0, 94, "Press F at first crack", C_DIM);
        }
    }
}

void AppRoast::renderChart(int x, int y, int w, int h, bool withText)
{
    cv().fillRect(x, y, w, h, C_PANEL);
    int lastT = _session.active() ? _session.lastT() : 0;
    int tMax  = std::max({_hasCurve ? _curve.endTime() + 30 : 0, lastT + 30, 360});
    tMax      = ((tMax + 59) / 60) * 60;
    const float yMin = 20, yMax = 235;
    auto PX = [&](float t) { return x + (int)((w - 1) * t / tMax); };
    auto PY = [&](float c) {
        float v = std::max(yMin, std::min(yMax, c));
        return y + h - 1 - (int)((h - 1) * (v - yMin) / (yMax - yMin));
    };
    for (int s = 60; s < tMax; s += 60) cv().drawFastVLine(PX(s), y, h, C_GRID);
    for (int c = 50; c < yMax; c += 50) cv().drawFastHLine(x, PY(c), w, C_GRID);
    // target curve
    if (_hasCurve) {
        int step = std::max(1, tMax / w);
        int px = PX(0), py = PY(_curve.at(0));
        for (int s = step; s <= _curve.endTime(); s += step) {
            int nx = PX(s), ny = PY(_curve.at(s));
            cv().drawLine(px, py, nx, ny, C_TGT);
            px = nx;
            py = ny;
        }
        for (auto& p : _curve.pts)
            if (p.tag == PointTag::FCs || p.tag == PointTag::Dry) cv().fillCircle(PX(p.t), PY(_curve.at(p.t)), 2, C_TGT);
    }
    // live data
    if (_session.active() || _phase == Phase::Done) {
        auto& ss = _session.samples();
        int ppx = -1, ppy = -1, rpx = -1, rpy = -1;
        for (size_t i = 0; i < ss.size(); i++) {
            int nx = PX(ss[i].t);
            int ny = PY(ss[i].bt10 / 10.0f);
            if (ppx >= 0) cv().drawLine(ppx, ppy, nx, ny, C_BT);
            ppx = nx;
            ppy = ny;
            if (ss[i].ror10 != INT16_MIN) {
                int ry = PY(yMin + ss[i].ror10 / 10.0f * 6.0f);  // RoR 0..30 -> lower part
                if (rpx >= 0) cv().drawLine(rpx, rpy, nx, ry, C_ROR);
                rpx = nx;
                rpy = ry;
            }
        }
        // mark lines, labelled along the bottom edge so they never cover the readout at the top
        const char* tags[] = {"Y", "F", "G", "D"};
        int lastLabelX = -100;
        for (int m = 0; m < (int)Mark::Count; m++) {
            int mt = _session.markT((Mark)m);
            if (mt < 0) continue;
            int mx = PX(mt);
            cv().drawFastVLine(mx, y, h, C_GREEN);
            int lx = std::max(mx + 2, lastLabelX + 8);
            text(lx, y + h - 13, tags[m], C_GREEN);
            lastLabelX = lx;
        }
        if (_phase == Phase::Roasting) cv().drawFastVLine(PX(roastT()), y, h, C_CURSOR);
    }
    if (withText) {
        if (_session.active() && _phase != Phase::Idle) {
            std::string a = fmt1(_snap.has(4) ? _snap.bt() : _session.lastBT()) + "°  " + FormatMmSs(roastT());
            text(x + 2, y + 1, a, C_BT);
            text(x + 2 + textW(a) + 12, y + 1, "RoR " + fmt1(_session.rorNow()), C_ROR);
        } else if (_hasCurve) {
            text(x + 2, y + 1, clipTo(_curve.name, topLimit() - 4), C_TGT);
        }
    }
}

bool AppRoast::bannerText(std::string& s, uint32_t& bg, uint32_t& fg)
{
    char b[96];
    fg       = 0x000000;
    int t    = roastT();
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    if (_phase == Phase::Roasting && !linkReady()) {
        s  = "BLE lost! Roaster holds target";
        bg = C_RED;
        fg = 0xFFFFFF;
        return true;
    }
    int fc = _session.markT(Mark::FCs);
    if (fc >= 0 && _phase == Phase::Roasting) {
        int dev  = t - fc;
        float dT = bt - _session.markBT(Mark::FCs);
        if (bt >= _rules.maxDropBT) {
            snprintf(b, sizeof(b), "%.0f°C reached: DROP NOW (D D)", _rules.maxDropBT);
            s  = b;
            bg = C_RED;
            fg = 0xFFFFFF;
            return true;
        }
        if (dev >= _rules.devMax) {
            snprintf(b, sizeof(b), "Dev %ds >= %ds: DROP (D D)", dev, _rules.devMax);
            s  = b;
            bg = C_RED;
            fg = 0xFFFFFF;
            return true;
        }
    }
    if (_heatDropWarned && fc >= 0 && t - fc <= 75 && !_manual) {
        s  = "Heat falling after FC: P=manual";
        bg = C_YEL;
        return true;
    }
    if (_satWarned && _satRun > 0) {
        snprintf(b, sizeof(b), "Heat maxed %.0f%% for %ds", _snap.heat(), _satRun);
        s  = b;
        bg = C_YEL;
        return true;
    }
    if (fc >= 0 && _phase == Phase::Roasting) {
        int dev  = t - fc;
        float dT = bt - _session.markBT(Mark::FCs);
        if (dev >= _rules.devMin && dT >= _rules.dtMin) {
            snprintf(b, sizeof(b), "Drop window %ds %+.1f°C (D D)", dev, dT);
            s  = b;
            bg = C_GREEN;
            return true;
        }
        if (dT >= _rules.dtMax && dev < _rules.devMin) {
            snprintf(b, sizeof(b), "Rise %+.1f°C: drop at %ds", dT, _rules.devMin);
            s  = b;
            bg = C_YEL;
            return true;
        }
        if (_rorWarned) {
            snprintf(b, sizeof(b), "RoR %.1f low (<%.0f)", _session.rorNow(), _rules.rorLow);
            s  = b;
            bg = C_YEL;
            return true;
        }
        snprintf(b, sizeof(b), "Window %d-%ds +%.0f-%.0f°C <=%.0f", _rules.devMin, _rules.devMax, _rules.dtMin,
                 _rules.dtMax, _rules.maxDropBT);
        s  = b;
        bg = C_PANEL;
        fg = C_DIM;
        return true;
    }
    s  = _manual ? "←→heat ↑↓fan Y/F mark DD drop" : "↑↓fan Y dry F FC DD drop P man";
    bg = C_PANEL;
    fg = C_DIM;
    return true;
}



void AppRoast::renderBanner()
{
    std::string s;
    uint32_t bg, fg;
    if (!bannerText(s, bg, fg)) return;
    bottomBar(s, fg, bg);
}

/* ---------------------------------- cool --------------------------------- */
void AppRoast::renderCool()
{
    if (_view == 1) {
        renderChart(0, 0, W, BAR_Y - 1, true);
        bottomBar("XX stop   Enter x2 end log   Tab");
        return;
    }
    int since = _dropMs ? (int)((GetHAL().millis() - _dropMs) / 1000) : 0;
    std::string title = _autoDropped ? "Auto-dropped" : "Cooling";
    text(2, 0, title, _autoDropped ? C_RED : C_ROR, F16);
    text(2 + textW(title, F16) + 8, 3, "+" + FormatMmSs(since), C_TEXT);
    if (_autoDropped) text(2, 18, clipTo(_autoDropWhy, W - 4), C_RED);
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    text(2, 34, fmt1(bt), bt < _rules.coldBT ? C_GREEN : C_BT, FB18);
    RoastMetrics m = _session.metrics();
    char b[80];
    snprintf(b, sizeof(b), "Drop %s/%s", FormatMmSs(m.markT[3]).c_str(), fmt1(m.markBT[3]).c_str());
    text(W - 2, 36, b, C_TEXT, F12, textdatum_t::top_right);
    if (m.dev >= 0) {
        snprintf(b, sizeof(b), "Dev %s %+.1f°C", FormatMmSs(m.dev).c_str(), m.deltaFC);
        text(W - 2, 50, b, C_TEXT, F12, textdatum_t::top_right);
    }
    text(2, 70,
         bt < _rules.coldBT ? "Below " + fmt0(_rules.coldBT) + "°C: OK to stop"
                            : "Stop once below " + fmt0(_rules.coldBT) + "°C",
         bt < _rules.coldBT ? C_GREEN : C_DIM);
    text(2, 85, "Fan " + fmt1(_snap.fan()) + "%", C_ROR);
    if (_session.roastedWeight() > 0)
        text(2, 100, "Roasted " + fmt1(_session.roastedWeight()) + "g", C_TEXT);
    else
        text(2, 100, "W roasted weight", C_DIM);
    bottomBar("XX stop | Enter x2 end log only");
}

/* -------------------------------- summary -------------------------------- */
void AppRoast::renderSummary()
{
    if (_view == 1) {
        renderChart(0, 0, W, BAR_Y - 1, false);
        bottomBar("Tab back to summary");
        return;
    }
    RoastMetrics m = _session.metrics();
    char b[96];
    text(2, 0, "Roast summary", C_ACCENT);
    text(2 + textW("Roast summary") + 8, 0, _session.logId(), C_DIM);
    std::string loss = std::isnan(m.lossPct) ? "Loss -- (W roasted g)"
                                             : "Loss " + fmt1(m.lossPct) + "% (" + fmt0(_session.greenWeight()) + "→" +
                                                   fmt0(_session.roastedWeight()) + "g)";
    text(2, 15, "Total " + FormatMmSs(m.total) + "   " + loss, C_TEXT);
    snprintf(b, sizeof(b), "Dry %s/%s   FC %s/%s", m.markT[0] >= 0 ? FormatMmSs(m.markT[0]).c_str() : "--",
             fmt1(m.markBT[0]).c_str(), m.markT[1] >= 0 ? FormatMmSs(m.markT[1]).c_str() : "--",
             fmt1(m.markBT[1]).c_str());
    text(2, 29, b, C_TEXT);
    snprintf(b, sizeof(b), "Drop %s/%s   Dev %s %s%%", m.markT[3] >= 0 ? FormatMmSs(m.markT[3]).c_str() : "--",
             fmt1(m.markBT[3]).c_str(), m.dev >= 0 ? FormatMmSs(m.dev).c_str() : "--", fmt1(m.devPct).c_str());
    text(2, 43, b, C_TEXT);
    snprintf(b, sizeof(b), "FC->drop %s°C   RoR %s->%s", signed1(m.deltaFC).c_str(), fmt1(m.rorAtFC).c_str(),
             fmt1(m.rorFC30).c_str());
    text(2, 57, b, C_TEXT);
    snprintf(b, sizeof(b), "Max heat %s%%   at max %ds   err %s", fmt0(m.maxHeat).c_str(), m.heatSatSec,
             fmt1(m.meanAbsErr).c_str());
    text(2, 71, b, C_DIM);
    text(2, 97, clipTo(_saveMsg, W - 4),
         (_saveMsg.rfind("Saved", 0) == 0 || _saveMsg.rfind("Updated", 0) == 0) ? C_GREEN : C_RED);
    bottomBar("W g   N note   Tab chart   Enter done");
}

/* -------------------------------- curves --------------------------------- */
void AppRoast::renderCurves()
{
    text(2, 0, "Curves (" + std::to_string(_curves.size()) + ")", C_ACCENT);
    int y = 16;
    for (int i = _listTop; i < (int)_curves.size() && i < _listTop + 4; i++) {
        auto& c  = _curves[i];
        bool sel = i == _listSel;
        if (sel) cv().fillRoundRect(0, y - 1, W, 25, 3, C_SEL);
        std::string name = c.name + (c.builtin ? " (sample)" : (c.readOnly ? " (alog)" : ""));
        text(3, y, clipTo(name, W - 6), sel ? C_ACCENT : C_TEXT);
        std::string sum = "End " + FormatMmSs(c.endTime()) + " " + fmt0(c.at(c.endTime())) + "°C";
        int fcT         = c.tagTime(PointTag::FCs);
        if (fcT >= 0) sum += "   FC " + FormatMmSs(fcT);
        if (c.batchG > 0) sum += "   " + std::to_string(c.batchG) + "g";
        text(3, y + 13, sum, C_DIM, F10);
        y += 26;
    }
    if (_curves.empty()) text(2, 30, "No curves", C_DIM);
    bottomBar("↑↓  Enter use  E edit  N none  Esc back");
}

/* -------------------------------- editor --------------------------------- */
void AppRoast::renderEditor()
{
    if (_view == 1) {
        renderChart(0, 0, W, BAR_Y - 1, true);
        bottomBar("Preview | Tab back to edit");
        return;
    }
    static const char* names[8] = {"Start", "Dry", "FC start", "FC end", "SC start", "SC end", "Drop", "Total"};
    std::string batch = _curve.batchG > 0 ? "  " + std::to_string(_curve.batchG) + "g" : "";
    std::string title = clipTo(_curve.name + (_edDirty ? " *" : ""), topLimit() - 4 - textW(batch));
    text(2, 0, title, C_ACCENT);
    text(2 + textW(title), 0, batch, C_DIM);
    const int rowH = 13;
    int y          = 15;
    for (int i = 0; i < kEdRows; i++) {  // all 8 rows fit
        const EdRow& r = _ed[i];
        bool sel       = i == _edRow;
        if (sel) cv().fillRect(0, y - 1, W, rowH, C_SEL);
        uint32_t base = r.on ? C_TEXT : C_DIM;
        text(3, y, names[i], base);
        // enabled
        std::string onTxt = (i >= 6) ? "fix" : (r.on ? "on" : "off");
        text(70, y, onTxt, sel && _edCol == 0 ? C_YEL : (r.on ? C_GREEN : C_DIM));
        // time (min:sec)
        std::string ts;
        if (i == 0)
            ts = "0:00";
        else if (r.t >= 0)
            ts = FormatMmSs(r.t);
        else
            ts = "--";
        if (sel && (_edStage == 1 || _edStage == 2)) {
            if (_edStage == 1)
                ts = _edBuf + "_:" + (r.t >= 0 ? (r.t % 60 < 10 ? "0" : "") + std::to_string(r.t % 60) : "00");
            else
                ts = _edMin + ":" + _edBuf + "_";
        }
        text(112, y, ts, sel && _edCol == 1 ? C_YEL : base);
        // temperature
        std::string cs;
        uint32_t cc = base;
        if (i == 7) {
            cs = "";
        } else if (sel && _edStage == 3) {
            cs = _edBuf + "_";
        } else if (r.on && !std::isnan(r.c)) {
            cs = fmt1(r.c) + "°";
        } else {
            int tt = i == 0 ? 0 : r.t;
            cs     = tt >= 0 ? fmt1(_curve.at(tt)) + "°" : "--";
            cc     = C_DIM;  // follows the preset curve
        }
        text(172, y, cs, sel && _edCol == 2 ? C_YEL : cc);
        y += rowH;
    }
    const char* hint = "↑↓ row  ←→ col  Enter edit  S save";
    if (_edStage == 1) hint = "Type minutes, Enter -> seconds";
    if (_edStage == 2) hint = "Type seconds, Enter = OK";
    if (_edStage == 3) hint = "Type temperature, Enter = OK";
    if (_edStage == 0 && _edCol == 0) hint = "Enter/Space on/off   S save";
    if (_edStage == 0 && _edCol == 2) hint = "Digits = temp   Del = preset";
    bottomBar(hint);
}

/* ------------------------------- settings -------------------------------- */
void AppRoast::renderSettings()
{
    struct Row {
        const char* name;
        std::string val;
    };
    char b[16];
    auto f1 = [&](float v) {
        snprintf(b, sizeof(b), "%.1f", v);
        return std::string(b);
    };
    Row rows[] = {
        {"Dev min (s)", std::to_string(_rules.devMin)},       {"Dev max (s)", std::to_string(_rules.devMax)},
        {"FC rise min °C", f1(_rules.dtMin)},               {"FC rise max °C", f1(_rules.dtMax)},
        {"Drop-now temp", f1(_rules.maxDropBT)},             {"Safety drop temp", f1(_rules.safetyBT)},
        {"Auto-drop after end s", std::to_string(_rules.afterEnd)}, {"RoR low alert", f1(_rules.rorLow)},
        {"Cold temp", f1(_rules.coldBT)},                    {"Start fan %", f1(_rules.startFan)},
        {"FC heat-drop alert %", f1(_rules.heatDrop)},
        {"Curve offset °C", f1(_rules.curveOffset)},
    };
    const int n = sizeof(rows) / sizeof(rows[0]), visible = 7;
    int top     = std::max(0, std::min(_setSel - 3, n - visible));
    text(2, 0, "Drop rules & alerts", C_ACCENT);
    int y = 16;
    for (int i = top; i < n && i < top + visible; i++) {
        bool sel = i == _setSel;
        if (sel) cv().fillRect(0, y - 1, W, 14, C_SEL);
        text(3, y, rows[i].name, sel ? C_ACCENT : C_TEXT);
        text(W - 3, y, rows[i].val, sel ? C_YEL : C_DIM, F12, textdatum_t::top_right);
        y += 15;
    }
    bottomBar("↑↓ select   Enter edit   Esc back");
}

/* ---------------------------------- help --------------------------------- */
void AppRoast::renderHelp()
{
    static const char* prep[] = {"Prep", "S fan on (no heat)   XX fan off", "↑↓ fan ±1    Aa+↑↓ ±0.1",
                                 "Enter charge (timer + PID)", "C curves   E edit   W green g",
                                 "B bean   O rules   Tab chart", "M mute   Esc back to Launcher"};
    static const char* roast[] = {"Roasting", "Y dry   F first crack   G FC end", "D D / Space x2 = drop",
                                  "↑↓ fan   P manual heat   ←→ ±5", "Press Y/F twice to clear a mark",
                                  "Tab chart   M mute", "Exit is locked until drop"};
    static const char* cool[] = {"Cooling / summary", "XX stop   Enter x2 end log only", "W roasted g   N note",
                                 "Logs: SD roast/logs/", ".csv 1 Hz data   .json summary", ".alog opens in Artisan",
                                 "Enter: done, back to prep"};
    const char** lines = prep;
    if (_backScreen == Screen::Roast) lines = roast;
    if (_backScreen == Screen::Cool || _backScreen == Screen::Summary) lines = cool;
    text(2, 0, std::string("Keys: ") + lines[0], C_ACCENT);
    for (int i = 1; i < 7; i++) text(2, 3 + i * 15, lines[i], C_TEXT);
    bottomBar("Esc / Enter / H back");
}

/* --------------------------------- input --------------------------------- */
void AppRoast::renderInput()
{
    int w = W - 24, h = 66, x = (W - w) / 2, y = (H - h) / 2;
    cv().fillRoundRect(x, y, w, h, 6, C_MODAL);
    cv().drawRoundRect(x, y, w, h, 6, C_ACCENT);
    text(x + 6, y + 4, _input.title, C_ACCENT);
    cv().fillRect(x + 6, y + 22, w - 12, 18, C_FIELD);
    bool blink = (GetHAL().millis() / 400) % 2;
    text(x + 9, y + 25, _input.buf + (blink ? "_" : " "), C_TEXT);
    text(x + 6, y + 46, _input.hint.empty() ? "Enter OK   Esc cancel" : _input.hint, C_DIM);
}

void AppRoast::renderToast()
{
    if (_toast.empty() || GetHAL().millis() > _toastUntil) return;
    int tw = std::min(W - 4, textW(_toast) + 12);
    int x  = (W - tw) / 2;
    cv().fillRoundRect(x, BAR_Y - 18, tw, 16, 4, C_TOAST);
    cv().drawRoundRect(x, BAR_Y - 18, tw, 16, 4, _toastColor);
    text(W / 2, BAR_Y - 16, _toast, _toastColor, F12, textdatum_t::top_center);
}
