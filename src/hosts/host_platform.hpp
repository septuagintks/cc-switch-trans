#pragma once

#include "config/app_paths.hpp"

#include <filesystem>
#include <string>

namespace ccs {

bool ensure_config_file(const AppPaths& paths, std::string& error);

class HostPlatform {
public:
    virtual ~HostPlatform() = default;

    virtual bool open_config_file(const AppPaths& paths, std::string& error) = 0;
    virtual bool open_logs_directory(const AppPaths& paths, std::string& error) = 0;
    virtual bool startup_registered(
        const std::filesystem::path& executable,
        bool& enabled,
        std::string& error) = 0;
    virtual bool set_startup_registered(
        const std::filesystem::path& executable,
        bool enabled,
        std::string& error) = 0;
};

} // namespace ccs
