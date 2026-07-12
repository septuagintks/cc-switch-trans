#pragma once

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace ccs {

inline constexpr wchar_t kTrayMutexName[] = L"Local\\ccs-trans-tray";
inline constexpr wchar_t kTrayWindowClass[] = L"ccs-trans.TrayWindow";
inline constexpr wchar_t kTrayShowMessageName[] = L"ccs-trans.tray.show-menu.v1";

enum class InstanceAcquireResult {
    Acquired,
    AlreadyRunning,
    Failed,
};

class InstanceCoordinator {
public:
    explicit InstanceCoordinator(std::wstring mutex_name = kTrayMutexName);
    ~InstanceCoordinator();

    InstanceCoordinator(const InstanceCoordinator&) = delete;
    InstanceCoordinator& operator=(const InstanceCoordinator&) = delete;

    InstanceAcquireResult acquire(std::string& error);
    bool notify_existing(std::string& error) const;

private:
    std::wstring mutex_name_;
    HANDLE mutex_ = nullptr;
};

UINT tray_show_message();

} // namespace ccs

#endif
