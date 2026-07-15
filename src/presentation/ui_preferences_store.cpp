#include "presentation/ui_preferences_store.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace ccs {

namespace {

bool read_preferences_file(
    const std::filesystem::path& path,
    bool& exists,
    std::string& content,
    std::string& error) {
    exists = false;
    content.clear();
    std::error_code ec;
    const bool file_exists = std::filesystem::exists(path, ec);
    if (ec) {
        error = "failed to inspect UI preference file: " + ec.message();
        return false;
    }
    if (!file_exists) {
        return true;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "failed to inspect UI preference file size: " + ec.message();
        return false;
    }
    if (size > kMaxUiPreferencesBytes) {
        error = "UI preferences exceed the 64 KiB limit";
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "failed to open UI preference file: " + path.string();
        return false;
    }
    content.reserve(static_cast<std::size_t>(size));
    std::array<char, 4096> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            content.append(buffer.data(), static_cast<std::size_t>(count));
            if (content.size() > kMaxUiPreferencesBytes) {
                error = "UI preferences exceed the 64 KiB limit";
                return false;
            }
        }
    }
    if (!input.eof()) {
        error = "failed to read UI preference file: " + path.string();
        return false;
    }
    exists = true;
    return true;
}

std::filesystem::path temporary_path(const std::filesystem::path& target) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto process_id = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto process_id = static_cast<unsigned long long>(getpid());
#endif
    return target.parent_path()
        / (target.filename().string() + ".tmp-" + std::to_string(process_id) + "-"
            + std::to_string(nonce));
}

class PreferenceWriteLock {
public:
    PreferenceWriteLock() = default;
    PreferenceWriteLock(const PreferenceWriteLock&) = delete;
    PreferenceWriteLock& operator=(const PreferenceWriteLock&) = delete;

    ~PreferenceWriteLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
#else
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            close(fd_);
        }
#endif
    }

    bool acquire(const std::filesystem::path& path, std::string& error) {
#ifdef _WIN32
        handle_ = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            error = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION
                ? "UI preferences are being modified by another process"
                : "failed to acquire UI preference write lock: Windows error "
                    + std::to_string(code);
            return false;
        }
#else
        fd_ = open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd_ < 0) {
            error = "failed to open UI preference write lock: "
                + std::string(std::strerror(errno));
            return false;
        }
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            error = errno == EWOULDBLOCK
                ? "UI preferences are being modified by another process"
                : "failed to acquire UI preference write lock: "
                    + std::string(std::strerror(errno));
            close(fd_);
            fd_ = -1;
            return false;
        }
#endif
        return true;
    }

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

class TemporaryFile {
public:
    explicit TemporaryFile(std::filesystem::path path)
        : path_(std::move(path)) {}

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    ~TemporaryFile() {
        if (active_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    void release() noexcept {
        active_ = false;
    }

private:
    std::filesystem::path path_;
    bool active_ = true;
};

bool write_file(
    const std::filesystem::path& path,
    const std::string& content,
    std::string& error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "failed to create temporary UI preference file: " + path.string();
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) {
        error = "failed to write temporary UI preference file: " + path.string();
        return false;
    }
    output.close();
    if (!output) {
        error = "failed to close temporary UI preference file: " + path.string();
        return false;
    }
#ifndef _WIN32
    std::error_code permissions_error;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        permissions_error);
    if (permissions_error) {
        error = "failed to restrict temporary UI preference file: "
            + permissions_error.message();
        return false;
    }
#endif
    return true;
}

bool replace_file(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    std::string& error) {
#ifdef _WIN32
    if (!MoveFileExW(
            source.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "failed to atomically replace UI preference file: Windows error "
            + std::to_string(GetLastError());
        return false;
    }
#else
    if (std::rename(source.c_str(), target.c_str()) != 0) {
        error = "failed to atomically replace UI preference file: "
            + std::string(std::strerror(errno));
        return false;
    }
#endif
    return true;
}

} // namespace

UiPreferencesStore::UiPreferencesStore(AppPaths paths)
    : paths_(std::move(paths)) {}

bool UiPreferencesStore::load(UiPreferences& preferences, std::string& error) {
    error.clear();
    loaded_ = false;
    bool exists = false;
    std::string content;
    if (!read_preferences_file(paths_.ui_preferences_file, exists, content, error)) {
        return false;
    }
    UiPreferences candidate = make_default_ui_preferences();
    if (exists && !parse_ui_preferences(content, candidate, error)) {
        return false;
    }
    source_content_ = std::move(content);
    source_exists_ = exists;
    loaded_ = true;
    preferences = candidate;
    return true;
}

bool UiPreferencesStore::source_is_unchanged(std::string& error) const {
    bool exists = false;
    std::string content;
    if (!read_preferences_file(paths_.ui_preferences_file, exists, content, error)) {
        return false;
    }
    if (exists != source_exists_ || (exists && content != source_content_)) {
        error = "UI preference file changed since it was loaded; reload before saving";
        return false;
    }
    return true;
}

bool UiPreferencesStore::save(const UiPreferences& preferences, std::string& error) {
    error.clear();
    if (!loaded_) {
        error = "UI preference store must be loaded successfully before saving";
        return false;
    }
    std::string serialized;
    if (!serialize_ui_preferences(preferences, serialized, error)
        || !ensure_app_directories(paths_, error)) {
        return false;
    }

    PreferenceWriteLock lock;
    if (!lock.acquire(paths_.state_directory / "ui.lock", error)
        || !source_is_unchanged(error)) {
        return false;
    }

    TemporaryFile temporary(temporary_path(paths_.ui_preferences_file));
    if (!write_file(temporary.path(), serialized, error)) {
        return false;
    }
    bool verification_exists = false;
    std::string verification_content;
    if (!read_preferences_file(
            temporary.path(), verification_exists, verification_content, error)
        || !verification_exists) {
        if (error.empty()) {
            error = "temporary UI preference file disappeared before verification";
        }
        return false;
    }
    UiPreferences verified;
    if (!parse_ui_preferences(verification_content, verified, error)
        || verified != preferences) {
        if (error.empty()) {
            error = "temporary UI preference file failed canonical verification";
        }
        return false;
    }
    if (!source_is_unchanged(error)
        || !replace_file(temporary.path(), paths_.ui_preferences_file, error)) {
        return false;
    }
    temporary.release();
    source_content_ = std::move(serialized);
    source_exists_ = true;
    return true;
}

bool UiPreferencesStore::loaded() const noexcept {
    return loaded_;
}

const AppPaths& UiPreferencesStore::paths() const noexcept {
    return paths_;
}

} // namespace ccs
