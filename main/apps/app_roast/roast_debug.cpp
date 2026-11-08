/*
 * USB-serial remote for testing the Roast app on a real Cardputer without touching it.
 * Active only while the app is open. Line commands on the USB Serial/JTAG port:
 *   key <name>     inject a key press: a..z 0..9 enter esc tab space bksp up down left right; prefix S- for shift
 *   type <text>    inject printable characters
 *   shot           dump the 240x135 screen canvas: "SHOT w h" + one hex line per row (RGB565, as stored) + "ENDSHOT"
 *   hold <key> <ms>, quiet, ping, regs
 *   ls <dir>, cat <file>, rm <file under /sdcard/roast/>, sdinfo, sdformat FORMAT-SD-CARD
 */
#include "roast_debug.h"
#include <hal/hal.h>
#include <apps/utils/audio/audio.h>
#include "roast_curve.h"
#include "roast_link.h"
#include <dirent.h>
#include <sys/stat.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <cstdio>
#include <esp_heap_caps.h>
#include <cstring>
#include <string>
#include <deque>
#include <vector>

namespace roast {

bool g_debugQuiet = false;
RoasterLink* g_activeLink = nullptr;
bool g_debugShift = false;  // kept for API compatibility; raw injection drives shift
static TaskHandle_t s_task       = nullptr;
static SemaphoreHandle_t s_mtx   = nullptr;
static std::deque<std::string> s_lines;
static volatile bool s_run       = false;
static bool s_installed          = false;

static void reader(void*)
{
    std::string line;
    uint8_t buf[64];
    while (s_run) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (!line.empty()) {
                    xSemaphoreTake(s_mtx, portMAX_DELAY);
                    if (s_lines.size() < 32) s_lines.push_back(line);
                    xSemaphoreGive(s_mtx);
                    line.clear();
                }
            } else if (line.size() < 120) {
                line += c;
            }
        }
    }
    s_task = nullptr;
    vTaskDelete(nullptr);
}

void DebugStart()
{
    if (s_run) return;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        cfg.rx_buffer_size                  = 512;
        cfg.tx_buffer_size                  = 1024;
        if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return;
        usb_serial_jtag_vfs_use_driver();
        s_installed = true;
    }
    s_run = true;
    xTaskCreate(reader, "roast_dbg", 3072, nullptr, 3, &s_task);
}

static bool DebugPoll(std::string& line)
{
    if (!s_mtx) return false;
    bool ok = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_lines.empty()) {
        line = s_lines.front();
        s_lines.pop_front();
        ok = true;
    }
    xSemaphoreGive(s_mtx);
    return ok;
}

static void DebugShot()
{
    auto& cv = GetHAL().canvas;
    int w = cv.width(), h = cv.height();
    const uint8_t* buf = (const uint8_t*)cv.getBuffer();
    if (!buf) return;
    printf("SHOT %d %d\n", w, h);
    static const char* hx = "0123456789abcdef";
    std::string row;
    row.reserve(w * 4 + 2);
    for (int y = 0; y < h; y++) {
        row.clear();
        const uint8_t* p = buf + y * w * 2;
        for (int i = 0; i < w * 2; i++) {
            row += hx[p[i] >> 4];
            row += hx[p[i] & 15];
        }
        row += '\n';
        fwrite(row.data(), 1, row.size(), stdout);
    }
    printf("ENDSHOT\n");
    fflush(stdout);
}


struct Pending {
    uint8_t row, col;
};
static std::deque<Pending> s_release;
static uint32_t s_holdUntil = 0;  // released once the hold time has passed


static bool resolve(const std::string& n, uint8_t& r, uint8_t& c)
{
    static const struct {
        const char* alias;
        const char* name;
    } kAlias[] = {{"up", ";"}, {"down", "."}, {"left", ","}, {"right", "/"}, {"esc", "`"},
                  {"bksp", "del"}, {"space", " "}, {"enter", "enter"}, {"tab", "tab"}};
    for (auto& a : kAlias)
        if (n == a.alias) return Keyboard::findKey(a.name, r, c);
    return Keyboard::findKey(n.c_str(), r, c);
}

