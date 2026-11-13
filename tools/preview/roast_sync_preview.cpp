// Desktop stand-in for main/apps/app_roast/roast_sync.cpp, on the fake clock; nothing touches the network.
// Curve jobs "run" for 1.5 s. SaveRoast uploads for 4 s, then idles (summary still open) until Finish();
// Resave() adds 1.5 s. ROAST_PREVIEW_SYNC_FAIL=1 makes jobs fail.
#include <apps/app_roast/roast_sync.h>
#include <hal/hal.h>
#include <cstdlib>

namespace roast {
namespace sync {
namespace {
Status s_st;
uint32_t s_t0 = 0, s_busyUntil = 0;
bool s_finish = false;
std::string s_id;
}  // namespace

bool Start(Job job, const std::string& arg)
{
    s_st         = Status();
    s_st.running = true;
    s_st.job     = job;
    s_id         = arg;
    s_finish     = false;
    s_t0         = GetHAL().millis();
    s_busyUntil  = s_t0 + (job == Job::SaveRoast ? 4000 : 1500);
    return true;
}

void Resave(const std::string&)
{
    if (s_st.running && s_st.job == Job::SaveRoast) s_busyUntil = GetHAL().millis() + 1500;
}

void Finish()
{
    s_finish = true;
}

Status Get()
{
    Status& s    = s_st;
    uint32_t now = GetHAL().millis();
    if (!s.running) return s;
    if (getenv("ROAST_PREVIEW_SYNC_FAIL") && now >= s_t0 + 1500) {
        s.running = false;
        s.finished = true;
        s.error    = "Wi-Fi sweethome failed (reason 201)";
        return s;
    }
    if (s.job == Job::SaveRoast) {
        if (now < s_busyUntil) {
            bool first = now < s_t0 + 4000;
            s.idle     = false;
            s.step     = "uploading";
            s.file     = s_id + (first ? ".wav" : ".json");
            s.total    = first ? 58 * 1048576 : 900;
            s.done     = first ? (uint32_t)((uint64_t)s.total * (now - s_t0) / 4000) : 450;
        } else {
            s.idle     = true;
            s.step     = "saved";
            s.uploaded = 4;
            if (s_finish) {
                s.running  = false;
                s.finished = true;
                s.ok       = true;
            }
        }
        return s;
    }
    if (s.job == Job::Curves) {
        s.step       = "downloading";
        s.file       = "Gotiti_200g_V4.json";
        s.downloaded = 1;
        s.unchanged  = 3;
    } else {
        s.step = s.job == Job::PushCurve ? "uploading" : "deleting on Dropbox";
    }
    if (now >= s_busyUntil) {
        s.running  = false;
        s.finished = true;
        s.ok       = true;
    }
    return s;
}

}  // namespace sync
}  // namespace roast
