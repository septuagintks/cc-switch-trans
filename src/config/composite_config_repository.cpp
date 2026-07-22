#include "config/composite_config_repository.hpp"

#include "config/config_editing_service.hpp"
#include "config/configuration_conversion.hpp"
#include "core/sha256.hpp"
#include "core/version.hpp"
#include "storage/sqlite_profile_store.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ccs {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kJournalSchema =
    "ccs-trans.repository-transaction/v1";
constexpr std::string_view kMigrationManifestSchema =
    "ccs-trans.migration/v1";
constexpr std::string_view kDatabaseBackupManifestSchema =
    "ccs-trans.database-replacement-backup/v1";
constexpr std::string_view kDatabaseBackupDirectoryPrefix = "replaced-db-";

class RepositoryError final : public std::runtime_error {
public:
    RepositoryError(ConfigRepositoryFailure failure, std::string message)
        : std::runtime_error(std::move(message))
        , failure_(failure) {}

    ConfigRepositoryFailure failure() const noexcept {
        return failure_;
    }

private:
    ConfigRepositoryFailure failure_;
};

[[noreturn]] void fail(ConfigRepositoryFailure failure, std::string message) {
    throw RepositoryError(failure, std::move(message));
}

bool valid_database_backup_directory_name(std::string_view name) {
    return name.size() == kDatabaseBackupDirectoryPrefix.size() + 64
        && name.starts_with(kDatabaseBackupDirectoryPrefix)
        && std::all_of(
            name.begin()
                + static_cast<std::ptrdiff_t>(kDatabaseBackupDirectoryPrefix.size()),
            name.end(),
            [](char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            });
}

ConfigRepositoryFailure map_profile_failure(ProfileStoreFailure failure) {
    switch (failure) {
    case ProfileStoreFailure::None:
        return ConfigRepositoryFailure::None;
    case ProfileStoreFailure::NotFound:
        return ConfigRepositoryFailure::RecoveryRequired;
    case ProfileStoreFailure::Busy:
        return ConfigRepositoryFailure::Busy;
    case ProfileStoreFailure::Stale:
        return ConfigRepositoryFailure::Stale;
    case ProfileStoreFailure::Constraint:
        return ConfigRepositoryFailure::Constraint;
    case ProfileStoreFailure::InvalidData:
        return ConfigRepositoryFailure::InvalidDocument;
    case ProfileStoreFailure::Corrupt:
        return ConfigRepositoryFailure::Corrupt;
    case ProfileStoreFailure::UnsupportedSchema:
        return ConfigRepositoryFailure::UnsupportedSchema;
    case ProfileStoreFailure::Io:
        return ConfigRepositoryFailure::Io;
    }
    return ConfigRepositoryFailure::Io;
}

std::filesystem::path temporary_path(const std::filesystem::path& target) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto process_id = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto process_id = static_cast<unsigned long long>(getpid());
#endif
    return target.parent_path()
        / (target.filename().string() + ".tmp-" + std::to_string(process_id)
            + "-" + std::to_string(nonce));
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        fail(ConfigRepositoryFailure::Io, "failed to inspect path: " + ec.message());
    }
    return exists;
}

void require_regular_file(const std::filesystem::path& path, std::string_view label) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status)) {
        fail(
            ConfigRepositoryFailure::Io,
            std::string(label) + " must be a regular file");
    }
}

std::string read_bounded_file(
    const std::filesystem::path& path,
    std::size_t maximum,
    std::string_view label) {
    require_regular_file(path, label);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        fail(ConfigRepositoryFailure::Io, "failed to inspect " + std::string(label));
    }
    if (size > maximum) {
        fail(
            ConfigRepositoryFailure::InvalidDocument,
            std::string(label) + " exceeds the supported size limit");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(ConfigRepositoryFailure::Io, "failed to open " + std::string(label));
    }
    std::string content;
    content.reserve(static_cast<std::size_t>(size));
    std::array<char, 8192> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            content.append(buffer.data(), static_cast<std::size_t>(count));
            if (content.size() > maximum) {
                fail(
                    ConfigRepositoryFailure::InvalidDocument,
                    std::string(label) + " exceeds the supported size limit");
            }
        }
    }
    if (!input.eof()) {
        fail(ConfigRepositoryFailure::Io, "failed to read " + std::string(label));
    }
    return content;
}

ApplicationSourceToken read_config_source(const AppPaths& paths) {
    ApplicationSourceToken source;
    source.exists = path_exists(paths.config_file);
    if (source.exists) {
        source.bytes = read_bounded_file(
            paths.config_file, kMaxConfigDocumentBytes, "config file");
    }
    return source;
}

void sync_directory(const std::filesystem::path& path) {
#ifndef _WIN32
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to open directory for sync: " + std::string(std::strerror(errno)));
    }
    const int result = fsync(descriptor);
    const int saved_errno = errno;
    close(descriptor);
    if (result != 0) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to sync directory: " + std::string(std::strerror(saved_errno)));
    }
#else
    (void)path;
#endif
}

void write_durable_file(const std::filesystem::path& path, std::string_view content) {
#ifdef _WIN32
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to create durable file: Windows error "
                + std::to_string(GetLastError()));
    }
    bool ok = true;
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto remaining = content.size() - offset;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, content.data() + offset, chunk, &written, nullptr)
            || written != chunk) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(file)) {
        ok = false;
    }
    const auto code = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to write durable file: Windows error " + std::to_string(code));
    }
#else
    const int file = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (file < 0) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to create durable file: " + std::string(std::strerror(errno)));
    }
    bool ok = true;
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto written = write(file, content.data() + offset, content.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (ok && fsync(file) != 0) {
        ok = false;
    }
    const int saved_errno = errno;
    if (close(file) != 0) {
        ok = false;
    }
    if (!ok) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to write durable file: " + std::string(std::strerror(saved_errno)));
    }
#endif
}

void mark_file_writable(const std::filesystem::path& path);

void copy_durable_file(
    const std::filesystem::path& source,
    const std::filesystem::path& target) {
    require_regular_file(source, "database backup source");
#ifdef _WIN32
    if (!CopyFileW(source.c_str(), target.c_str(), TRUE)) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to copy database backup: Windows error "
                + std::to_string(GetLastError()));
    }
    mark_file_writable(target);
    const HANDLE file = CreateFileW(
        target.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to open database backup for flush: Windows error "
                + std::to_string(GetLastError()));
    }
    const bool flushed = FlushFileBuffers(file) != FALSE;
    const auto code = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!flushed) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to flush database backup: Windows error " + std::to_string(code));
    }
#else
    const int input = open(source.c_str(), O_RDONLY);
    if (input < 0) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to open database backup source: "
                + std::string(std::strerror(errno)));
    }
    const int output = open(target.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (output < 0) {
        const int code = errno;
        close(input);
        fail(
            ConfigRepositoryFailure::Io,
            "failed to create database backup: " + std::string(std::strerror(code)));
    }
    std::array<char, 64 * 1024> buffer{};
    bool ok = true;
    int saved_errno = 0;
    while (ok) {
        const auto read_count = read(input, buffer.data(), buffer.size());
        if (read_count == 0) {
            break;
        }
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            saved_errno = errno;
            break;
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(read_count)) {
            const auto written = write(
                output,
                buffer.data() + offset,
                static_cast<std::size_t>(read_count) - offset);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ok = false;
                saved_errno = errno;
                break;
            }
            offset += static_cast<std::size_t>(written);
        }
    }
    if (ok && fsync(output) != 0) {
        ok = false;
        saved_errno = errno;
    }
    if (close(input) != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    if (close(output) != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    if (!ok) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to copy database backup: " + std::string(std::strerror(saved_errno)));
    }
#endif
}

