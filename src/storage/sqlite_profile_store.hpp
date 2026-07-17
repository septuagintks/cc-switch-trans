#pragma once

#include "config/profile_model.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ccs {

inline constexpr std::int64_t kProfileStoreSchemaVersion = 1;

struct ProfileStoreSnapshot {
    ProfileRevision revision = 0;
    std::optional<std::string> migrated_from_sha256;
    std::vector<StoredProfile> profiles;

    bool operator==(const ProfileStoreSnapshot&) const = default;
};

enum class ProfileStoreFailure {
    None,
    NotFound,
    Busy,
    Stale,
    Constraint,
    InvalidData,
    Corrupt,
    UnsupportedSchema,
    Io,
};

struct SqliteProfileStoreOptions {
    int busy_timeout_ms = 2000;
    bool integrity_check_on_load = true;
};

class SqliteProfileStore final {
public:
    explicit SqliteProfileStore(
        std::filesystem::path database_path,
        SqliteProfileStoreOptions options = {});

    bool open_or_create(ProfileStoreSnapshot& snapshot, std::string& error);
    bool load(ProfileStoreSnapshot& snapshot, std::string& error);
    bool save(
        const ProfileStoreSnapshot& desired,
        ProfileStoreSnapshot& committed,
        std::string& error);
    bool mark_migrated(
        std::string source_schema,
        std::string source_sha256,
        ProfileStoreSnapshot& committed,
        std::string& error);
    bool checkpoint_for_move(std::string& error);
    bool verify(std::string& error);

    const std::filesystem::path& path() const noexcept;
    ProfileStoreFailure last_failure() const noexcept;

private:
    std::filesystem::path database_path_;
    SqliteProfileStoreOptions options_;
    ProfileStoreFailure last_failure_ = ProfileStoreFailure::None;
};

} // namespace ccs
