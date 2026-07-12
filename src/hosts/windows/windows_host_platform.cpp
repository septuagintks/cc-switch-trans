#include "hosts/windows/windows_host_platform.hpp"
#include "hosts/windows/windows_error.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace ccs {

namespace {

bool open_path(const std::filesystem::path& path, std::string& error) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = path.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        error = windows_error_message("failed to open " + path.string(), GetLastError());
        return false;
    }
    return true;
}

} // namespace

bool WindowsHostPlatform::open_config_file(const AppPaths& paths, std::string& error) {
    error.clear();
    return ensure_config_file(paths, error) && open_path(paths.config_file, error);
}

bool WindowsHostPlatform::open_logs_directory(const AppPaths& paths, std::string& error) {
    error.clear();
    return ensure_app_directories(paths, error) && open_path(paths.logs_directory, error);
}

bool WindowsHostPlatform::startup_registered(
    const std::filesystem::path& executable,
    bool& enabled,
    std::string& error) {
    return startup_registration_.registered(executable, enabled, error);
}

bool WindowsHostPlatform::set_startup_registered(
    const std::filesystem::path& executable,
    bool enabled,
    std::string& error) {
    return startup_registration_.set_registered(executable, enabled, error);
}

} // namespace ccs

#endif
