#include "hosts/windows/instance_coordinator.hpp"

#ifdef _WIN32

#include "hosts/windows/windows_error.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace ccs {

InstanceCoordinator::InstanceCoordinator(
    std::wstring mutex_name,
    std::wstring window_class,
    std::wstring window_title)
    : mutex_name_(std::move(mutex_name))
    , window_class_(std::move(window_class))
    , window_title_(std::move(window_title)) {}

InstanceCoordinator::~InstanceCoordinator() {
    if (mutex_ != nullptr) {
        CloseHandle(mutex_);
    }
}

InstanceAcquireResult InstanceCoordinator::acquire(std::string& error) {
    error.clear();
    if (mutex_ != nullptr) {
        return InstanceAcquireResult::Acquired;
    }
    mutex_ = CreateMutexW(nullptr, FALSE, mutex_name_.c_str());
    if (mutex_ == nullptr) {
        error = windows_error_message("failed to create tray instance mutex", GetLastError());
        return InstanceAcquireResult::Failed;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return InstanceAcquireResult::AlreadyRunning;
    }
    return InstanceAcquireResult::Acquired;
}

bool InstanceCoordinator::notify_existing(std::string& error) const {
    error.clear();
    for (int attempt = 0; attempt < 40; ++attempt) {
        const HWND window = FindWindowW(
            window_class_.c_str(),
            window_title_.empty() ? nullptr : window_title_.c_str());
        if (window != nullptr) {
            if (PostMessageW(window, tray_show_message(), 0, 0)) {
                return true;
            }
            error = windows_error_message("failed to notify existing tray instance", GetLastError());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = "existing tray instance did not create its window within 2 seconds";
    return false;
}

UINT tray_show_message() {
    static const UINT message = RegisterWindowMessageW(kTrayShowMessageName);
    return message;
}

} // namespace ccs

#endif
