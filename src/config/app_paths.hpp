#pragma once

#include <filesystem>
#include <string>

namespace ccs {

struct AppPaths {
    std::filesystem::path root;
    std::filesystem::path config_file;
    std::filesystem::path logs_directory;
    std::filesystem::path default_log_file;
    std::filesystem::path host_log_file;
    std::filesystem::path state_directory;
    std::filesystem::path ui_preferences_file;
};

AppPaths make_app_paths(const std::filesystem::path& root);
bool resolve_app_paths(AppPaths& paths, std::string& error);
bool ensure_app_directories(const AppPaths& paths, std::string& error);

} // namespace ccs
