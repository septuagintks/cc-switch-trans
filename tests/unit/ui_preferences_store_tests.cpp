#include "config/app_paths.hpp"
#include "presentation/ui_preferences_store.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path unique_root(const std::string& label) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("ccs-trans-" + label + "-" + std::to_string(nonce));
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "failed to open test preference file");
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(static_cast<bool>(output), "failed to write test preference file");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "failed to read test preference file");
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void test_default_atomic_save_and_reload() {
    const auto root = unique_root("ui-store");
    const auto paths = ccs::make_app_paths(root);
    ccs::UiPreferencesStore store(paths);
    ccs::UiPreferences preferences;
    preferences.lightweight_mode = false;
    std::string error;

    require(store.load(preferences, error), error);
    require(store.loaded() && preferences.lightweight_mode,
        "missing preference file loads lightweight defaults");
    require(!std::filesystem::exists(paths.ui_preferences_file),
        "read-only default load does not create ui.json");

    preferences.lightweight_mode = false;
    require(store.save(preferences, error), error);
    require(std::filesystem::is_regular_file(paths.ui_preferences_file),
        "atomic preference save creates state/ui.json");
    require(std::filesystem::is_regular_file(paths.state_directory / "ui.lock"),
        "UI preference store uses an independent lock file");
    for (const auto& entry : std::filesystem::directory_iterator(paths.state_directory)) {
        require(entry.path().filename().string().find("ui.json.tmp-") == std::string::npos,
            "successful preference save leaves no temporary file");
    }

    ccs::UiPreferencesStore reloaded(paths);
    ccs::UiPreferences disk;
    require(reloaded.load(disk, error), error);
    require(!disk.lightweight_mode, "saved preference reloads from disk");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void test_stale_and_malformed_preservation() {
    const auto root = unique_root("ui-store-stale");
    const auto paths = ccs::make_app_paths(root);
    ccs::UiPreferences first_value;
    ccs::UiPreferences stale_value;
    std::string error;
    ccs::UiPreferencesStore first(paths);
    ccs::UiPreferencesStore stale(paths);
    require(first.load(first_value, error), error);
    require(stale.load(stale_value, error), error);

    first_value.lightweight_mode = false;
    require(first.save(first_value, error), error);
    const auto saved = read_file(paths.ui_preferences_file);
    stale_value.lightweight_mode = true;
    require(!stale.save(stale_value, error)
            && error.find("changed since it was loaded") != std::string::npos,
        "stale UI preference writer cannot overwrite newer bytes");
    require(read_file(paths.ui_preferences_file) == saved,
        "stale UI preference save leaves disk bytes unchanged");

    const std::string malformed = "{ broken";
    write_file(paths.ui_preferences_file, malformed);
    ccs::UiPreferencesStore malformed_store(paths);
    ccs::UiPreferences target;
    target.lightweight_mode = false;
    require(!malformed_store.load(target, error)
            && error.find("failed to parse") != std::string::npos,
        "malformed UI preference file is rejected");
    require(!target.lightweight_mode, "failed preference load preserves caller state");
    require(!malformed_store.save(ccs::UiPreferences{}, error),
        "failed preference load cannot overwrite malformed bytes");
    require(read_file(paths.ui_preferences_file) == malformed,
        "malformed UI preference bytes remain available for diagnosis");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

#ifdef _WIN32
void test_windows_lock_contention() {
    const auto root = unique_root("ui-store-lock");
    const auto paths = ccs::make_app_paths(root);
    ccs::UiPreferencesStore store(paths);
    ccs::UiPreferences preferences;
    std::string error;
    require(store.load(preferences, error), error);
    require(ccs::ensure_app_directories(paths, error), error);

    HANDLE held_lock = CreateFileW(
        (paths.state_directory / "ui.lock").c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    require(held_lock != INVALID_HANDLE_VALUE, "test acquired external UI lock");
    preferences.lightweight_mode = false;
    require(!store.save(preferences, error)
            && error.find("another process") != std::string::npos,
        "active UI preference writer lock is respected");
    CloseHandle(held_lock);
    require(!std::filesystem::exists(paths.ui_preferences_file),
        "lock contention leaves preference target unchanged");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
#endif

} // namespace

int main() {
    try {
        test_default_atomic_save_and_reload();
        test_stale_and_malformed_preservation();
#ifdef _WIN32
        test_windows_lock_contention();
#endif
        std::cout << "UI preference store tests ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "UI preference store tests failed: " << ex.what() << "\n";
        return 1;
    }
}
