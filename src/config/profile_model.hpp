#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ccs {

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

} // namespace ccs
