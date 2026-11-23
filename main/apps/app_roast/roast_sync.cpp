#include "roast_sync.h"
#include "okou_agent.h"
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
#include <cerrno>
#include <memory>
#include <strings.h>
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
constexpr int kHubWait = 45000;

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
    const char *manifestPath = ROAST_SD_ROOT "/roast/.sync-index";
    const char *pagePath = ROAST_SD_ROOT "/roast/.sync-page";
    struct Cleanup {
        const char *index, *page;
        ~Cleanup() { remove(index); remove(page); }
    } cleanup{manifestPath, pagePath};
    using File = std::unique_ptr<FILE, decltype(&fclose)>;
    File manifest(fopen(manifestPath, "w+b"), fclose);
    if (!manifest) return fail("cannot create sync files on SD");
    step("listing Dropbox curves");
    auto record = [](const rn_dbx_entry_t *e, void *ctx) {
        return !isCurveFile(e->name) || fwrite(e, sizeof(*e), 1, static_cast<FILE *>(ctx)) == 1;
    };
    if (!rn_dbx_list(kCurvesFolder, pagePath, record, manifest.get(), s_err, sizeof s_err))
        return fail(s_err);
    if (fflush(manifest.get()) || fseek(manifest.get(), 0, SEEK_SET)) return fail("cannot finish SD index");
    if (mkdir(kCurveDir, 0755) && errno != EEXIST) return fail("cannot create curve directory");
    rn_dbx_entry_t e;
    for (;;) {
        size_t bytes = fread(&e, 1, sizeof e, manifest.get());
        if (!bytes && feof(manifest.get()) && !ferror(manifest.get())) break;
        if (bytes != sizeof e) return fail("cannot read SD index");
        std::string local = std::string(kCurveDir) + "/" + e.name;
        char have[65];
        if (e.hash[0] && rn_dbx_local_hash(local.c_str(), have) && !strcmp(have, e.hash)) {
            update([](Status& s) { s.unchanged++; });
            continue;
        }
        step("downloading", e.name);
        std::string remote = std::string(kCurvesFolder) + "/" + e.name;
        if (!rn_dbx_download(remote.c_str(), local.c_str(), s_err, sizeof s_err)) return fail(s_err);
        update([](Status& s) { s.downloaded++; });
    }
    // FAT directories are small on this device. Scan the disk index instead
    // of retaining all names in SRAM. Only prune after every page and download succeeded.
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(kCurveDir), closedir);
    if (!dir) return fail("cannot open curve directory");
    for (;;) {
        errno = 0;
        dirent *de = readdir(dir.get());
        if (!de) {
            if (errno) return fail("cannot read curve directory");
            break;
        }
        if (de->d_name[0] == '.' || !isCurveFile(de->d_name)) continue;
        if (fseek(manifest.get(), 0, SEEK_SET)) return fail("cannot rewind SD index");
        size_t n;
        while ((n = fread(&e, 1, sizeof e, manifest.get())) == sizeof e && strcasecmp(e.name, de->d_name)) {}
        if (n == sizeof e) continue;
        if (n || ferror(manifest.get()) || !feof(manifest.get())) return fail("cannot read SD index");
        step("removing", de->d_name);
        if (remove((std::string(kCurveDir) + "/" + de->d_name).c_str())) return fail("could not delete a local curve");
        update([](Status& s) { s.removed++; });
    }
    const Status totals = Get();
    ESP_LOGI(TAG, "curve sync complete: downloaded=%d unchanged=%d removed=%d",
             totals.downloaded, totals.unchanged, totals.removed);
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

std::vector<std::string> s_pending;  // SaveRoast: extensions still to upload, in order
bool s_finish = false;

bool saveRoast(const std::string& id)
{
    update([](Status& s) { s.idle = false; });
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_pending.clear();
    for (const char* ext : {".csv", ".json", ".alog"}) {
        struct stat st;
        if (stat((std::string(kLogDir) + "/" + id + ext).c_str(), &st) == 0) s_pending.push_back(ext);
    }
    xSemaphoreGive(s_mtx);
    for (;;) {
        std::string ext;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (!s_pending.empty()) {
            ext = s_pending.front();
            s_pending.erase(s_pending.begin());
            s_st.idle = false;
        } else {
            s_st.idle = true;
            if (s_finish) {
                xSemaphoreGive(s_mtx);
                return true;
            }
        }
        xSemaphoreGive(s_mtx);
        if (ext.empty()) {  // waiting for an edit to re-save, or for the summary to close
            if (Get().step != "saved") step("saved");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        std::string local = std::string(kLogDir) + "/" + id + ext;
        step("uploading", id + ext);
        std::string remote = std::string(kLogsFolder) + "/" + id + ext;
        if (!rn_dbx_upload(local.c_str(), remote.c_str(), progress, nullptr, s_err, sizeof s_err)) return fail(s_err);
        update([](Status& s) { s.uploaded++; });
    }
}

void run(void*)
{
    bool ok = false;
    step("waiting for ESP Hub Wi-Fi");
    if (s_job == Job::Curves && mkdir(ROAST_SD_ROOT "/roast", 0755) && errno != EEXIST) {
        fail("SD card unavailable");
    } else if (!roast_net_wait_for_network(kWifiWait, s_err, sizeof s_err)) {
        fail(s_err);
    } else {
        if (s_job == Job::Curves && !okou_connected()) {
            step("waiting for ESP Hub");
            // Boot sync shares internal RAM with MQTT enrollment and its first TLS handshake.
            // Keep Dropbox available when the Hub is unreachable.
            const TickType_t start = xTaskGetTickCount();
            while (!okou_connected() && xTaskGetTickCount() - start < pdMS_TO_TICKS(kHubWait)) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
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
    s_job    = job;
    s_arg    = arg;
    s_finish = false;
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

void Resave(const std::string& ext)
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_st.running && s_job == Job::SaveRoast &&
        std::find(s_pending.begin(), s_pending.end(), ext) == s_pending.end()) {
        s_pending.push_back(ext);
        s_st.idle = false;
    }
    xSemaphoreGive(s_mtx);
}

void Finish()
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_finish = true;
    xSemaphoreGive(s_mtx);
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
