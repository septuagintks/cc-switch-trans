#ifdef _WIN32

#include "config/app_paths.hpp"
#include "hosts/windows/instance_coordinator.hpp"
#include "hosts/windows/tray_app.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

bool executable_path(std::filesystem::path& path, std::string& error) {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        error = "failed to resolve the tray executable path";
        return false;
    }
    path = std::filesystem::path(buffer.data());
    return true;
}

void show_error(const std::string& error) {
    const int required = MultiByteToWideChar(
        CP_UTF8, 0, error.data(), static_cast<int>(error.size()), nullptr, 0);
    std::wstring message;
    if (required > 0) {
        message.resize(static_cast<std::size_t>(required));
        (void)MultiByteToWideChar(
            CP_UTF8,
            0,
            error.data(),
            static_cast<int>(error.size()),
            message.data(),
            required);
    } else {
        message = L"Unknown error";
    }
    MessageBoxW(nullptr, message.c_str(), L"ccs-trans", MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ccs::InstanceCoordinator coordinator;
    std::string error;
    const auto acquired = coordinator.acquire(error);
    if (acquired == ccs::InstanceAcquireResult::AlreadyRunning) {
        if (!coordinator.notify_existing(error)) {
            show_error(error);
            return 1;
        }
        return 0;
    }
    if (acquired == ccs::InstanceAcquireResult::Failed) {
        show_error(error);
        return 1;
    }

    ccs::AppPaths paths;
    std::filesystem::path executable;
    if (!ccs::resolve_app_paths(paths, error) || !executable_path(executable, error)) {
        show_error(error);
        return 1;
    }

    ccs::TrayApplication application(instance, std::move(paths), std::move(executable));
    const int exit_code = application.run();
    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }
    return exit_code;
}

#endif