void DebugPollMain()
{
    // keyboard.update() clears the raw buffer every pass and the launcher polls it at most once per millisecond.
    // Inject right after a millisecond boundary so the very next poll sees the event exactly once.
    auto align_ms = []() {
        uint32_t t0 = GetHAL().millis();
        while (GetHAL().millis() == t0) {
        }
    };
    if (s_holdUntil) {  // "hold": keep the key down until the deadline, then release it
        if (GetHAL().millis() < s_holdUntil) return;
        s_holdUntil = 0;
    }
    if (!s_release.empty()) {
        align_ms();
        while (!s_release.empty()) {
            auto p = s_release.front();
            s_release.pop_front();
            GetHAL().keyboard.injectRaw(p.row, p.col, false);
        }
        return;
    }
    std::string line;
    if (!DebugPoll(line)) return;  // one command per pass so each press is seen by the UI
    if (line.rfind("hold ", 0) == 0) {  // hold <key> <ms>
        char name[16] = {0};
        int ms        = 0;
        uint8_t r, c;
        if (sscanf(line.c_str() + 5, "%15s %d", name, &ms) == 2 && resolve(name, r, c)) {
            align_ms();
            GetHAL().keyboard.injectRaw(r, c, true);
            s_release.push_back({r, c});
            s_holdUntil = GetHAL().millis() + ms;
            printf("OK hold\n");
        } else {
            printf("ERR hold\n");
        }
        return;
    }
    if (line == "shot") {
        DebugShot();
    } else if (line.rfind("key ", 0) == 0 || line.rfind("type ", 0) == 0) {
        bool typing = line[0] == 't';
        std::vector<std::string> keys;
        if (typing) {
            for (char ch : line.substr(5)) keys.push_back(std::string(1, ch));
        } else {
            keys.push_back(line.substr(4));
        }
        bool ok = true;
        uint8_t sr = 0, sc = 0;
        Keyboard::findKey("shift", sr, sc);
        for (auto& k : keys) {
            bool shift = false;
            std::string n = k;
            if (n.rfind("S-", 0) == 0) {
                shift = true;
                n     = n.substr(2);
            }
            uint8_t r, c;
            if (!resolve(n, r, c)) {
                ok = false;
                continue;
            }
            if (keys.size() == 1) align_ms();
            if (shift) GetHAL().keyboard.injectRaw(sr, sc, true);
            GetHAL().keyboard.injectRaw(r, c, true);
            if (keys.size() > 1) {  // typing: complete each stroke immediately
                GetHAL().keyboard.injectRaw(r, c, false);
                if (shift) GetHAL().keyboard.injectRaw(sr, sc, false);
            } else {
                s_release.push_back({r, c});
                if (shift) s_release.push_back({sr, sc});
            }
        }
        printf(ok ? "OK key\n" : "ERR key\n");
    } else if (line == "regs") {  // read-only dump of the active link's register cache
        if (!g_activeLink) {
            printf("REGS none\n");
        } else {
            static RegSnapshot r;  // keep big locals off the 8 KB main-task stack
            r = g_activeLink->snapshot();
            printf("REGS state=%d frames=%lu age=%lums valid=%08lx\n", (int)g_activeLink->state(), (unsigned long)r.frames,
                   (unsigned long)(GetHAL().millis() - r.lastRxMs), (unsigned long)r.valid);
            for (int i = 0; i < kRegCount; i++) printf("r%d=%d%s", i, r.reg[i], (i % 10 == 9) ? "\n" : " ");
            printf("ENDREGS\n");
        }
    } else if (line.rfind("ls ", 0) == 0) {
        DIR* d = opendir(line.substr(3).c_str());
        printf("LS %s\n", line.substr(3).c_str());
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                static struct stat st;
                std::string p = line.substr(3) + "/" + e->d_name;
                long sz = stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1;
                printf("  %s %ld\n", e->d_name, sz);
            }
            closedir(d);
        }
        printf("ENDLS\n");
    } else if (line.rfind("cat ", 0) == 0) {  // print a (text) file from the SD card
        FILE* f = fopen(line.substr(4).c_str(), "r");
        printf("CATBEGIN\n");
        if (f) {
            static char buf[256];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
            fclose(f);
        }
        printf("\nCATEND\n");
        fflush(stdout);
    } else if (line.rfind("rm ", 0) == 0) {  // delete one file, only inside /sdcard/roast/
        std::string p = line.substr(3);
        if (p.rfind("/sdcard/roast/", 0) != 0 || p.find("..") != std::string::npos) {
            printf("ERR rm: only files under /sdcard/roast/\n");
        } else if (remove(p.c_str()) == 0) {
            printf("OK rm %s\n", p.c_str());
        } else {
            printf("ERR rm %s\n", p.c_str());
        }
        fflush(stdout);
    } else if (line == "sdinfo") {
        printf("SDINFO\n%sENDSDINFO\n", GetHAL().sdCardInspect().c_str());
    } else if (line == "sdformat FORMAT-SD-CARD") {
        std::string msg;
        uint32_t t0 = GetHAL().millis();
        bool ok     = GetHAL().sdCardFormat(msg);
        printf("SDFORMAT %s in %lu ms\n", msg.c_str(), (unsigned long)(GetHAL().millis() - t0));
        if (ok) {
            bool d1 = roast::EnsureDir("/sdcard/roast/curves") && roast::EnsureDir("/sdcard/roast/logs");
            FILE* f = fopen("/sdcard/roast/curves/gotiti_200g_light_v3.json", "w");
            if (f) {
                fputs("{\n  \"name\": \"Gotiti 200g Light V3\",\n"
                      "  \"note\": \"Drop 75-85 s after FC at +5-6 C, max 206 C; untested\",\n"
                      "  \"batch_g\": 200,\n  \"points\": [\n"
                      "    {\"t\": 0, \"c\": 50, \"tag\": \"CHARGE\"},\n"
                      "    {\"t\": 285, \"c\": 150, \"tag\": \"DRY\"},\n"
                      "    {\"t\": 470, \"c\": 199, \"tag\": \"FCs\"},\n"
                      "    {\"t\": 570, \"c\": 209, \"tag\": \"DROP\"}\n  ]\n}\n",
                      f);
                fclose(f);
            }
            printf("SDFORMAT dirs=%d curve=%d\n", d1 ? 1 : 0, f ? 1 : 0);
        }
        printf("SDINFO\n%sENDSDINFO\n", GetHAL().sdCardInspect().c_str());
    } else if (line == "quiet") {
        // silence for remote testing: runtime only (not saved), so the user's own setting returns after a reset
        audio::set_quiet_mode(true);
        GetHAL().speaker.setVolume(0);
        g_debugQuiet = true;
        printf("OK quiet\n");
    } else if (line == "ping") {
        printf("PONG roast-debug heap=%u largest=%u min=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    }
}

}  // namespace roast
