// Desktop stand-in for main/apps/app_roast/net/roast_net.c: plays a whole pairing on the fake clock (browser pairs,
// Wi-Fi joins, Dropbox authorizes and verifies) so the setup screen can be previewed. All values are fake.
#include <apps/app_roast/net/roast_net.h>
#include <hal/hal.h>
#include <cstdio>
#include <cstring>

static bool s_open    = false;
static uint32_t s_t0  = 0;
static bool s_wifi    = false, s_dropbox = false;

extern "C" {

bool roast_net_open(void)
{
    s_open = true;
    s_t0   = GetHAL().millis();
    return true;
}

void roast_net_close(void)
{
    if (s_open && GetHAL().millis() - s_t0 > 3500) s_wifi = true;
    if (s_open && GetHAL().millis() - s_t0 > 7000) s_dropbox = true;
    s_open = false;
}

bool roast_net_busy(void)
{
    uint32_t t = GetHAL().millis() - s_t0;
    return s_open && t > 4500 && t < 7000;
}

rn_view_t roast_net_view(void)
{
    rn_view_t v{};
    uint32_t t = s_open ? GetHAL().millis() - s_t0 : 0;
    v.open        = s_open;
    v.passkey     = 482915;
    v.advertising = s_open && t > 300 && t < 1500;
    v.paired      = s_open && t >= 1500;
    snprintf(v.device_name, sizeof v.device_name, "CardputerRoast-3F2A");
    if (t >= 3500) {
        v.state = RN_NET_CONNECTED;
        snprintf(v.ssid, sizeof v.ssid, "Home-2.4G");
        snprintf(v.ip, sizeof v.ip, "192.168.1.42");
    } else if (t >= 2500) {
        v.state = RN_NET_CONNECTING;
        snprintf(v.ssid, sizeof v.ssid, "Home-2.4G");
    }
    const char* d = t >= 7000 ? "connected" : t >= 6000 ? "verifying" : t >= 4500 ? "authorizing" : "unconfigured";
    snprintf(v.dropbox, sizeof v.dropbox, "%s", d);
    return v;
}

bool roast_net_saved_wifi(char ssid[33])
{
    snprintf(ssid, 33, "%s", s_wifi ? "Home-2.4G" : "");
    return s_wifi;
}

bool roast_net_saved_dropbox(void)
{
    return s_dropbox;
}
}
