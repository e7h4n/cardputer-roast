/*
 * Dropbox sync (app folder; paths below are inside it). Wi-Fi and Dropbox are required: they are set up once under
 * Rules > network, and every job fails fast with a short reason; nothing is retried.
 *
 *  - Curves      boot (and S in the curve library): mirror "/Roast Curves" onto roast/curves. Afterwards the card holds
 *                exactly the folder's .json/.alog files as they were at that moment: missing or different files are
 *                downloaded (content_hash compared), local curve files not in the folder are deleted.
 *  - PushCurve   after a curve is saved on the device: upload that one file, overwriting.
 *  - DeleteCurve after a curve is deleted on the device: delete that one file on Dropbox, then on the card.
 *  - SaveRoast   once the fan is off on a roast's summary: upload <id>.csv/.json/.alog to "/Roast Logs" (not the
 *                .wav: it stays on the card),
 *                overwriting. The summary stays usable meanwhile; an edit there (weight, note) rewrites the .json and
 *                .alog, and Resave() uploads them again after the current file. Finish() ends the session once nothing
 *                is pending; the summary cannot be closed before that. The record is not synced again afterwards.
 * Every upload and download is checked against Dropbox's content_hash. Jobs run on their own task while the roaster
 * link is closed (BLE + Wi-Fi + TLS do not fit together without PSRAM); the app restarts afterwards for a clean heap.
 */
#pragma once
#include <cstdint>
#include <string>

namespace roast {
namespace sync {

enum class Job : uint8_t { Curves, PushCurve, DeleteCurve, SaveRoast };

constexpr const char* kCurvesFolder = "/Roast Curves";
constexpr const char* kLogsFolder   = "/Roast Logs";

struct Status {
    bool running  = false;
    bool finished = false;
    bool ok       = false;
    Job job       = Job::Curves;
    std::string step;   // what is happening now
    std::string file;   // file being transferred
    std::string error;  // why it failed
    uint32_t done = 0, total = 0;  // bytes of the current file
    int downloaded = 0, removed = 0, unchanged = 0, uploaded = 0;
    bool idle = false;  // SaveRoast: everything uploaded, waiting for a re-save or Finish()
};

/// `arg`: PushCurve/DeleteCurve = local curve path, SaveRoast = log id. False if a job is already running.
bool Start(Job job, const std::string& arg);
Status Get();
/// SaveRoast session: upload `<id><ext>` again (after an edit rewrote it). Ignored once the session has ended.
void Resave(const std::string& ext);
/// SaveRoast session: end once nothing is pending (the task brings Wi-Fi down and finishes).
void Finish();

}  // namespace sync
}  // namespace roast