void replace_file(
    const std::filesystem::path& source,
    const std::filesystem::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(
            source.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to atomically replace file: Windows error "
                + std::to_string(GetLastError()));
    }
#else
    if (std::rename(source.c_str(), target.c_str()) != 0) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to atomically replace file: " + std::string(std::strerror(errno)));
    }
    sync_directory(target.parent_path());
#endif
}

void move_file_no_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& target) {
    if (path_exists(target)) {
        fail(ConfigRepositoryFailure::Constraint, "target file already exists: " + target.string());
    }
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to move file without replacement: Windows error "
                + std::to_string(GetLastError()));
    }
#else
    if (link(source.c_str(), target.c_str()) != 0 || unlink(source.c_str()) != 0) {
        const int code = errno;
        std::error_code cleanup_error;
        std::filesystem::remove(target, cleanup_error);
        fail(
            ConfigRepositoryFailure::Io,
            "failed to move file without replacement: "
                + std::string(std::strerror(code)));
    }
    sync_directory(target.parent_path());
#endif
}

void move_directory_no_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& target) {
    if (path_exists(target)) {
        fail(
            ConfigRepositoryFailure::RecoveryRequired,
            "repository transaction directory already exists");
    }
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to publish repository transaction: Windows error "
                + std::to_string(GetLastError()));
    }
#else
    if (std::rename(source.c_str(), target.c_str()) != 0) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to publish repository transaction: "
                + std::string(std::strerror(errno)));
    }
    sync_directory(target.parent_path());
#endif
}

void write_atomic_file(const std::filesystem::path& target, std::string_view content) {
    const auto temporary = temporary_path(target);
    try {
        write_durable_file(temporary, content);
        replace_file(temporary, target);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        throw;
    }
}

void remove_file_durable(const std::filesystem::path& path) {
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    if (ec) {
        fail(ConfigRepositoryFailure::Io, "failed to remove file: " + ec.message());
    }
    if (removed) {
        sync_directory(path.parent_path());
    }
}

void remove_database_family(const std::filesystem::path& database_path) {
    for (const auto& suffix : {"", "-wal", "-shm"}) {
        auto path = database_path;
        path += suffix;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            fail(ConfigRepositoryFailure::Io, "failed to remove temporary database: " + ec.message());
        }
    }
    sync_directory(database_path.parent_path());
}

void mark_file_read_only(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || !SetFileAttributesW(path.c_str(), attributes | FILE_ATTRIBUTE_READONLY)) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to mark migration backup read-only: Windows error "
                + std::to_string(GetLastError()));
    }
#else
    if (chmod(path.c_str(), S_IRUSR) != 0) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to mark migration backup read-only: "
                + std::string(std::strerror(errno)));
    }
#endif
}

void mark_file_writable(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || !SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY)) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to make temporary database writable: Windows error "
                + std::to_string(GetLastError()));
    }
#else
    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        fail(
            ConfigRepositoryFailure::Io,
            "failed to make temporary database writable: "
                + std::string(std::strerror(errno)));
    }
#endif
}

class RepositoryLock final {
public:
    explicit RepositoryLock(const std::filesystem::path& path) {
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
            fail(
                code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION
                    ? ConfigRepositoryFailure::Busy
                    : ConfigRepositoryFailure::Io,
                "failed to acquire repository lock: Windows error " + std::to_string(code));
        }
#else
        descriptor_ = open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (descriptor_ < 0) {
            fail(
                ConfigRepositoryFailure::Io,
                "failed to open repository lock: " + std::string(std::strerror(errno)));
        }
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int code = errno;
            close(descriptor_);
            descriptor_ = -1;
            fail(
                code == EWOULDBLOCK ? ConfigRepositoryFailure::Busy
                                    : ConfigRepositoryFailure::Io,
                "failed to acquire repository lock: " + std::string(std::strerror(code)));
        }
#endif
    }

    ~RepositoryLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            flock(descriptor_, LOCK_UN);
            close(descriptor_);
        }
#endif
    }

    RepositoryLock(const RepositoryLock&) = delete;
    RepositoryLock& operator=(const RepositoryLock&) = delete;

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

struct DatabaseState {
    bool exists = false;
    ProfileStoreSnapshot snapshot;
    std::optional<std::string> file_sha256;
};

DatabaseState read_database_state(const AppPaths& paths, bool include_hash = false) {
    DatabaseState state;
    state.exists = path_exists(paths.profiles_database);
    if (!state.exists) {
        for (const auto& suffix : {"-wal", "-shm"}) {
            auto sidecar = paths.profiles_database;
            sidecar += suffix;
            if (path_exists(sidecar)) {
                fail(
                    ConfigRepositoryFailure::RecoveryRequired,
                    "SQLite sidecar exists without profiles.db");
            }
        }
        return state;
    }
    SqliteProfileStore store(paths.profiles_database);
    std::string error;
    if (!store.load(state.snapshot, error)) {
        fail(map_profile_failure(store.last_failure()), error);
    }
    if (include_hash) {
        state.file_sha256.emplace();
        if (!sha256_file_hex(paths.profiles_database, *state.file_sha256, error)) {
            fail(ConfigRepositoryFailure::Io, error);
        }
    }
    return state;
}

struct RepositoryJournal {
    std::string kind;
    ApplicationSourceToken old_config;
    std::string new_config;
    bool old_database_exists = false;
    ProfileRevision old_profile_revision = 0;
    ProfileRevision target_profile_revision = 0;
    std::optional<std::string> old_migration_hash;
    std::optional<std::string> target_migration_hash;
    std::optional<std::string> old_database_sha256;
    std::optional<std::string> target_database_sha256;
    std::optional<std::string> old_database_backup_directory;
};

