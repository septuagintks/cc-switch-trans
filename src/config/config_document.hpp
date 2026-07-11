#pragma once

#include "core/timeouts.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ccs {

inline constexpr std::size_t kMaxConfigDocumentBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaxConfigProfiles = 128;
inline constexpr std::size_t kMaxConfigRoutes = 256;
inline constexpr std::size_t kMaxRulesPerProfile = 64;

struct ProtocolId {
    std::string value;
};

struct RuleId {
    std::string value;
};

struct ListenerSettings {
    std::string host = "127.0.0.1";
    std::uint16_t port = 15723;
};

struct RuntimeSettings {
    std::uint32_t worker_threads = 32;
    std::uint32_t max_connections = 64;
    std::uint64_t max_request_body_size = 100ULL * 1024 * 1024;
    std::uint64_t max_response_body_size = 100ULL * 1024 * 1024;
    std::uint32_t metrics_interval_ms = 0;
};

struct LoggingSettings {
    std::string path = "logs/ccs-trans.log";
    std::string level = "info";
    bool body = true;
    bool redact_sensitive = false;
    std::uint64_t body_limit = 1024ULL * 1024;
    std::uint64_t queue_capacity = 16ULL * 1024 * 1024;
    std::uint32_t flush_interval_ms = 100;
};

struct ApplicationSettings {
    ListenerSettings listener;
    RuntimeSettings runtime;
    TimeoutConfig timeouts;
    LoggingSettings logging;
};

struct LocalRoutes {
    std::optional<std::string> request_path;
    std::optional<std::string> usage_path;
};

struct UpstreamDefinition {
    std::optional<std::string> base_url;
    std::optional<std::string> request_path;
    std::optional<std::string> usage_path;
};

struct RuleDefinition {
    RuleId id;
    bool enabled = false;
    std::string type;
    std::map<std::string, nlohmann::json> options;
};

struct ProfileDefinition {
    bool enabled = false;
    std::optional<ProtocolId> protocol;
    LocalRoutes local;
    UpstreamDefinition upstream;
    std::vector<RuleDefinition> rules;
};

struct ConfigDocument {
    ApplicationSettings application;
    std::map<std::string, ProfileDefinition> profiles;
};

ConfigDocument make_default_config_document();

bool parse_config_document(
    std::string_view content,
    ConfigDocument& document,
    std::string& error);
bool serialize_config_document(
    const ConfigDocument& document,
    std::string& content,
    std::string& error);

bool validate_config_document(const ConfigDocument& document, std::string& error);
bool validate_profile_definition(
    const std::string& profile_id,
    const ProfileDefinition& profile,
    bool require_complete,
    std::string& error);

bool is_valid_profile_id(const std::string& value);
bool is_valid_protocol_id(const std::string& value);
bool is_valid_rule_id(const std::string& value);
bool is_valid_rule_type(const std::string& value);
bool is_valid_rule_option_name(const std::string& value);
bool resolve_application_log_path(
    const ApplicationSettings& application,
    const std::filesystem::path& application_root,
    std::filesystem::path& log_path,
    std::string& error);

} // namespace ccs
