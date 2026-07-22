#pragma once

#include "config/application_settings.hpp"
#include "config/profile_model.hpp"

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
    RuleKey storage_key = 0;
};

struct ProfileDefinition {
    bool enabled = false;
    std::optional<ProtocolId> protocol;
    LocalRoutes local;
    UpstreamDefinition upstream;
    std::vector<RuleDefinition> rules;
    ProfileKey storage_key = 0;
    std::optional<std::size_t> storage_position;
};

struct ConfigDocument {
    ApplicationSettings application;
    std::map<std::string, ProfileDefinition> profiles;
};

struct ConfigDocumentValidationFailure {
    std::string profile_id;
    std::string field;
    std::string detail;
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
bool validate_config_document(
    const ConfigDocument& document,
    ConfigDocumentValidationFailure& failure);
bool validate_application_settings(
    const ApplicationSettings& application,
    std::string& error);
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