Json optional_json(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

void write_journal(const AppPaths& paths, const RepositoryJournal& journal) {
    if (path_exists(paths.repository_transaction_directory)) {
        fail(
            ConfigRepositoryFailure::RecoveryRequired,
            "unfinished repository transaction requires recovery");
    }
    const auto temporary = temporary_path(paths.repository_transaction_directory);
    std::error_code ec;
    std::filesystem::create_directory(temporary, ec);
    if (ec) {
        fail(ConfigRepositoryFailure::Io, "failed to create transaction directory: " + ec.message());
    }
    try {
        if (journal.old_config.exists) {
            write_durable_file(temporary / "old-config.bin", journal.old_config.bytes);
        }
        write_durable_file(temporary / "new-config.json", journal.new_config);
        const Json manifest = {
            {"schema_version", kJournalSchema},
            {"kind", journal.kind},
            {"old_config_exists", journal.old_config.exists},
            {"old_config_sha256", journal.old_config.exists
                    ? Json(sha256_hex(journal.old_config.bytes)) : Json(nullptr)},
            {"new_config_sha256", sha256_hex(journal.new_config)},
            {"old_database_exists", journal.old_database_exists},
            {"old_profile_revision", journal.old_profile_revision},
            {"target_profile_revision", journal.target_profile_revision},
            {"old_migration_hash", optional_json(journal.old_migration_hash)},
            {"target_migration_hash", optional_json(journal.target_migration_hash)},
            {"old_database_sha256", optional_json(journal.old_database_sha256)},
            {"target_database_sha256", optional_json(journal.target_database_sha256)},
            {"old_database_backup_directory",
                optional_json(journal.old_database_backup_directory)},
        };
        write_durable_file(temporary / "manifest.json", manifest.dump(2) + "\n");
        sync_directory(temporary);
        move_directory_no_replace(temporary, paths.repository_transaction_directory);
    } catch (...) {
        std::filesystem::remove_all(temporary, ec);
        throw;
    }
}

std::optional<std::string> read_optional_string(const Json& root, const char* key) {
    if (!root.contains(key)) {
        fail(ConfigRepositoryFailure::RecoveryRequired, "journal is missing field: " + std::string(key));
    }
    if (root.at(key).is_null()) {
        return std::nullopt;
    }
    if (!root.at(key).is_string()) {
        fail(ConfigRepositoryFailure::RecoveryRequired, "journal field has wrong type: " + std::string(key));
    }
    return root.at(key).get<std::string>();
}

std::optional<std::string> read_optional_string_if_present(
    const Json& root,
    const char* key) {
    if (!root.contains(key) || root.at(key).is_null()) {
        return std::nullopt;
    }
    if (!root.at(key).is_string()) {
        fail(ConfigRepositoryFailure::RecoveryRequired,
            "journal field has wrong type: " + std::string(key));
    }
    return root.at(key).get<std::string>();
}

RepositoryJournal read_journal(const AppPaths& paths) {
    const auto& directory = paths.repository_transaction_directory;
    const auto manifest_bytes = read_bounded_file(
        directory / "manifest.json", 1024 * 1024, "repository transaction manifest");
    Json manifest;
    try {
        manifest = Json::parse(manifest_bytes);
    } catch (const Json::exception& exception) {
        fail(
            ConfigRepositoryFailure::RecoveryRequired,
            "failed to parse repository transaction manifest: "
                + std::string(exception.what()));
    }
    if (!manifest.is_object()
        || manifest.value("schema_version", "") != kJournalSchema
        || !manifest.contains("kind")
        || !manifest.at("kind").is_string()
        || !manifest.contains("old_config_exists")
        || !manifest.at("old_config_exists").is_boolean()
        || !manifest.contains("old_database_exists")
        || !manifest.at("old_database_exists").is_boolean()
        || !manifest.contains("old_profile_revision")
        || !manifest.at("old_profile_revision").is_number_integer()
        || !manifest.contains("target_profile_revision")
        || !manifest.at("target_profile_revision").is_number_integer()) {
        fail(ConfigRepositoryFailure::RecoveryRequired, "repository transaction manifest is invalid");
    }

    RepositoryJournal journal;
    journal.kind = manifest.at("kind").get<std::string>();
    journal.old_config.exists = manifest.at("old_config_exists").get<bool>();
    journal.old_database_exists = manifest.at("old_database_exists").get<bool>();
    journal.old_profile_revision = manifest.at("old_profile_revision").get<ProfileRevision>();
    journal.target_profile_revision = manifest.at("target_profile_revision").get<ProfileRevision>();
    journal.old_migration_hash = read_optional_string(manifest, "old_migration_hash");
    journal.target_migration_hash = read_optional_string(manifest, "target_migration_hash");
    journal.old_database_sha256 = read_optional_string_if_present(
        manifest, "old_database_sha256");
    journal.target_database_sha256 = read_optional_string_if_present(
        manifest, "target_database_sha256");
    journal.old_database_backup_directory = read_optional_string_if_present(
        manifest, "old_database_backup_directory");
    if (journal.old_database_backup_directory) {
        if (!valid_database_backup_directory_name(
                *journal.old_database_backup_directory)
            || journal.kind != "migration"
            || !journal.old_database_exists
            || !journal.old_database_sha256) {
            fail(
                ConfigRepositoryFailure::RecoveryRequired,
                "journal database backup reference is invalid");
        }
    }
    if (journal.old_config.exists) {
        journal.old_config.bytes = read_bounded_file(
            directory / "old-config.bin", kMaxConfigDocumentBytes, "journal old config");
    }
    journal.new_config = read_bounded_file(
        directory / "new-config.json", kMaxConfigDocumentBytes, "journal new config");

    const auto old_hash = read_optional_string(manifest, "old_config_sha256");
    if (journal.old_config.exists) {
        if (!old_hash || *old_hash != sha256_hex(journal.old_config.bytes)) {
            fail(ConfigRepositoryFailure::RecoveryRequired, "journal old config hash mismatch");
        }
    } else if (old_hash) {
        fail(ConfigRepositoryFailure::RecoveryRequired, "journal has hash for absent old config");
    }
    if (!manifest.contains("new_config_sha256")
        || !manifest.at("new_config_sha256").is_string()
        || manifest.at("new_config_sha256").get<std::string>()
            != sha256_hex(journal.new_config)) {
        fail(ConfigRepositoryFailure::RecoveryRequired, "journal new config hash mismatch");
    }
    return journal;
}

void clear_journal(const AppPaths& paths, std::string_view kind = {}) {
    if (kind == "migration") {
        auto temporary_database = paths.profiles_database;
        temporary_database += ".migrating";
        remove_database_family(temporary_database);
    }
    std::error_code ec;
    std::filesystem::remove_all(paths.repository_transaction_directory, ec);
    if (ec) {
        fail(ConfigRepositoryFailure::Io, "failed to clear transaction journal: " + ec.message());
    }
    sync_directory(paths.state_directory);
}

bool config_matches(
    const ApplicationSourceToken& actual,
    const ApplicationSourceToken& expected) {
    return actual == expected;
}

bool database_matches(
    const DatabaseState& actual,
    bool expected_exists,
    ProfileRevision expected_revision,
    const std::optional<std::string>& expected_hash,
    const std::optional<std::string>& expected_file_hash = std::nullopt) {
    return actual.exists == expected_exists
        && (!expected_exists
            || (actual.snapshot.revision == expected_revision
                && actual.snapshot.migrated_from_sha256 == expected_hash
                && (!expected_file_hash
                    || actual.file_sha256 == expected_file_hash)));
}

bool recoverable_replacement_database_failure(ConfigRepositoryFailure failure) {
    switch (failure) {
    case ConfigRepositoryFailure::InvalidDocument:
    case ConfigRepositoryFailure::Corrupt:
    case ConfigRepositoryFailure::UnsupportedSchema:
    case ConfigRepositoryFailure::RecoveryRequired:
        return true;
    default:
        return false;
    }
}

void restore_config(const AppPaths& paths, const ApplicationSourceToken& source) {
    if (source.exists) {
        write_atomic_file(paths.config_file, source.bytes);
    } else if (path_exists(paths.config_file)) {
        remove_file_durable(paths.config_file);
    }
}

void write_migration_backup(
    const AppPaths& paths,
    std::string_view source_bytes,
    std::string_view source_hash) {
    const auto target = paths.migrations_directory / std::string(source_hash);
    if (path_exists(target)) {
        const auto existing = read_bounded_file(
            target / "config-v2.json", kMaxConfigDocumentBytes, "migration backup");
        if (existing != source_bytes) {
            fail(
                ConfigRepositoryFailure::Constraint,
                "migration backup directory contains different source bytes");
        }
        const auto manifest_bytes = read_bounded_file(
            target / "manifest.json", 1024 * 1024, "migration backup manifest");
        try {
            const auto manifest = Json::parse(manifest_bytes);
            if (!manifest.is_object()
                || manifest.value("schema_version", "") != kMigrationManifestSchema
                || manifest.value("source_schema", "") != "ccs-trans.config/v2"
                || manifest.value("source_sha256", "") != source_hash) {
                fail(
                    ConfigRepositoryFailure::Constraint,
                    "migration backup manifest does not match the v2 source");
            }
        } catch (const Json::exception& exception) {
            fail(
                ConfigRepositoryFailure::Constraint,
                "failed to validate migration backup manifest: "
                    + std::string(exception.what()));
        }
        mark_file_read_only(target / "config-v2.json");
        mark_file_read_only(target / "manifest.json");
        return;
    }

    const auto temporary = temporary_path(target);
    std::error_code ec;
    std::filesystem::create_directories(temporary, ec);
    if (ec) {
        fail(ConfigRepositoryFailure::Io, "failed to create migration backup: " + ec.message());
    }
    try {
        write_durable_file(temporary / "config-v2.json", source_bytes);
        const Json manifest = {
            {"schema_version", kMigrationManifestSchema},
            {"source_schema", "ccs-trans.config/v2"},
            {"source_sha256", source_hash},
            {"source_bytes", source_bytes.size()},
            {"ccs_trans_version", kVersion},
            {"source_commit", kSourceCommit},
            {"source_dirty", kSourceDirty},
        };
        write_durable_file(temporary / "manifest.json", manifest.dump(2) + "\n");
        sync_directory(temporary);
        move_directory_no_replace(temporary, target);
        mark_file_read_only(target / "config-v2.json");
        mark_file_read_only(target / "manifest.json");
    } catch (...) {
        std::filesystem::remove_all(temporary, ec);
        throw;
    }
}

void require_plain_directory(const std::filesystem::path& path, std::string_view label) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_directory(status)
        || std::filesystem::is_symlink(status)) {
        fail(
            ConfigRepositoryFailure::Io,
            std::string(label) + " must be a non-symlink directory");
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        fail(
            ConfigRepositoryFailure::Io,
            std::string(label) + " must not be a reparse point");
    }
