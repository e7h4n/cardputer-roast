#pragma once
#include <string>
namespace mooncake {
struct AppInfo_t {
    std::string name;
    void* userData = nullptr;
};
class AppAbility {
public:
    virtual ~AppAbility() = default;
    virtual void onCreate() {}
    virtual void onOpen() {}
    virtual void onRunning() {}
    virtual void onSleeping() {}
    virtual void onClose() {}
    AppInfo_t& setAppInfo() { return _info; }
    const AppInfo_t& getAppInfo() { return _info; }
    void close() { closed = true; }
    bool closed = false;

private:
    AppInfo_t _info;
};
}  // namespace mooncake
