#include "config/app_paths.hpp"

#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ccs {

namespace {

bool resolve_home_directory(std::filesystem::path& home, std::string& error) {
#ifdef _WIN32
    const DWORD required = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
    if (required == 0) {
        error = "USERPROFILE is not set";
        return false;
    }
    std::vector<wchar_t> buffer(required);
    const DWORD written = GetEnvironmentVariableW(
        L"USERPROFILE",
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        error = "failed to read USERPROFILE";
        return false;
    }
    home = std::filesystem::path(buffer.data());
    if (!home.is_absolute()) {
        error = "USERPROFILE must be an absolute path";
        return false;
    }
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