#endif
}

struct DatabaseSidecarState {
    bool wal_exists = false;
    std::uintmax_t wal_bytes = 0;
    bool shm_exists = false;
    std::uintmax_t shm_bytes = 0;
};

std::pair<bool, std::uintmax_t> inspect_sidecar(
    const std::filesystem::path& database,
    std::string_view suffix) {
    auto path = database;
    path += suffix;
    if (!path_exists(path)) {
        return {false, 0};
    }
    require_regular_file(path, "SQLite sidecar");
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec) {
        fail(ConfigRepositoryFailure::Io, "failed to inspect SQLite sidecar size");
    }
    return {true, bytes};
}

DatabaseSidecarState inspect_database_sidecars(
    const std::filesystem::path& database) {
    const auto wal = inspect_sidecar(database, "-wal");
    const auto shm = inspect_sidecar(database, "-shm");
    return {wal.first, wal.second, shm.first, shm.second};
}

Json sidecar_json(const DatabaseSidecarState& state) {
    return {
        {"wal", {{"exists", state.wal_exists}, {"bytes", state.wal_bytes}}},
        {"shm", {{"exists", state.shm_exists}, {"bytes", state.shm_bytes}}},
    };
}

bool optional_manifest_value_matches(
    const Json& manifest,
    std::string_view key,
    const std::optional<std::string>& expected) {
    if (!manifest.contains(key)) {
        return false;
    }
    const auto& value = manifest.at(key);
    return expected
        ? value.is_string() && value.get<std::string>() == *expected
        : value.is_null();
}

struct DatabaseReplacementBackup {
    std::filesystem::path path;
    std::string database_sha256;
};

ProfileStoreSnapshot verify_database_backup_bytes(
    const AppPaths& paths,
    const std::filesystem::path& database_path,
    std::string_view expected_hash,
    const ProfileStoreSnapshot* expected_snapshot,
    ConfigRepositoryFailure failure) {
    std::string actual_hash;
    std::string error;
    if (!sha256_file_hex(database_path, actual_hash, error)
        || actual_hash != expected_hash) {
        fail(
            failure,
            error.empty() ? "database backup hash mismatch" : std::move(error));
    }

    const auto verification_database = temporary_path(paths.profiles_database);
    ProfileStoreSnapshot actual_snapshot;
    try {
        copy_durable_file(database_path, verification_database);
        {
            SqliteProfileStore store(verification_database);
            if (!store.load(actual_snapshot, error)) {
                fail(
                    failure,
                    error.empty()
                        ? "database backup semantic verification failed"
                        : std::move(error));
            }
        }
        remove_database_family(verification_database);
        if (expected_snapshot != nullptr && actual_snapshot != *expected_snapshot) {
            fail(failure, "database backup semantic snapshot mismatch");
        }
    } catch (...) {
        std::error_code cleanup_error;
        for (const auto& suffix : {"", "-wal", "-shm"}) {
            auto cleanup = verification_database;
            cleanup += suffix;
            std::filesystem::remove(cleanup, cleanup_error);
            cleanup_error.clear();
        }
        throw;
    }
    return actual_snapshot;
}

ProfileStoreSnapshot validate_database_replacement_backup(
    const AppPaths& paths,
    std::string_view directory_name,
    std::string_view source_hash,
    std::string_view database_hash,
    ProfileRevision expected_revision,
    const std::optional<std::string>& expected_migration_hash,
    const ProfileStoreSnapshot* expected_snapshot,
    ConfigRepositoryFailure failure) {
    if (!valid_database_backup_directory_name(directory_name)) {
        fail(failure, "database backup directory name is invalid");
    }
    const auto target = paths.migrations_directory / std::string(directory_name);
    require_plain_directory(target, "database backup directory");
    const auto database_path = target / "profiles.db";
    require_regular_file(database_path, "database backup");
    for (const auto& suffix : {"-wal", "-shm"}) {
        auto sidecar = database_path;
        sidecar += suffix;
        if (path_exists(sidecar)) {
            fail(failure, "database backup must not contain SQLite sidecars");
        }
    }

    const auto manifest_bytes = read_bounded_file(
        target / "manifest.json", 1024 * 1024, "database backup manifest");
    std::uintmax_t database_bytes = 0;
    try {
        const auto manifest = Json::parse(manifest_bytes);
        if (!manifest.is_object()
            || manifest.value("schema_version", "")
                != kDatabaseBackupManifestSchema
            || manifest.value("source_config_sha256", "") != source_hash
            || manifest.value("database_sha256", "") != database_hash
            || !manifest.contains("database_bytes")
            || !manifest.at("database_bytes").is_number_unsigned()
            || manifest.value("profile_revision", ProfileRevision{-1})
                != expected_revision
            || !optional_manifest_value_matches(
                manifest,
                "migrated_from_sha256",
                expected_migration_hash)) {
            fail(failure, "database backup manifest does not match the replacement source");
        }
        database_bytes = manifest.at("database_bytes").get<std::uintmax_t>();
    } catch (const Json::exception& exception) {
        fail(
            failure,
            "failed to validate database backup manifest: "
                + std::string(exception.what()));
    }

    std::error_code size_error;
    if (std::filesystem::file_size(database_path, size_error) != database_bytes
        || size_error) {
        fail(failure, "database backup size mismatch");
    }
    auto actual_snapshot = verify_database_backup_bytes(
        paths, database_path, database_hash, expected_snapshot, failure);
    if (actual_snapshot.revision != expected_revision
        || actual_snapshot.migrated_from_sha256 != expected_migration_hash) {
        fail(failure, "database backup metadata does not match its manifest");
    }
    return actual_snapshot;
}

