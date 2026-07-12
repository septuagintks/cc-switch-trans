#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace ccs {

std::wstring windows_startup_command(const std::filesystem::path& executable);
bool windows_startup_command_matches(
    const std::wstring& command,
    const std::filesystem::path& executable);

class StartupValueStore {
public:
    virtual ~StartupValueStore() = default;

    virtual bool read(std::optional<std::wstring>& value, std::string& error) const = 0;
    virtual bool write(const std::optional<std::wstring>& value, std::string& error) const = 0;
};

class WindowsStartupRegistration {
public:
    explicit WindowsStartupRegistration(std::shared_ptr<StartupValueStore> store = {});

    bool registered(
        const std::filesystem::path& executable,
        bool& enabled,
        std::string& error) const;
    bool set_registered(
        const std::filesystem::path& executable,
        bool enabled,
        std::string& error) const;

private:
    std::shared_ptr<StartupValueStore> store_;
};

} // namespace ccs
