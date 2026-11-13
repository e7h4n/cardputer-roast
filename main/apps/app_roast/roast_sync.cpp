#include "roast_sync.h"
#include "net/roast_net.h"
#include "net/roast_dropbox.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <cctype>
#include <cstdio>
#include <set>
#include <vector>

#ifndef ROAST_SD_ROOT
#define ROAST_SD_ROOT "/sdcard"
#endif

namespace roast {
namespace sync {
namespace {

const char* TAG         = "RoastSync";
const char* kCurveDir   = ROAST_SD_ROOT "/roast/curves";
const char* kLogDir     = ROAST_SD_ROOT "/roast/logs";
constexpr int kWifiWait = 20000;

SemaphoreHandle_t s_mtx;
Status s_st;
Job s_job;
std::string s_arg;
char s_err[96];

void update(const std::function<void(Status&)>& f)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    f(s_st);
    xSemaphoreGive(s_mtx);
}

void step(const std::string& what, const std::string& file = "")
{
    ESP_LOGI(TAG, "%s %s", what.c_str(), file.c_str());
    update([&](Status& s) {
        s.step  = what;
        s.file  = file;
        s.done  = 0;
        s.total = 0;
    });
}

void progress(uint32_t done, uint32_t total, void*)
{
    update([&](Status& s) {
        s.done  = done;
        s.total = total;
    });
}

std::string lower(std::string s)
{
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool isCurveFile(const std::string& name)
{
    std::string l = lower(name);
    auto ends     = [&](const char* e) { return l.size() > strlen(e) && l.compare(l.size() - strlen(e), strlen(e), e) == 0; };
    return ends(".json") || ends(".alog");
}

std::string base(const std::string& path)
{
    size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

bool fail(const char* why)
{
    ESP_LOGE(TAG, "failed: %s", why);
    update([&](Status& s) { s.error = why; });
    return false;
}

// Card := "/Roast Curves" as it is now (last writer wins: the device pushes its own edits right away).
bool curves()
{
    struct Ctx {
        std::vector<rn_dbx_entry_t> files;
    } ctx;
    step("listing Dropbox curves");
    if (!rn_dbx_list(
            kCurvesFolder,
            [](const rn_dbx_entry_t* e, void* c) {
                if (isCurveFile(e->name)) static_cast<Ctx*>(c)->files.push_back(*e);
            },
            &ctx, s_err, sizeof s_err))
        return fail(s_err);
    mkdir(ROAST_SD_ROOT "/roast", 0755);
    mkdir(kCurveDir, 0755);
    std::set<std::string> keep;
    for (auto& e : ctx.files) {
        keep.insert(lower(e.name));
        std::string local = std::string(kCurveDir) + "/" + e.name;
        char have[65];
        if (rn_dbx_local_hash(local.c_str(), have) && e.hash[0] && !strcmp(have, e.hash)) {
            update([](Status& s) { s.unchanged++; });
            continue;
        }
        step("downloading", e.name);
        std::string remote = std::string(kCurvesFolder) + "/" + e.name;
        if (!rn_dbx_download(remote.c_str(), local.c_str(), s_err, sizeof s_err)) return fail(s_err);
        update([](Status& s) { s.downloaded++; });
    }
    std::vector<std::string> extra;
    if (DIR* d = opendir(kCurveDir)) {
        while (dirent* de = readdir(d)) {
            std::string n = de->d_name;
            if (n.empty() || n[0] == '.') continue;
            if (!keep.count(lower(n))) extra.push_back(n);
        }
        closedir(d);
    }
    for (auto& n : extra) {
        step("removing", n);
        if (remove((std::string(kCurveDir) + "/" + n).c_str()) != 0) return fail("could not delete a local curve");
        update([](Status& s) { s.removed++; });
    }
    return true;
}

bool pushCurve(const std::string& local)
{
    step("uploading", base(local));
    std::string remote = std::string(kCurvesFolder) + "/" + base(local);
    if (!rn_dbx_upload(local.c_str(), remote.c_str(), progress, nullptr, s_err, sizeof s_err)) return fail(s_err);
    update([](Status& s) { s.uploaded++; });
    return true;
}

bool deleteCurve(const std::string& local)
{
    step("deleting on Dropbox", base(local));
    std::string remote = std::string(kCurvesFolder) + "/" + base(local);
    if (!rn_dbx_delete(remote.c_str(), s_err, sizeof s_err)) return fail(s_err);
    if (remove(local.c_str()) != 0) return fail("deleted on Dropbox, but not on the card");
    update([](Status& s) { s.removed++; });
    return true;
}

bool saveRoast(const std::string& id)
{
    for (const char* ext : {".csv", ".json", ".alog", ".wav"}) {  // the recording last: it is the big one
        std::string local = std::string(kLogDir) + "/" + id + ext;
        struct stat st;
        if (stat(local.c_str(), &st) != 0) continue;
        step("uploading", id + ext);
        std::string remote = std::string(kLogsFolder) + "/" + id + ext;
        if (!rn_dbx_upload(local.c_str(), remote.c_str(), progress, nullptr, s_err, sizeof s_err)) return fail(s_err);
        update([](Status& s) { s.uploaded++; });
    }
    return true;
}

void run(void*)
{
    bool ok = false;
    step("joining Wi-Fi");
    if (!roast_net_wifi_up(kWifiWait, s_err, sizeof s_err)) {
        fail(s_err);
    } else {
        step("signing in to Dropbox");
        if (!rn_dbx_open(s_err, sizeof s_err)) {
            fail(s_err);
        } else {
            switch (s_job) {
                case Job::Curves:
                    ok = curves();
                    break;
                case Job::PushCurve:
                    ok = pushCurve(s_arg);
                    break;
                case Job::DeleteCurve:
                    ok = deleteCurve(s_arg);
                    break;
                case Job::SaveRoast:
                    ok = saveRoast(s_arg);
                    break;
            }
            rn_dbx_close();
        }
        roast_net_wifi_down();
    }
    ESP_LOGI(TAG, "job %d %s", (int)s_job, ok ? "done" : "FAILED");
    update([&](Status& s) {
        s.running  = false;
        s.finished = true;
        s.ok       = ok;
        if (ok) s.step = "done";
    });
    vTaskDelete(nullptr);
}

}  // namespace

bool Start(Job job, const std::string& arg)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (Get().running) return false;
    s_job = job;
    s_arg = arg;
    update([&](Status& s) {
        s         = Status();
        s.running = true;
        s.job     = job;
    });
    // TLS runs on this task: mbedTLS needs several KB of stack on top of cJSON and the file I/O.
    if (xTaskCreatePinnedToCore(run, "roast_sync", 10240, nullptr, 4, nullptr, 1) != pdPASS) {
        update([](Status& s) {
            s.running  = false;
            s.finished = true;
            s.error    = "out of memory (sync task)";
        });
        return false;
    }
    return true;
}

Status Get()
{
    if (!s_mtx) return Status();
    Status s;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s = s_st;
    xSemaphoreGive(s_mtx);
    return s;
}

}  // namespace sync
}  // namespace roast