DatabaseReplacementBackup write_database_replacement_backup(
    const AppPaths& paths,
    std::string_view source_hash,
    const DatabaseState& database,
    const DatabaseSidecarState& sidecars_before,
    const DatabaseSidecarState& sidecars_after) {
    std::string database_hash;
    std::string hash_error;
    if (!sha256_file_hex(paths.profiles_database, database_hash, hash_error)) {
        fail(ConfigRepositoryFailure::Io, std::move(hash_error));
    }
    std::error_code size_error;
    const auto database_bytes = std::filesystem::file_size(
        paths.profiles_database, size_error);
    if (size_error) {
        fail(ConfigRepositoryFailure::Io, "failed to inspect profile database size");
    }
    std::string backup_identity(source_hash);
    backup_identity.push_back('\0');
    backup_identity.append(database_hash);
    const auto directory_name = std::string(kDatabaseBackupDirectoryPrefix)
        + sha256_hex(backup_identity);
    const auto target = paths.migrations_directory / directory_name;

    if (path_exists(target)) {
        validate_database_replacement_backup(
            paths,
            directory_name,
            source_hash,
            database_hash,
            database.snapshot.revision,
            database.snapshot.migrated_from_sha256,
            &database.snapshot,
            ConfigRepositoryFailure::Constraint);
        mark_file_read_only(target / "profiles.db");
        mark_file_read_only(target / "manifest.json");
        return {target, database_hash};
    }

    const auto temporary = temporary_path(target);
    std::error_code ec;
    std::filesystem::create_directory(temporary, ec);
    if (ec) {
        fail(ConfigRepositoryFailure::Io, "failed to create database backup: " + ec.message());
    }
    try {
        copy_durable_file(paths.profiles_database, temporary / "profiles.db");
        verify_database_backup_bytes(
            paths,
            temporary / "profiles.db",
            database_hash,
            &database.snapshot,
            ConfigRepositoryFailure::Io);
        const Json manifest = {
            {"schema_version", kDatabaseBackupManifestSchema},
            {"source_config_sha256", source_hash},
            {"database_sha256", database_hash},
            {"database_bytes", database_bytes},
            {"profile_revision", database.snapshot.revision},
            {"migrated_from_sha256", optional_json(database.snapshot.migrated_from_sha256)},
            {"sidecars_before_checkpoint", sidecar_json(sidecars_before)},
            {"sidecars_after_checkpoint", sidecar_json(sidecars_after)},
            {"ccs_trans_version", kVersion},
            {"source_commit", kSourceCommit},
            {"source_dirty", kSourceDirty},
        };
        write_durable_file(temporary / "manifest.json", manifest.dump(2) + "\n");
        sync_directory(temporary);
        move_directory_no_replace(temporary, target);
        validate_database_replacement_backup(
            paths,
            directory_name,
            source_hash,
            database_hash,
            database.snapshot.revision,
            database.snapshot.migrated_from_sha256,
            &database.snapshot,
            ConfigRepositoryFailure::Io);
        mark_file_read_only(target / "profiles.db");
        mark_file_read_only(target / "manifest.json");
        return {target, database_hash};
    } catch (...) {
        std::filesystem::remove_all(temporary, ec);
        throw;
    }
}

void remove_database_sidecars(const std::filesystem::path& database) {
    for (const auto& suffix : {"-wal", "-shm"}) {
        auto path = database;
        path += suffix;
        if (path_exists(path)) {
            remove_file_durable(path);
        }
    }
}

void restore_database_replacement_backup(
    const AppPaths& paths,
    const RepositoryJournal& journal) {
    if (journal.kind != "migration"
        || !journal.old_config.exists
        || !journal.old_database_exists
        || !journal.old_database_sha256
        || !journal.old_database_backup_directory) {
        fail(
            ConfigRepositoryFailure::RecoveryRequired,
            "migration journal has no usable database replacement backup");
    }

    ProfileStoreSnapshot backup_snapshot;
    try {
        backup_snapshot = validate_database_replacement_backup(
            paths,
            *journal.old_database_backup_directory,
            sha256_hex(journal.old_config.bytes),
            *journal.old_database_sha256,
            journal.old_profile_revision,
            journal.old_migration_hash,
            nullptr,
            ConfigRepositoryFailure::RecoveryRequired);
    } catch (const RepositoryError& exception) {
        fail(
            ConfigRepositoryFailure::RecoveryRequired,
            "database replacement backup cannot be used for recovery: "
                + std::string(exception.what()));
    }

    const auto backup_database = paths.migrations_directory
        / *journal.old_database_backup_directory / "profiles.db";
    const auto temporary_database = temporary_path(paths.profiles_database);
    try {
        copy_durable_file(backup_database, temporary_database);
        verify_database_backup_bytes(
            paths,
            temporary_database,
            *journal.old_database_sha256,
            &backup_snapshot,
            ConfigRepositoryFailure::RecoveryRequired);
        remove_database_sidecars(paths.profiles_database);
        replace_file(temporary_database, paths.profiles_database);
        restore_config(paths, journal.old_config);
    } catch (...) {
        std::error_code cleanup_error;
        for (const auto& suffix : {"", "-wal", "-shm"}) {
            auto cleanup = temporary_database;
            cleanup += suffix;
            std::filesystem::remove(cleanup, cleanup_error);
            cleanup_error.clear();
        }
        throw;
    }
}

std::vector<StoredProfile> merge_stable_keys(
    const std::vector<StoredProfile>& current,
    std::vector<StoredProfile> edited) {
    std::unordered_map<std::string, const StoredProfile*> current_by_id;
    std::unordered_map<ProfileKey, const StoredProfile*> current_by_key;
    for (const auto& existing : current) {
        current_by_id.emplace(existing.profile_id, &existing);
        current_by_key.emplace(existing.key, &existing);
    }

    for (auto& profile : edited) {
        const StoredProfile* existing = nullptr;
        if (profile.key > 0) {
            const auto found = current_by_key.find(profile.key);
            if (found != current_by_key.end()) {
                existing = found->second;
            }
        } else {
            const auto found = current_by_id.find(profile.profile_id);
            if (found != current_by_id.end()) {
                existing = found->second;
            }
        }
        if (existing == nullptr) {
            continue;
        }
        profile.key = existing->key;
        std::unordered_map<std::string, RuleKey> rule_keys;
        std::unordered_map<RuleKey, std::string_view> rule_ids_by_key;
        for (const auto& rule : existing->rules) {
            rule_keys.emplace(rule.rule_id, rule.key);
            rule_ids_by_key.emplace(rule.key, rule.rule_id);
        }
        for (auto& rule : profile.rules) {
            if (rule.key > 0) {
                const auto existing_id = rule_ids_by_key.find(rule.key);
                if (existing_id == rule_ids_by_key.end()
                    || existing_id->second != rule.rule_id) {
                    rule.key = 0;
                }
            } else {
                const auto key = rule_keys.find(rule.rule_id);
                if (key != rule_keys.end()) {
                    rule.key = key->second;
                }
            }
        }
    }
    return edited;
}

} // namespace

const char* storage_state_name(StorageState state) noexcept {
    switch (state) {
    case StorageState::Uninitialized:
        return "uninitialized";
    case StorageState::MigrationRequired:
        return "migration_required";
    case StorageState::Ready:
        return "ready";
    case StorageState::RecoveryRequired:
        return "recovery_required";
    case StorageState::Invalid:
        return "invalid";
    }
    return "unknown";
}

CompositeConfigRepository::CompositeConfigRepository(AppPaths paths)
    : paths_(std::move(paths)) {}

