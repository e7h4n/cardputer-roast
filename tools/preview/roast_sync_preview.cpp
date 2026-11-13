// Desktop stand-in for main/apps/app_roast/roast_sync.cpp: every job "runs" for 1.5 s on the fake clock and succeeds
// (set ROAST_PREVIEW_SYNC_FAIL=1 to see the failure screen). Nothing touches the network.
#include <apps/app_roast/roast_sync.h>
#include <hal/hal.h>
#include <cstdlib>

namespace roast {
namespace sync {
namespace {
Status s_st;
uint32_t s_t0 = 0;
}  // namespace

bool Start(Job job, const std::string& arg)
{
    s_st         = Status();
    s_st.running = true;
    s_st.job     = job;
    s_st.file    = arg;
    s_t0         = GetHAL().millis();
    return true;
}

Status Get()
{
    Status s     = s_st;
    uint32_t t   = GetHAL().millis() - s_t0;
    bool failing = getenv("ROAST_PREVIEW_SYNC_FAIL") != nullptr;
    if (!s.running) return s;
    if (s.job == Job::SaveRoast) {
        s.step  = "uploading";
        s.file  = s_st.file + ".wav";
        s.total = 58 * 1048576;
        s.done  = (uint32_t)((uint64_t)s.total * std::min<uint32_t>(t, 1500) / 1500);
        s.uploaded = 3;
    } else if (s.job == Job::Curves) {
        s.step       = "downloading";
        s.file       = "Gotiti_200g_V4.json";
        s.downloaded = 1;
        s.unchanged  = 3;
    } else {
        s.step = s.job == Job::PushCurve ? "uploading" : "deleting on Dropbox";
    }
    if (t >= 1500) {
        s.running  = false;
        s.finished = true;
        s.ok       = !failing;
        if (failing) s.error = "Wi-Fi sweethome failed (reason 201)";
        s_st = s;
    }
    return s;
}

}  // namespace sync
}  // namespace roast
