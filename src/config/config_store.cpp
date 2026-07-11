#include "config/config_store.hpp"

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

bool read_config_file(
    const std::filesystem::path& path,
    bool& exists,
    std::string& content,
    std::string& error) {
    exists = false;
    content.clear();
    std::error_code ec;
    const bool file_exists = std::filesystem::exists(path, ec);
    if (ec) {
        error = "failed to inspect config file: " + ec.message();
        return false;
    }
    if (!file_exists) {
        return true;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "failed to inspect config file size: " + ec.message();
        return false;
    }
    if (size > kMaxConfigDocumentBytes) {
        error = "config document exceeds the 4 MiB limit";
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "failed to open config file: " + path.string();
        return false;
    }
    content.reserve(static_cast<std::size_t>(size));
    std::array<char, 8192> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            content.append(buffer.data(), static_cast<std::size_t>(count));
            if (content.size() > kMaxConfigDocumentBytes) {
                error = "config document exceeds the 4 MiB limit";
                return false;
            }
        }
    }
    if (!input.eof()) {
        error = "failed to read config file: " + path.string();
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
        / (target.filename().string() + ".tmp-" + std::to_string(process_id) + "-" + std::to_string(nonce));
}

class ConfigWriteLock {
public:
    ConfigWriteLock() = default;
    ConfigWriteLock(const ConfigWriteLock&) = delete;
    ConfigWriteLock& operator=(const ConfigWriteLock&) = delete;

    ~ConfigWriteLock() {
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
            if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
                error = "config is being modified by another process";
            } else {
                error = "failed to acquire config write lock: Windows error " + std::to_string(code);
            }
            return false;
        }
#else
        fd_ = open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd_ < 0) {
            error = "failed to open config write lock: " + std::string(std::strerror(errno));
            return false;
        }
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            error = errno == EWOULDBLOCK
                ? "config is being modified by another process"
                : "failed to acquire config write lock: " + std::string(std::strerror(errno));
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

    const std::filesystem::path& path() const {
        return path_;
    }

    void release() {
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
        error = "failed to create temporary config file: " + path.string();
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) {
        error = "failed to write temporary config file: " + path.string();
        return false;
    }
    output.close();
    if (!output) {
        error = "failed to close temporary config file: " + path.string();
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
        error = "failed to restrict temporary config file: " + permissions_error.message();
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
        error = "failed to atomically replace config file: Windows error "
            + std::to_string(GetLastError());
        return false;
    }
#else
    if (std::rename(source.c_str(), target.c_str()) != 0) {
        error = "failed to atomically replace config file: " + std::string(std::strerror(errno));
        return false;
    }
#endif
    return true;
}

} // namespace

ConfigStore::ConfigStore(AppPaths paths)
    : paths_(std::move(paths)) {}

bool ConfigStore::load(std::string& error) {
    error.clear();
    loaded_ = false;
    bool exists = false;
    std::string content;
    if (!read_config_file(paths_.config_file, exists, content, error)) {
        return false;
    }
    ConfigDocument candidate;
    if (exists && !parse_config_document(content, candidate, error)) {
        return false;
    }
    if (!exists) {
        candidate = make_default_config_document();
    }
    document_ = std::move(candidate);
    source_content_ = std::move(content);
    source_exists_ = exists;
    loaded_ = true;
    return true;
}

bool ConfigStore::source_is_unchanged(std::string& error) const {
    bool exists = false;
    std::string content;
    if (!read_config_file(paths_.config_file, exists, content, error)) {
        return false;
    }
    if (exists != source_exists_ || (exists && content != source_content_)) {
        error = "config file changed since it was loaded; reload before saving";
        return false;
    }
    return true;
}

bool ConfigStore::save(const ConfigDocument& document, std::string& error) {
    error.clear();
    if (!loaded_) {
        error = "config store must be loaded successfully before saving";
        return false;
    }
    std::string serialized;
    if (!serialize_config_document(document, serialized, error)) {
        return false;
    }
    if (!ensure_app_directories(paths_, error)) {
        return false;
    }

    ConfigWriteLock lock;
    if (!lock.acquire(paths_.state_directory / "config.lock", error)
        || !source_is_unchanged(error)) {
        return false;
    }

    TemporaryFile temporary(temporary_path(paths_.config_file));
    if (!write_file(temporary.path(), serialized, error)) {
        return false;
    }
    bool verification_exists = false;
    std::string verification_content;
    if (!read_config_file(temporary.path(), verification_exists, verification_content, error)
        || !verification_exists) {
        if (error.empty()) {
            error = "temporary config file disappeared before verification";
        }
        return false;
    }
    ConfigDocument verified;
    if (!parse_config_document(verification_content, verified, error)) {
        error = "failed to verify temporary config file: " + error;
        return false;
    }
    std::string verified_serialized;
    if (!serialize_config_document(verified, verified_serialized, error)
        || verified_serialized != serialized) {
        if (error.empty()) {
            error = "temporary config file failed canonical round-trip verification";
        }
        return false;
    }
    if (!source_is_unchanged(error)) {
        return false;
    }

    ConfigDocument next_document = document;
    std::string next_source = serialized;
    if (!replace_file(temporary.path(), paths_.config_file, error)) {
        return false;
    }
    temporary.release();
    document_ = std::move(next_document);
    source_content_ = std::move(next_source);
    source_exists_ = true;
    return true;
}

bool ConfigStore::loaded() const {
    return loaded_;
}

const ConfigDocument& ConfigStore::document() const {
    return document_;
}

const AppPaths& ConfigStore::paths() const {
    return paths_;
}

} // namespace ccs