bool CompositeConfigRepository::recover_locked(std::string& error) {
    error.clear();
    if (!path_exists(paths_.repository_transaction_directory)) {
        return true;
    }
    try {
        const auto journal = read_journal(paths_);
        const auto actual_config = read_config_source(paths_);
        DatabaseState actual_database;
        try {
            actual_database = read_database_state(paths_, true);
        } catch (const RepositoryError& exception) {
            if (!journal.old_database_backup_directory
                || !recoverable_replacement_database_failure(exception.failure())) {
                throw;
            }
            restore_database_replacement_backup(paths_, journal);
            clear_journal(paths_, journal.kind);
            return true;
        }
        const bool config_is_old = config_matches(actual_config, journal.old_config);
        const bool config_is_new = actual_config.exists
            && actual_config.bytes == journal.new_config;
        const bool database_is_old = database_matches(
            actual_database,
            journal.old_database_exists,
            journal.old_profile_revision,
            journal.old_migration_hash,
            journal.old_database_sha256);
        const bool database_is_target = database_matches(
            actual_database,
            true,
            journal.target_profile_revision,
            journal.target_migration_hash,
            journal.target_database_sha256);

        if (config_is_old && database_is_old) {
            clear_journal(paths_, journal.kind);
            return true;
        }
        if (config_is_new && database_is_old) {
            restore_config(paths_, journal.old_config);
            clear_journal(paths_, journal.kind);
            return true;
        }
        if (config_is_old && database_is_target) {
            write_atomic_file(paths_.config_file, journal.new_config);
            clear_journal(paths_, journal.kind);
            return true;
        }
        if (config_is_new && database_is_target) {
            clear_journal(paths_, journal.kind);
            return true;
        }
        if (journal.old_database_backup_directory) {
            restore_database_replacement_backup(paths_, journal);
            clear_journal(paths_, journal.kind);
            return true;
        }
        fail(
            ConfigRepositoryFailure::RecoveryRequired,
            "repository state does not match transaction old or target values");
    } catch (const RepositoryError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
        return false;
    } catch (const std::exception& exception) {
        last_failure_ = ConfigRepositoryFailure::RecoveryRequired;
        error = std::string("repository recovery failed: ") + exception.what();
        return false;
    }
}

bool CompositeConfigRepository::initialize_fresh_locked(std::string& error) {
    ApplicationConfigDocument application;
    std::string new_config;
    if (!serialize_application_config_document(application, new_config, error)) {
        last_failure_ = ConfigRepositoryFailure::InvalidDocument;
        return false;
    }
    RepositoryJournal journal;
    journal.kind = "initialize";
    journal.new_config = new_config;
    journal.old_database_exists = false;
    journal.target_profile_revision = 0;
    try {
        write_journal(paths_, journal);
        write_atomic_file(paths_.config_file, new_config);
        SqliteProfileStore store(paths_.profiles_database);
        ProfileStoreSnapshot profiles;
        if (!store.open_or_create(profiles, error)) {
            remove_database_family(paths_.profiles_database);
            restore_config(paths_, journal.old_config);
            clear_journal(paths_);
            last_failure_ = map_profile_failure(store.last_failure());
            return false;
        }
        if (profiles.revision != 0 || !profiles.profiles.empty()) {
            fail(ConfigRepositoryFailure::Corrupt, "fresh profile database is not empty");
        }
        clear_journal(paths_);
        return true;
    } catch (const RepositoryError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
    }
    return false;
}

bool CompositeConfigRepository::update_legacy_document(std::string& error) {
    ConfigDocument converted;
    if (!configuration_snapshot_to_config_document(snapshot_, converted, error)) {
        return false;
    }
    legacy_document_ = std::move(converted);
    return true;
}

bool CompositeConfigRepository::load_ready_locked(std::string& error) {
    const auto source = read_config_source(paths_);
    if (!source.exists) {
        fail(ConfigRepositoryFailure::RecoveryRequired, "config.json is missing while profiles.db exists");
    }
    ConfigSchemaKind schema = ConfigSchemaKind::Unsupported;
    if (!detect_config_schema(source.bytes, schema, error)) {
        fail(ConfigRepositoryFailure::InvalidDocument, error);
    }
    if (schema == ConfigSchemaKind::V2) {
        fail(
            ConfigRepositoryFailure::MigrationRequired,
            "ccs-trans.config/v2 requires explicit 'ccs-trans storage migrate'");
    }
    if (schema != ConfigSchemaKind::V3) {
        fail(ConfigRepositoryFailure::UnsupportedSchema, "unsupported config schema_version");
    }
    ApplicationConfigDocument application;
    if (!parse_application_config_document(source.bytes, application, error)) {
        fail(ConfigRepositoryFailure::InvalidDocument, error);
    }
    const auto database = read_database_state(paths_);
    if (!database.exists) {
        fail(ConfigRepositoryFailure::RecoveryRequired, "profiles.db is missing for v3 config");
    }

    snapshot_.application = application.application;
    snapshot_.profiles = database.snapshot.profiles;
    snapshot_.revision.application_source = source;
    snapshot_.revision.profile_revision = database.snapshot.revision;
    snapshot_.migrated_from_sha256 = database.snapshot.migrated_from_sha256;
    if (!update_legacy_document(error)) {
        fail(ConfigRepositoryFailure::InvalidDocument, error);
    }
    loaded_ = true;
    return true;
}

bool CompositeConfigRepository::load(std::string& error) {
    error.clear();
    last_failure_ = ConfigRepositoryFailure::None;
    loaded_ = false;
    try {
        if (!ensure_app_directories(paths_, error)) {
            fail(ConfigRepositoryFailure::Io, error);
        }
        RepositoryLock lock(paths_.repository_lock_file);
        if (!recover_locked(error)) {
            return false;
        }
        const auto source = read_config_source(paths_);
        const bool database_exists = path_exists(paths_.profiles_database);
        if (!source.exists && !database_exists) {
            if (!initialize_fresh_locked(error)) {
                return false;
            }
        }
        return load_ready_locked(error);
    } catch (const RepositoryError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
    } catch (const std::exception& exception) {
        last_failure_ = ConfigRepositoryFailure::Io;
        error = std::string("failed to load composite repository: ") + exception.what();
    }
    return false;
}

