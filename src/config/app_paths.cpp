#include "config/app_paths.hpp"

#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ccs {

namespace {

bool resolve_home_directory(std::filesystem::path& home, std::string& error) {
#ifdef _WIN32
    PWSTR raw_path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &raw_path);
    if (FAILED(result) || raw_path == nullptr) {
        error = "failed to resolve the current user profile directory";
        return false;
    }
    home = std::filesystem::path(raw_path);
    CoTaskMemFree(raw_path);
    return true;
#else
    const passwd* account = getpwuid(getuid());
    if (account == nullptr || account->pw_dir == nullptr || account->pw_dir[0] == '\0') {
        error = "failed to resolve the current user home directory";
        return false;
    }
    home = std::filesystem::path(account->pw_dir);
    return true;
#endif
}

bool create_directory(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        error = "failed to create application directory " + path.string() + ": " + ec.message();
        return false;
    }
#ifndef _WIN32
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        ec);
    if (ec) {
        error = "failed to restrict application directory " + path.string() + ": " + ec.message();
        return false;
    }
#endif
    return true;
}

} // namespace

AppPaths make_app_paths(const std::filesystem::path& root) {
    return AppPaths{
        root,
        root / "config.json",
        root / "logs",
        root / "logs" / "ccs-trans.log",
        root / "state",
    };
}

bool resolve_app_paths(AppPaths& paths, std::string& error) {
    std::filesystem::path home;
    if (!resolve_home_directory(home, error)) {
        return false;
    }
    paths = make_app_paths(home / ".ccs-trans");
    return true;
}

bool ensure_app_directories(const AppPaths& paths, std::string& error) {
    return create_directory(paths.root, error)
        && create_directory(paths.logs_directory, error)
        && create_directory(paths.state_directory, error);
}

} // namespace ccs
