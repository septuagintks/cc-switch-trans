#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ccs {

inline constexpr std::int64_t kProfileStoreSchemaVersion = 1;
inline constexpr std::size_t kMaxStoredRuleOptionsBytes = 1024 * 1024;
inline constexpr std::size_t kMaxStoredProfileRulesTextBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaxStoredProfilePayloadBytes = 64 * 1024 * 1024;

using ProfileKey = std::int64_t;
using RuleKey = std::int64_t;
using ProfileRevision = std::int64_t;

struct StoredRule {
    RuleKey key = 0;
    std::string rule_id;
    bool enabled = false;
    std::string type;
    std::string options_json = "{}";

    bool operator==(const StoredRule&) const = default;
};

struct StoredProfile {
    ProfileKey key = 0;
    std::string profile_id;
    bool enabled = false;
    std::optional<std::string> protocol;
    std::optional<std::string> local_request_path;
    std::optional<std::string> local_usage_path;
    std::optional<std::string> upstream_base_url;
    std::optional<std::string> upstream_request_path;
    std::optional<std::string> upstream_usage_path;
    std::vector<StoredRule> rules;

    bool operator==(const StoredProfile&) const = default;
};

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
    bool verify(std::string& error);

    const std::filesystem::path& path() const noexcept;
    ProfileStoreFailure last_failure() const noexcept;

private:
    std::filesystem::path database_path_;
    SqliteProfileStoreOptions options_;
    ProfileStoreFailure last_failure_ = ProfileStoreFailure::None;
};

} // namespace ccs
