// Desktop stand-in for main/apps/app_roast/roast_audio.cpp: "records" on the fake clock, so the REC indicator, the
// summary line and the JSON audio object can be previewed. Writes a valid but silent WAV header only.
#include <apps/app_roast/roast_audio.h>
#include <hal/hal.h>
#include <cmath>
#include <cstdio>

namespace roast {

float PeakDbfs(int peak)
{
    return peak <= 0 ? -INFINITY : 20.0f * log10f((float)peak / 32768.0f);
}

namespace rec {
namespace {
bool s_active = false;
uint32_t s_t0 = 0;
int s_gain    = 0;
std::vector<AudioGap> s_gaps;
}  // namespace

bool Start(const std::string& path, int gainDb, std::string*)
{
    if (FILE* f = fopen(path.c_str(), "wb")) fclose(f);
    s_active = true;
    s_t0     = GetHAL().millis();
    s_gain   = gainDb;
    s_gaps.clear();
    return true;
}

AudioStats Stats()
{
    AudioStats s;
    s.active     = s_active;
    s.samples    = s_active ? (GetHAL().millis() - s_t0) * (kSampleRate / 1000) : 0;
    s.peak       = 9200;  // about -11 dBFS
    s.recentPeak = 6000;
    s.bufferMs   = 896;
    s.gapCount   = (uint32_t)s_gaps.size();
    s.slot       = 1;
    s.gainDb     = s_gain;
    return s;
}

bool Stop(AudioStats* out, std::vector<AudioGap>* gaps)
{
    AudioStats s = Stats();
    s.active     = false;
    s_active     = false;
    if (out) *out = s;
    if (gaps) *gaps = s_gaps;
    return true;
}

bool Active()
{
    return s_active;
}

uint32_t StartMs()
{
    return s_t0;
}

}  // namespace rec
}  // namespace roast
