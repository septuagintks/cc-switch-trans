#pragma once

#include "hosts/host_platform.hpp"
#include "hosts/windows/startup_registration.hpp"

namespace ccs {

class WindowsHostPlatform final : public HostPlatform {
public:
    bool open_config_file(const AppPaths& paths, std::string& error) override;
    bool open_logs_directory(const AppPaths& paths, std::string& error) override;
    bool startup_registered(
        const std::filesystem::path& executable,
        bool& enabled,
        std::string& error) override;
    bool set_startup_registered(
        const std::filesystem::path& executable,
        bool enabled,
        std::string& error) override;

private:
    WindowsStartupRegistration startup_registration_;
};

} // namespace ccs