bool CompositeConfigRepository::save_snapshot(
    const ConfigurationSnapshot& desired,
    ConfigurationSnapshot& committed,
    std::string& error) {
    error.clear();
    last_failure_ = ConfigRepositoryFailure::None;
    ApplicationConfigDocument application{desired.application};
    if (!validate_application_config_document(application, error)) {
        last_failure_ = ConfigRepositoryFailure::InvalidDocument;
        return false;
    }
    ConfigDocument validation;
    if (!configuration_snapshot_to_config_document(desired, validation, error)
        || !validate_config_candidate(validation, paths_.root, error)) {
        last_failure_ = ConfigRepositoryFailure::InvalidDocument;
        return false;
    }

    try {
        if (!ensure_app_directories(paths_, error)) {
            fail(ConfigRepositoryFailure::Io, error);
        }
        RepositoryLock lock(paths_.repository_lock_file);
        if (!recover_locked(error)) {
            return false;
        }
        if (!load_ready_locked(error)) {
            return false;
        }
        if (snapshot_.revision != desired.revision) {
            fail(ConfigRepositoryFailure::Stale, "composite repository revision changed");
        }
        if (snapshot_.migrated_from_sha256 != desired.migrated_from_sha256) {
            fail(ConfigRepositoryFailure::InvalidDocument, "migration origin is immutable");
        }

        std::string new_config;
        if (!serialize_application_config_document(application, new_config, error)) {
            fail(ConfigRepositoryFailure::InvalidDocument, error);
        }
        const bool config_changed =
            snapshot_.revision.application_source.bytes != new_config;
        const bool profiles_changed = snapshot_.profiles != desired.profiles;
        if (!config_changed && !profiles_changed) {
            committed = snapshot_;
            return true;
        }

        ProfileStoreSnapshot desired_profiles;
        desired_profiles.revision = snapshot_.revision.profile_revision;
        desired_profiles.migrated_from_sha256 = snapshot_.migrated_from_sha256;
        desired_profiles.profiles = desired.profiles;
        ProfileStoreSnapshot saved_profiles;

        if (config_changed && profiles_changed) {
            RepositoryJournal journal;
            journal.kind = "commit";
            journal.old_config = snapshot_.revision.application_source;
            journal.new_config = new_config;
            journal.old_database_exists = true;
            journal.old_profile_revision = snapshot_.revision.profile_revision;
            journal.target_profile_revision = snapshot_.revision.profile_revision + 1;
            journal.old_migration_hash = snapshot_.migrated_from_sha256;
            journal.target_migration_hash = snapshot_.migrated_from_sha256;
            write_journal(paths_, journal);
            try {
                write_atomic_file(paths_.config_file, new_config);
                SqliteProfileStore store(paths_.profiles_database);
                if (!store.save(desired_profiles, saved_profiles, error)) {
                    restore_config(paths_, journal.old_config);
                    clear_journal(paths_);
                    fail(map_profile_failure(store.last_failure()), error);
                }
                clear_journal(paths_);
            } catch (...) {
                if (path_exists(paths_.repository_transaction_directory)) {
                    std::string recovery_error;
                    if (!recover_locked(recovery_error)) {
                        fail(ConfigRepositoryFailure::RecoveryRequired, recovery_error);
                    }
                }
                throw;
            }
        } else if (config_changed) {
            write_atomic_file(paths_.config_file, new_config);
            saved_profiles.revision = snapshot_.revision.profile_revision;
            saved_profiles.migrated_from_sha256 = snapshot_.migrated_from_sha256;
            saved_profiles.profiles = snapshot_.profiles;
        } else {
            SqliteProfileStore store(paths_.profiles_database);
            if (!store.save(desired_profiles, saved_profiles, error)) {
                fail(map_profile_failure(store.last_failure()), error);
            }
            new_config = snapshot_.revision.application_source.bytes;
        }

        snapshot_.application = desired.application;
        snapshot_.profiles = saved_profiles.profiles;
        snapshot_.revision.application_source = {true, std::move(new_config)};
        snapshot_.revision.profile_revision = saved_profiles.revision;
        snapshot_.migrated_from_sha256 = saved_profiles.migrated_from_sha256;
        if (!update_legacy_document(error)) {
            fail(ConfigRepositoryFailure::Corrupt, error);
        }
        loaded_ = true;
        committed = snapshot_;
        return true;
    } catch (const RepositoryError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
    } catch (const std::exception& exception) {
        last_failure_ = ConfigRepositoryFailure::Io;
        error = std::string("failed to save composite repository: ") + exception.what();
    }
    return false;
}

bool CompositeConfigRepository::save(
    const ConfigDocument& document,
    std::string& error) {
    if (!loaded_) {
        last_failure_ = ConfigRepositoryFailure::NotLoaded;
        error = "composite repository must be loaded before saving";
        return false;
    }
    std::vector<StoredProfile> converted;
    if (!config_document_to_stored_profiles(document, converted, error)) {
        last_failure_ = ConfigRepositoryFailure::InvalidDocument;
        return false;
    }
    ConfigurationSnapshot desired = snapshot_;
    desired.application = document.application;
    desired.profiles = merge_stable_keys(snapshot_.profiles, std::move(converted));
    ConfigurationSnapshot committed;
    return save_snapshot(desired, committed, error);
}

bool CompositeConfigRepository::inspect_storage(
    StorageStatus& status,
    std::string& error) {
    error.clear();
    last_failure_ = ConfigRepositoryFailure::None;
    status = {};
    try {
        if (!ensure_app_directories(paths_, error)) {
            fail(ConfigRepositoryFailure::Io, error);
        }
        RepositoryLock lock(paths_.repository_lock_file);
        if (!recover_locked(error)) {
            status.state = StorageState::RecoveryRequired;
            status.detail = error;
            return false;
        }
        const auto source = read_config_source(paths_);
        const bool database_exists = path_exists(paths_.profiles_database);
        if (!source.exists && !database_exists) {
            status.state = StorageState::Uninitialized;
            return true;
        }
        if (!source.exists) {
            status.state = StorageState::RecoveryRequired;
            status.detail = "config.json is missing while profiles.db exists";
            return true;
        }
        ConfigSchemaKind schema = ConfigSchemaKind::Unsupported;
        if (!detect_config_schema(source.bytes, schema, error)) {
            status.state = StorageState::Invalid;
            status.detail = error;
            return true;
        }
        if (schema == ConfigSchemaKind::V2) {
            status.state = StorageState::MigrationRequired;
            status.detail = "explicit migration is required";
            return true;
        }
        if (schema != ConfigSchemaKind::V3) {
            status.state = StorageState::Invalid;
            status.detail = "unsupported config schema_version";
            return true;
        }
        if (!database_exists) {
            status.state = StorageState::RecoveryRequired;
            status.detail = "profiles.db is missing for v3 config";
            return true;
        }
        const auto database = read_database_state(paths_);
        status.state = StorageState::Ready;
        status.profile_revision = database.snapshot.revision;
        status.migrated_from_sha256 = database.snapshot.migrated_from_sha256;
        return true;
    } catch (const RepositoryError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
    } catch (const std::exception& exception) {
        last_failure_ = ConfigRepositoryFailure::Io;
        error = std::string("failed to inspect storage: ") + exception.what();
    }
    return false;
}

bool CompositeConfigRepository::migrate_v2(
    MigrationOutcome& outcome,
    std::string& error) {
    MigrationResult result;
    if (!migrate_v2({}, result, error)) {
        return false;
    }
    outcome = result.outcome;
    return true;
}

