/*
 * Firmware update from GitHub releases, installed by the Launcher.
 *
 * CI publishes a release for every merge to main (v0.3.<commits on main>): the app image, SHA256SUMS and latest.json
 * ({"version","bin","size","sha256"}). Rules > SYNC > firmware reads latest.json of the latest release
 * (github.com/<repo>/releases/latest/download/latest.json: no API, no token, no rate limit) and, when it is newer than
 * this build, downloads the image to the SD card as /roast/update/Roast.bin through a .part file, checking its size
 * and SHA-256. Roast cannot replace itself (it runs from the partition the Launcher installed it in), so the Launcher
 * installs it: bind a key to that file once (Launcher SD browser > Roast.bin > Bind to key, e.g. U), then after each
 * download go to the Launcher and press that key. When the flash is full the Launcher asks which app partition to
 * reuse: pick the old Roast. NVS (rules, Wi-Fi, Dropbox) is not touched by an install.
 */
#pragma once
#include <cstdint>
#include <string>

#ifndef ROAST_SD_ROOT
#define ROAST_SD_ROOT "/sdcard"
#endif

namespace roast {
namespace update {

constexpr const char* kRepo        = "e7h4n/cardputer-roast";
constexpr const char* kManifestUrl = "https://github.com/e7h4n/cardputer-roast/releases/latest/download/latest.json";
constexpr const char* kDir         = ROAST_SD_ROOT "/roast/update";
constexpr const char* kImage       = ROAST_SD_ROOT "/roast/update/Roast.bin";
constexpr const char* kLauncherPath = "/roast/update/Roast.bin";  // as the Launcher's SD browser shows it

/// This build's version: v0.3.N from CI (main), the tag for a tagged build, otherwise what the build had (git
/// describe or a short hash; "dev" on the desktop).
const char* Installed();

struct Version {
    int major = 0, minor = 0, patch = 0;
};
/// "v1.2.3" or "1.2.3", nothing before or after. Builds that are not releases (a hash, "v0.2.0-12-gabc", "dev") fail.
bool ParseRelease(const std::string& s, Version& v);
int Compare(const Version& a, const Version& b);  // <0, 0, >0
/// Whether `latest` is worth downloading on a device running `installed`: a newer release, or any release when this
/// build is not one (a local or PR build).
bool Offer(const std::string& installed, const std::string& latest);

struct Manifest {
    std::string version;  // release tag, e.g. v0.3.58
    std::string bin;      // asset name, e.g. cardputer-roast-v0.3.58.bin
    std::string sha256;   // 64 lowercase hex digits
    uint32_t size = 0;    // bytes
};
/// latest.json. Checked: version is a release, bin is a plain *.bin file name, sha256 is 64 hex digits, size fits an
/// app partition (64 KB..4 MB). why: a short reason when it fails.
bool ParseManifest(const std::string& json, Manifest& m, std::string* why = nullptr);
/// Download URL of a release asset.
std::string AssetUrl(const std::string& tag, const std::string& file);

}  // namespace update
}  // namespace roast
