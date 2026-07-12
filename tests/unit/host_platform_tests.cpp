#include "config/config_document.hpp"
#include "config/config_store.hpp"
#include "hosts/host_platform.hpp"
#include "hosts/windows/startup_registration.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeStartupValueStore final : public ccs::StartupValueStore {
public:
    bool read(std::optional<std::wstring>& output, std::string&) const override {
        output = value;
        return true;
    }

    bool write(const std::optional<std::wstring>& input, std::string&) const override {
        value = input;
        ++write_count;
        return true;
    }

    mutable std::optional<std::wstring> value;
    mutable int write_count = 0;
};

void test_config_file_preparation() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("ccs-trans-host-platform-" + std::to_string(nonce));
    const auto paths = ccs::make_app_paths(root);
    std::string error;
    require(paths.host_log_file == root / "logs" / "ccs-trans-host.log",
        "application paths expose the fixed host log file");

    const bool prepared = ccs::ensure_config_file(paths, error);
    require(prepared, error);
    require(std::filesystem::is_regular_file(paths.config_file),
        "config preparation creates a regular config file");
    ccs::ConfigStore store(paths);
    const bool loaded = store.load(error);
    require(loaded, "prepared config does not load: " + error);
    require(store.document().profiles.empty(), "prepared config uses default profiles");
    const bool prepared_again = ccs::ensure_config_file(paths, error);
    require(prepared_again, "config preparation is not idempotent: " + error);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void test_windows_startup_command() {
    const std::filesystem::path executable =
        L"C:\\Program Files\\ccs-trans\\ccs-trans-tray.exe";
    const auto command = ccs::windows_startup_command(executable);
    require(command == L"\"C:\\Program Files\\ccs-trans\\ccs-trans-tray.exe\"",
        "startup command quotes the executable path exactly");
    require(ccs::windows_startup_command_matches(command, executable),
        "startup command matches its executable");
    require(ccs::windows_startup_command_matches(
                L"\"c:\\program files\\CCS-TRANS\\ccs-trans-tray.exe\"",
                executable),
        "startup command comparison follows Windows path casing");
    require(!ccs::windows_startup_command_matches(
                L"\"C:\\Program Files\\ccs-trans\\other.exe\"",
                executable),
        "startup command rejects a different executable");

    const auto store = std::make_shared<FakeStartupValueStore>();
    ccs::WindowsStartupRegistration registration(store);
    bool enabled = true;
    std::string error;
    require(!registration.registered("relative.exe", enabled, error)
            && error.find("absolute") != std::string::npos,
        "startup registration rejects relative executable paths before registry access");

    error.clear();
    require(registration.registered(executable, enabled, error) && !enabled,
        "missing startup value reports disabled");
    bool updated = registration.set_registered(executable, true, error);
    require(updated, error);
    require(store->value == command && store->write_count == 1,
        "enable writes the canonical startup command");
    require(registration.set_registered(executable, true, error) && store->write_count == 1,
        "repeated enable is idempotent");
    require(registration.registered(executable, enabled, error) && enabled,
        "canonical startup value reports enabled");

    store->value = L"\"C:\\old\\ccs-trans-tray.exe\"";
    updated = registration.set_registered(executable, true, error);
    require(updated, error);
    require(store->value == command && store->write_count == 2,
        "enable repairs a stale executable path");
    updated = registration.set_registered(executable, false, error);
    require(updated, error);
    require(!store->value && store->write_count == 3, "disable removes the startup value");
    require(registration.set_registered(executable, false, error) && store->write_count == 3,
        "repeated disable is idempotent");
}

} // namespace

int main() {
    try {
        test_config_file_preparation();
        test_windows_startup_command();
        std::cout << "host platform tests ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "host platform tests failed: " << ex.what() << "\n";
        return 1;
    }
}