bool CompositeConfigRepository::migrate_v2(
    const MigrationOptions& options,
    MigrationResult& result,
    std::string& error) {
    error.clear();
    result = {};
    last_failure_ = ConfigRepositoryFailure::None;
    loaded_ = false;
    try {
        if (!ensure_app_directories(paths_, error)) {
            fail(ConfigRepositoryFailure::Io, error);
        }
        std::error_code directory_error;
        std::filesystem::create_directories(paths_.migrations_directory, directory_error);
        if (directory_error) {
            fail(ConfigRepositoryFailure::Io, "failed to create migration directory: " + directory_error.message());
        }
        RepositoryLock lock(paths_.repository_lock_file);
        if (!recover_locked(error)) {
            return false;
        }
        const auto source = read_config_source(paths_);
        if (!source.exists) {
            fail(ConfigRepositoryFailure::NotLoaded, "config.json does not exist");
        }
        ConfigSchemaKind schema = ConfigSchemaKind::Unsupported;
        if (!detect_config_schema(source.bytes, schema, error)) {
            fail(ConfigRepositoryFailure::InvalidDocument, error);
        }
        if (schema == ConfigSchemaKind::V3) {
            if (!load_ready_locked(error)) {
                return false;
            }
            if (!snapshot_.migrated_from_sha256) {
                fail(ConfigRepositoryFailure::Constraint, "v3 repository was not created by v2 migration");
            }
            result.outcome = MigrationOutcome::AlreadyMigrated;
            return true;
        }
        if (schema != ConfigSchemaKind::V2) {
            fail(ConfigRepositoryFailure::UnsupportedSchema, "unsupported config schema_version");
        }
        const bool replacing_database = path_exists(paths_.profiles_database);
        if (replacing_database && !options.replace_existing_database) {
            fail(
                ConfigRepositoryFailure::Constraint,
                "profiles.db already exists; refusing to overwrite it during migration");
        }

        ConfigDocument legacy;
        if (!parse_config_document(source.bytes, legacy, error)
            || !validate_config_candidate(legacy, paths_.root, error)) {
            fail(ConfigRepositoryFailure::InvalidDocument, error);
        }
        std::vector<StoredProfile> profiles;
        if (!config_document_to_stored_profiles(legacy, profiles, error)) {
            fail(ConfigRepositoryFailure::InvalidDocument, error);
        }
        const auto source_hash = sha256_hex(source.bytes);
        write_migration_backup(paths_, source.bytes, source_hash);

        DatabaseState old_database;
        std::optional<DatabaseReplacementBackup> database_backup;
        if (replacing_database) {
            const auto sidecars_before = inspect_database_sidecars(paths_.profiles_database);
            old_database = read_database_state(paths_);
            SqliteProfileStore old_store(paths_.profiles_database);
            if (!old_store.checkpoint_for_move(error)) {
                fail(map_profile_failure(old_store.last_failure()), error);
            }
            const auto sidecars_after = inspect_database_sidecars(paths_.profiles_database);
            database_backup = write_database_replacement_backup(
                paths_, source_hash, old_database, sidecars_before, sidecars_after);
            result.replaced_database_backup = database_backup->path;
        }

        auto temporary_database = paths_.profiles_database;
        temporary_database += ".migrating";
        if (path_exists(temporary_database)) {
            fail(
                ConfigRepositoryFailure::RecoveryRequired,
                "profiles.db.migrating already exists and requires inspection");
        }
        SqliteProfileStore temporary_store(temporary_database);
        ProfileStoreSnapshot empty;
        if (!temporary_store.open_or_create(empty, error)) {
            fail(map_profile_failure(temporary_store.last_failure()), error);
        }
        ProfileStoreSnapshot imported;
        empty.profiles = profiles;
        if (!temporary_store.save(empty, imported, error)
            || !temporary_store.mark_migrated(
                "ccs-trans.config/v2", source_hash, imported, error)
            || !temporary_store.checkpoint_for_move(error)) {
            const auto failure = map_profile_failure(temporary_store.last_failure());
            remove_database_family(temporary_database);
            fail(failure, error);
        }
        std::string target_database_sha256;
        if (!sha256_file_hex(
                temporary_database, target_database_sha256, error)) {
            remove_database_family(temporary_database);
            fail(ConfigRepositoryFailure::Io, error);
        }

        ApplicationConfigDocument application{legacy.application};
        std::string new_config;
        if (!serialize_application_config_document(application, new_config, error)) {
            remove_database_family(temporary_database);
            fail(ConfigRepositoryFailure::InvalidDocument, error);
        }
        RepositoryJournal journal;
        journal.kind = "migration";
        journal.old_config = source;
        journal.new_config = new_config;
        journal.old_database_exists = replacing_database;
        if (replacing_database) {
            journal.old_profile_revision = old_database.snapshot.revision;
            journal.old_migration_hash = old_database.snapshot.migrated_from_sha256;
            journal.old_database_sha256 = database_backup->database_sha256;
            journal.old_database_backup_directory
                = database_backup->path.filename().string();
        }
        journal.target_profile_revision = imported.revision;
        journal.target_migration_hash = source_hash;
        journal.target_database_sha256 = target_database_sha256;
        try {
            write_journal(paths_, journal);
        } catch (...) {
            remove_database_family(temporary_database);
            throw;
        }
        try {
            write_atomic_file(paths_.config_file, new_config);
            if (replacing_database) {
                remove_database_sidecars(paths_.profiles_database);
                replace_file(temporary_database, paths_.profiles_database);
            } else {
                move_file_no_replace(temporary_database, paths_.profiles_database);
            }
            clear_journal(paths_, journal.kind);
        } catch (...) {
            if (path_exists(paths_.repository_transaction_directory)) {
                std::string recovery_error;
                if (!recover_locked(recovery_error)) {
                    fail(ConfigRepositoryFailure::RecoveryRequired, recovery_error);
                }
            }
            throw;
        }

        if (!load_ready_locked(error)) {
            return false;
        }
        std::string before;
        std::string after;
        if (!serialize_config_document(legacy, before, error)
            || !serialize_config_document(legacy_document_, after, error)
            || before != after) {
            fail(ConfigRepositoryFailure::Corrupt, "v2 migration semantic round-trip failed");
        }
        result.outcome = MigrationOutcome::Migrated;
        return true;
    } catch (const RepositoryError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
    } catch (const std::exception& exception) {
        last_failure_ = ConfigRepositoryFailure::Io;
        error = std::string("failed to migrate v2 repository: ") + exception.what();
    }
    return false;
}

bool CompositeConfigRepository::verify_storage(std::string& error) {
    error.clear();
    last_failure_ = ConfigRepositoryFailure::None;
    loaded_ = false;
    try {
        if (!ensure_app_directories(paths_, error)) {
            fail(ConfigRepositoryFailure::Io, error);
        }
        RepositoryLock lock(paths_.repository_lock_file);
        if (!recover_locked(error)) {
            return false;
        }
        const auto source = read_config_source(paths_);
        const bool database_exists = path_exists(paths_.profiles_database);
        if (!source.exists && !database_exists) {
            fail(ConfigRepositoryFailure::NotLoaded, "repository storage is uninitialized");
        }
        if (!source.exists || !database_exists) {
            fail(
                ConfigRepositoryFailure::RecoveryRequired,
                "config.json and profiles.db must both exist for verification");
        }
        ConfigSchemaKind schema = ConfigSchemaKind::Unsupported;
        if (!detect_config_schema(source.bytes, schema, error)) {
            fail(ConfigRepositoryFailure::InvalidDocument, error);
        }
        if (schema == ConfigSchemaKind::V2) {
            fail(
                ConfigRepositoryFailure::MigrationRequired,
                "ccs-trans.config/v2 requires explicit 'ccs-trans storage migrate'");
        }
        if (schema != ConfigSchemaKind::V3) {
            fail(ConfigRepositoryFailure::UnsupportedSchema, "unsupported config schema_version");
        }
        if (!load_ready_locked(error)) {
            return false;
        }
        SqliteProfileStore store(paths_.profiles_database);
        if (!store.verify(error)) {
            fail(map_profile_failure(store.last_failure()), error);
        }
        return true;
    } catch (const RepositoryError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
    } catch (const std::exception& exception) {
        last_failure_ = ConfigRepositoryFailure::Io;
        error = std::string("failed to verify repository storage: ") + exception.what();
    }
    return false;
}

bool CompositeConfigRepository::loaded() const {
    return loaded_;
}

const ConfigDocument& CompositeConfigRepository::document() const {
    return legacy_document_;
}

const ConfigurationSnapshot& CompositeConfigRepository::snapshot() const {
    return snapshot_;
}

const AppPaths& CompositeConfigRepository::paths() const {
    return paths_;
}

ConfigRepositoryFailure CompositeConfigRepository::last_failure() const noexcept {
    return last_failure_;
}

} // namespace ccs
