#include "config/config_cli.hpp"

#include "config/application_config.hpp"
#include "config/composite_config_repository.hpp"
#include "config/configuration_editor.hpp"
#include "config/field_descriptor.hpp"
#include "core/version.hpp"

#include "config/config_editing_service.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <iterator>
#include <limits>
#include <set>
#include <unordered_set>
#include <utility>

namespace ccs {

namespace {

using Json = nlohmann::json;

const std::set<std::string>& application_keys() {
    static const std::set<std::string> keys = [] {
        std::set<std::string> result;
        for (const auto& descriptor : application_field_descriptors()) {
            result.emplace(descriptor.key);
        }
        return result;
    }();
    return keys;
}

const std::set<std::string>& profile_keys() {
    static const std::set<std::string> keys = [] {
        std::set<std::string> result;
        for (const auto& descriptor : profile_field_descriptors()) {
            if (descriptor.key != "id" && descriptor.key != "enabled") {
                result.emplace(descriptor.key);
            }
        }
        return result;
    }();
    return keys;
}

bool parse_unsigned(const std::string& raw, std::uint64_t& value) {
    if (raw.empty() || raw.front() == '-' || raw.front() == '+') {
        return false;
    }
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    return result.ec == std::errc{} && result.ptr == raw.data() + raw.size();
}

bool parse_position(const std::string& raw, std::size_t& position) {
    std::uint64_t value = 0;
    if (!parse_unsigned(raw, value)
        || value == 0
        || value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    position = static_cast<std::size_t>(value);
    return true;
}

bool parse_bool_value(const std::string& raw, bool& value) {
    if (raw == "true") {
        value = true;
        return true;
    }
    if (raw == "false") {
        value = false;
        return true;
    }
    return false;
}

bool is_log_level(const std::string& value) {
    static const std::set<std::string> values = {"trace", "debug", "info", "warn", "error"};
    return values.count(value) != 0;
}

bool require_profile_id(
    const std::string& value,
    ConfigCliParseResult& result) {
    if (!is_valid_profile_id(value)) {
        result.error = "profile id must be 1-64 characters using letters, digits, ., _, or -";
        return false;
    }
    result.command.profile_id = value;
    return true;
}

bool require_rule_id(
    const std::string& value,
    ConfigCliParseResult& result) {
    if (!is_valid_rule_id(value)) {
        result.error = "rule id must be 1-64 characters using letters, digits, ., _, or -";
        return false;
    }
    result.command.rule_id = value;
    return true;
}

bool parse_config_command(int argc, char** argv, ConfigCliParseResult& result) {
    if (argc < 3) {
        result.error = "config requires a subcommand";
        return false;
    }
    const std::string subcommand = argv[2];
    if (subcommand == "show" && argc == 3) {
        result.command.kind = ConfigCliCommandKind::ConfigShow;
    } else if (subcommand == "set" && argc == 5) {
        result.command.kind = ConfigCliCommandKind::ConfigSet;
        result.command.key = argv[3];
        result.command.value = argv[4];
        if (application_keys().count(result.command.key) == 0) {
            result.error = "unknown application config key: " + result.command.key;
            return false;
        }
    } else if (subcommand == "unset" && argc == 4) {
        result.command.kind = ConfigCliCommandKind::ConfigUnset;
        result.command.key = argv[3];
        if (application_keys().count(result.command.key) == 0) {
            result.error = "unknown application config key: " + result.command.key;
            return false;
        }
    } else {
        result.error = "invalid config command or argument count";
        return false;
    }
    return true;
}

bool parse_profile_command(int argc, char** argv, ConfigCliParseResult& result) {
    if (argc < 3) {
        result.error = "profile requires a subcommand";
        return false;
    }
    const std::string subcommand = argv[2];
    if (subcommand == "list" && argc == 3) {
        result.command.kind = ConfigCliCommandKind::ProfileList;
        return true;
    }
    if (argc < 4 || !require_profile_id(argv[3], result)) {
        if (result.error.empty()) {
            result.error = "profile " + subcommand + " requires a profile id";
        }
        return false;
    }
    if (subcommand == "show" && argc == 4) {
        result.command.kind = ConfigCliCommandKind::ProfileShow;
    } else if (subcommand == "create" && argc == 4) {
        result.command.kind = ConfigCliCommandKind::ProfileCreate;
    } else if (subcommand == "remove" && argc == 4) {
        result.command.kind = ConfigCliCommandKind::ProfileRemove;
    } else if (subcommand == "enable" && argc == 4) {
        result.command.kind = ConfigCliCommandKind::ProfileEnable;
    } else if (subcommand == "disable" && argc == 4) {
        result.command.kind = ConfigCliCommandKind::ProfileDisable;
    } else if (subcommand == "set" && argc == 6) {
        result.command.kind = ConfigCliCommandKind::ProfileSet;
        result.command.key = argv[4];
        result.command.value = argv[5];
        if (profile_keys().count(result.command.key) == 0) {
            result.error = "unknown profile key: " + result.command.key;
            return false;
        }
    } else if (subcommand == "unset" && argc == 5) {
        result.command.kind = ConfigCliCommandKind::ProfileUnset;
        result.command.key = argv[4];
        if (profile_keys().count(result.command.key) == 0) {
            result.error = "unknown profile key: " + result.command.key;
            return false;
        }
    } else if (subcommand == "rename" && argc == 5) {
        result.command.kind = ConfigCliCommandKind::ProfileRename;
        if (!is_valid_profile_id(argv[4])) {
            result.error = "new profile id must be 1-64 characters using letters, digits, ., _, or -";
            return false;
        }
        result.command.value = argv[4];
    } else if (subcommand == "move" && argc == 5) {
        result.command.kind = ConfigCliCommandKind::ProfileMove;
        if (!parse_position(argv[4], result.command.position)) {
            result.error = "profile position must be a positive 1-based integer";
            return false;
        }
    } else {
        result.error = "invalid profile command or argument count";
        return false;
    }
    return true;
}

bool parse_rule_command(int argc, char** argv, ConfigCliParseResult& result) {
    if (argc < 4) {
        result.error = "rule requires a subcommand and profile id";
        return false;
    }
    const std::string subcommand = argv[2];
    if (!require_profile_id(argv[3], result)) {
        return false;
    }
    if (subcommand == "list" && argc == 4) {
        result.command.kind = ConfigCliCommandKind::RuleList;
        return true;
    }
    if (argc < 5 || !require_rule_id(argv[4], result)) {
        if (result.error.empty()) {
            result.error = "rule " + subcommand + " requires a rule id";
        }
        return false;
    }
    if (subcommand == "show" && argc == 5) {
        result.command.kind = ConfigCliCommandKind::RuleShow;
    } else if (subcommand == "add" && argc == 6) {
        result.command.kind = ConfigCliCommandKind::RuleAdd;
        result.command.rule_type = argv[5];
        if (!is_valid_rule_type(result.command.rule_type)) {
            result.error = "rule type must be 1-64 lowercase snake_case characters";
            return false;
        }
    } else if (subcommand == "remove" && argc == 5) {
        result.command.kind = ConfigCliCommandKind::RuleRemove;
    } else if (subcommand == "enable" && argc == 5) {
        result.command.kind = ConfigCliCommandKind::RuleEnable;
    } else if (subcommand == "disable" && argc == 5) {
        result.command.kind = ConfigCliCommandKind::RuleDisable;
    } else if (subcommand == "set" && argc == 7) {
        result.command.kind = ConfigCliCommandKind::RuleSet;
        result.command.key = argv[5];
        result.command.value = argv[6];
        if (!is_valid_rule_option_name(result.command.key)) {
            result.error = "rule option key must use lowercase snake_case and cannot be id, enabled, or type";
            return false;
        }
    } else if (subcommand == "unset" && argc == 6) {
        result.command.kind = ConfigCliCommandKind::RuleUnset;
        result.command.key = argv[5];
        if (!is_valid_rule_option_name(result.command.key)) {
            result.error = "rule option key must use lowercase snake_case and cannot be id, enabled, or type";
            return false;
        }
    } else if (subcommand == "move" && argc == 6) {
        result.command.kind = ConfigCliCommandKind::RuleMove;
        if (!parse_position(argv[5], result.command.position)) {
            result.error = "rule position must be a positive 1-based integer";
            return false;
        }
    } else {
        result.error = "invalid rule command or argument count";
        return false;
    }
    return true;
}

bool parse_run_command(int argc, char** argv, ConfigCliParseResult& result) {
    result.command.kind = ConfigCliCommandKind::Run;
    std::unordered_set<std::string> seen;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--help") {
            if (argc != 3) {
                result.error = "--help cannot be combined with run options";
                return false;
            }
            result.command.kind = ConfigCliCommandKind::Help;
            return true;
        }
        if (option != "--profile" && option != "--log-level" && option != "--log-path") {
            result.error = "unknown run option: " + option;
            return false;
        }
        if (!seen.emplace(option).second) {
            result.error = "duplicate run option: " + option;
            return false;
        }
        if (++index >= argc) {
            result.error = option + " requires a value";
            return false;
        }
        const std::string value = argv[index];
        if (option == "--profile") {
            if (!is_valid_profile_id(value)) {
                result.error = "--profile requires a valid profile id";
                return false;
            }
            result.command.run_profile = value;
        } else if (option == "--log-level") {
            if (!is_log_level(value)) {
                result.error = "--log-level must be one of trace, debug, info, warn, error";
                return false;
            }
            result.command.run_log_level = value;
        } else {
            auto probe = make_default_config_document();
            probe.application.logging.path = value;
            std::string validation_error;
            if (!validate_config_document(probe, validation_error)) {
                result.error = "--log-path: " + validation_error;
                return false;
            }
            result.command.run_log_path = value;
        }
    }
    return true;
}

bool parse_storage_command(int argc, char** argv, ConfigCliParseResult& result) {
    if (argc != 3) {
        result.error = "storage requires exactly one subcommand";
        return false;
    }
    const std::string subcommand = argv[2];
    if (subcommand == "status") {
        result.command.kind = ConfigCliCommandKind::StorageStatus;
    } else if (subcommand == "migrate") {
        result.command.kind = ConfigCliCommandKind::StorageMigrate;
    } else if (subcommand == "verify") {
        result.command.kind = ConfigCliCommandKind::StorageVerify;
    } else {
        result.error = "unknown storage subcommand: " + subcommand;
        return false;
    }
    return true;
}

bool set_application_value(
    ApplicationSettings& application,
    const std::string& key,
    const std::string& raw,
    std::string& error) {
    if (key == "listener.host") {
        application.listener.host = raw;
        return true;
    }
    if (key == "logging.path") {
        application.logging.path = raw;
        return true;
    }
    if (key == "logging.level") {
        application.logging.level = raw;
        return true;
    }
    if (key == "logging.body" || key == "logging.redact-sensitive") {
        bool value = false;
        if (!parse_bool_value(raw, value)) {
            error = key + " must be true or false";
            return false;
        }
        if (key == "logging.body") {
            application.logging.body = value;
        } else {
            application.logging.redact_sensitive = value;
        }
        return true;
    }

    std::uint64_t value = 0;
    if (!parse_unsigned(raw, value)) {
        error = key + " must be a non-negative integer";
        return false;
    }
    if (key == "listener.port") {
        if (value > std::numeric_limits<std::uint16_t>::max()) {
            error = key + " exceeds 65535";
            return false;
        }
        application.listener.port = static_cast<std::uint16_t>(value);
    } else if (key == "runtime.worker-threads") {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            error = key + " exceeds the supported integer range";
            return false;
        }
        application.runtime.worker_threads = static_cast<std::uint32_t>(value);
    } else if (key == "runtime.max-connections") {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            error = key + " exceeds the supported integer range";
            return false;
        }
        application.runtime.max_connections = static_cast<std::uint32_t>(value);
    } else if (key == "runtime.max-request-body-size") {
        application.runtime.max_request_body_size = value;
    } else if (key == "runtime.max-response-body-size") {
        application.runtime.max_response_body_size = value;
    } else if (key == "runtime.max-inflight-bytes") {
        application.runtime.max_inflight_bytes = value;
    } else if (key == "runtime.metrics-interval-ms") {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            error = key + " exceeds the supported integer range";
            return false;
        }
        application.runtime.metrics_interval_ms = static_cast<std::uint32_t>(value);
    } else if (key == "logging.body-limit") {
        application.logging.body_limit = value;
    } else if (key == "logging.queue-capacity") {
        application.logging.queue_capacity = value;
    } else if (key == "logging.max-total-size") {
        application.logging.max_total_size = value;
    } else if (key == "logging.flush-interval-ms") {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            error = key + " exceeds the supported integer range";
            return false;
        }
        application.logging.flush_interval_ms = static_cast<std::uint32_t>(value);
    } else {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            error = key + " exceeds the supported integer range";
            return false;
        }
        auto* target = &application.timeouts.total_ms;
        if (key == "timeouts.resolve-ms") {
            target = &application.timeouts.resolve_ms;
        } else if (key == "timeouts.connect-ms") {
            target = &application.timeouts.connect_ms;
        } else if (key == "timeouts.send-ms") {
            target = &application.timeouts.send_ms;
        } else if (key == "timeouts.response-header-ms") {
            target = &application.timeouts.response_header_ms;
        } else if (key == "timeouts.stream-idle-ms") {
            target = &application.timeouts.stream_idle_ms;
        } else if (key != "timeouts.total-ms") {
            error = "unknown application config key: " + key;
            return false;
        }
        *target = static_cast<int>(value);
    }
    return true;
}

void reset_application_value(ApplicationSettings& target, const std::string& key) {
    const auto defaults = ApplicationSettings{};
    if (key == "listener.host") {
        target.listener.host = defaults.listener.host;
    } else if (key == "listener.port") {
        target.listener.port = defaults.listener.port;
    } else if (key == "runtime.worker-threads") {
        target.runtime.worker_threads = defaults.runtime.worker_threads;
    } else if (key == "runtime.max-connections") {
        target.runtime.max_connections = defaults.runtime.max_connections;
    } else if (key == "runtime.max-request-body-size") {
        target.runtime.max_request_body_size = defaults.runtime.max_request_body_size;
    } else if (key == "runtime.max-response-body-size") {
        target.runtime.max_response_body_size = defaults.runtime.max_response_body_size;
    } else if (key == "runtime.max-inflight-bytes") {
        target.runtime.max_inflight_bytes = defaults.runtime.max_inflight_bytes;
    } else if (key == "runtime.metrics-interval-ms") {
        target.runtime.metrics_interval_ms = defaults.runtime.metrics_interval_ms;
    } else if (key == "timeouts.resolve-ms") {
        target.timeouts.resolve_ms = defaults.timeouts.resolve_ms;
    } else if (key == "timeouts.connect-ms") {
        target.timeouts.connect_ms = defaults.timeouts.connect_ms;
    } else if (key == "timeouts.send-ms") {
        target.timeouts.send_ms = defaults.timeouts.send_ms;
    } else if (key == "timeouts.response-header-ms") {
        target.timeouts.response_header_ms = defaults.timeouts.response_header_ms;
    } else if (key == "timeouts.stream-idle-ms") {
        target.timeouts.stream_idle_ms = defaults.timeouts.stream_idle_ms;
    } else if (key == "timeouts.total-ms") {
        target.timeouts.total_ms = defaults.timeouts.total_ms;
    } else if (key == "logging.path") {
        target.logging.path = defaults.logging.path;
    } else if (key == "logging.level") {
        target.logging.level = defaults.logging.level;
    } else if (key == "logging.body") {
        target.logging.body = defaults.logging.body;
    } else if (key == "logging.redact-sensitive") {
        target.logging.redact_sensitive = defaults.logging.redact_sensitive;
    } else if (key == "logging.body-limit") {
        target.logging.body_limit = defaults.logging.body_limit;
    } else if (key == "logging.queue-capacity") {
        target.logging.queue_capacity = defaults.logging.queue_capacity;
    } else if (key == "logging.max-total-size") {
        target.logging.max_total_size = defaults.logging.max_total_size;
    } else if (key == "logging.flush-interval-ms") {
        target.logging.flush_interval_ms = defaults.logging.flush_interval_ms;
    }
}

bool set_profile_value(
    ProfileDefinition& profile,
    const std::string& key,
    const std::string& value) {
    if (key == "protocol") {
        profile.protocol = ProtocolId{value};
    } else if (key == "local.request-path") {
        profile.local.request_path = value;
    } else if (key == "local.usage-path") {
        profile.local.usage_path = value;
    } else if (key == "upstream.base-url") {
        profile.upstream.base_url = value;
    } else if (key == "upstream.request-path") {
        profile.upstream.request_path = value;
    } else if (key == "upstream.usage-path") {
        profile.upstream.usage_path = value;
    } else {
        return false;
    }
    return true;
}

bool unset_profile_value(ProfileDefinition& profile, const std::string& key) {
    if (key == "protocol") {
        if (!profile.protocol) {
            return false;
        }
        profile.protocol.reset();
    } else if (key == "local.request-path") {
        if (!profile.local.request_path) {
            return false;
        }
        profile.local.request_path.reset();
    } else if (key == "local.usage-path") {
        if (!profile.local.usage_path) {
            return false;
        }
        profile.local.usage_path.reset();
    } else if (key == "upstream.base-url") {
        if (!profile.upstream.base_url) {
            return false;
        }
        profile.upstream.base_url.reset();
    } else if (key == "upstream.request-path") {
        if (!profile.upstream.request_path) {
            return false;
        }
        profile.upstream.request_path.reset();
    } else if (key == "upstream.usage-path") {
        if (!profile.upstream.usage_path) {
            return false;
        }
        profile.upstream.usage_path.reset();
    } else {
        return false;
    }
    return true;
}

auto find_rule(ProfileDefinition& profile, const std::string& id) {
    return std::find_if(profile.rules.begin(), profile.rules.end(), [&](const RuleDefinition& rule) {
        return rule.id.value == id;
    });
}

bool full_document_json(const ConfigDocument& document, Json& root, std::string& error) {
    std::string serialized;
    if (!serialize_config_document(document, serialized, error)) {
        return false;
    }
    try {
        root = Json::parse(serialized);
        return true;
    } catch (const Json::exception& ex) {
        error = "failed to render config output: " + std::string(ex.what());
        return false;
    }
}

bool render_application(const ConfigDocument& document, std::string& output, std::string& error) {
    Json root;
    if (!full_document_json(document, root, error)) {
        return false;
    }
    Json rendered = Json::object();
    for (const auto* key : {"listener", "runtime", "timeouts", "logging"}) {
        rendered[key] = root.at(key);
    }
    output = rendered.dump(2) + "\n";
    return true;
}

bool render_profile_list(const ConfigDocument& document, std::string& output) {
    Json rendered = Json::array();
    for (const auto& [id, profile] : document.profiles) {
        Json item = {
            {"id", id},
            {"enabled", profile.enabled},
        };
        if (profile.protocol) {
            item["protocol"] = profile.protocol->value;
        }
        rendered.push_back(std::move(item));
    }
    output = rendered.dump(2) + "\n";
    return true;
}

bool render_profile(
    const ConfigDocument& document,
    const std::string& id,
    std::string& output,
    std::string& error) {
    Json root;
    if (!full_document_json(document, root, error)) {
        return false;
    }
    auto rendered = root.at("profiles").at(id);
    rendered["id"] = id;
    output = rendered.dump(2) + "\n";
    return true;
}

bool render_rule_list(
    const ConfigDocument& document,
    const std::string& profile_id,
    std::string& output,
    std::string& error) {
    Json root;
    if (!full_document_json(document, root, error)) {
        return false;
    }
    output = root.at("profiles").at(profile_id).at("rules").dump(2) + "\n";
    return true;
}

bool render_rule(
    const ConfigDocument& document,
    const std::string& profile_id,
    std::size_t index,
    std::string& output,
    std::string& error) {
    Json root;
    if (!full_document_json(document, root, error)) {
        return false;
    }
    output = root.at("profiles").at(profile_id).at("rules").at(index).dump(2) + "\n";
    return true;
}

bool save_candidate(
    ConfigEditingService& editing,
    std::string rendered,
    std::string& output,
    std::string& error) {
    if (!editing.commit(error)) {
        return false;
    }
    output = std::move(rendered);
    return true;
}

const StoredProfile* find_stored_profile(
    const ConfigurationSnapshot& snapshot,
    std::string_view profile_id) {
    const auto found = std::find_if(
        snapshot.profiles.begin(), snapshot.profiles.end(), [profile_id](const auto& profile) {
            return profile.profile_id == profile_id;
        });
    return found == snapshot.profiles.end() ? nullptr : &*found;
}

const StoredRule* find_stored_rule(
    const StoredProfile& profile,
    std::string_view rule_id) {
    const auto found = std::find_if(
        profile.rules.begin(), profile.rules.end(), [rule_id](const auto& rule) {
            return rule.rule_id == rule_id;
        });
    return found == profile.rules.end() ? nullptr : &*found;
}

bool stored_rule_json(const StoredRule& rule, Json& rendered, std::string& error) {
    try {
        const auto options = Json::parse(rule.options_json);
        if (!options.is_object()) {
            error = "stored rule options are not a JSON object: " + rule.rule_id;
            return false;
        }
        rendered = {
            {"id", rule.rule_id},
            {"enabled", rule.enabled},
            {"type", rule.type},
        };
        for (auto item = options.begin(); item != options.end(); ++item) {
            rendered[item.key()] = item.value();
        }
        return true;
    } catch (const Json::exception& exception) {
        error = "failed to render stored rule " + rule.rule_id + ": " + exception.what();
        return false;
    }
}

bool stored_profile_json(
    const StoredProfile& profile,
    Json& rendered,
    std::string& error) {
    rendered = {
        {"id", profile.profile_id},
        {"enabled", profile.enabled},
    };
    if (profile.protocol) {
        rendered["protocol"] = *profile.protocol;
    }
    rendered["local"] = Json::object();
    if (profile.local_request_path) {
        rendered["local"]["request_path"] = *profile.local_request_path;
    }
    if (profile.local_usage_path) {
        rendered["local"]["usage_path"] = *profile.local_usage_path;
    }
    rendered["upstream"] = Json::object();
    if (profile.upstream_base_url) {
        rendered["upstream"]["base_url"] = *profile.upstream_base_url;
    }
    if (profile.upstream_request_path) {
        rendered["upstream"]["request_path"] = *profile.upstream_request_path;
    }
    if (profile.upstream_usage_path) {
        rendered["upstream"]["usage_path"] = *profile.upstream_usage_path;
    }
    rendered["rules"] = Json::array();
    for (const auto& rule : profile.rules) {
        Json item;
        if (!stored_rule_json(rule, item, error)) {
            return false;
        }
        rendered["rules"].push_back(std::move(item));
    }
    return true;
}

bool render_snapshot_application(
    const ConfigurationSnapshot& snapshot,
    std::string& output,
    std::string& error) {
    std::string serialized;
    if (!serialize_application_config_document({snapshot.application}, serialized, error)) {
        return false;
    }
    try {
        auto rendered = Json::parse(serialized);
        rendered.erase("schema_version");
        output = rendered.dump(2) + "\n";
        return true;
    } catch (const Json::exception& exception) {
        error = "failed to render application settings: " + std::string(exception.what());
        return false;
    }
}

bool render_snapshot_profile_list(
    const ConfigurationSnapshot& snapshot,
    std::string& output) {
    Json rendered = Json::array();
    for (const auto& profile : snapshot.profiles) {
        Json item = {
            {"id", profile.profile_id},
            {"enabled", profile.enabled},
        };
        if (profile.protocol) {
            item["protocol"] = *profile.protocol;
        }
        rendered.push_back(std::move(item));
    }
    output = rendered.dump(2) + "\n";
    return true;
}

bool render_snapshot_profile(
    const ConfigurationSnapshot& snapshot,
    std::string_view profile_id,
    std::string& output,
    std::string& error) {
    const auto* profile = find_stored_profile(snapshot, profile_id);
    if (profile == nullptr) {
        error = "profile does not exist: " + std::string(profile_id);
        return false;
    }
    Json rendered;
    if (!stored_profile_json(*profile, rendered, error)) {
        return false;
    }
    output = rendered.dump(2) + "\n";
    return true;
}

bool render_snapshot_rules(
    const ConfigurationSnapshot& snapshot,
    std::string_view profile_id,
    std::string& output,
    std::string& error) {
    const auto* profile = find_stored_profile(snapshot, profile_id);
    if (profile == nullptr) {
        error = "profile does not exist: " + std::string(profile_id);
        return false;
    }
    Json rendered = Json::array();
    for (const auto& rule : profile->rules) {
        Json item;
        if (!stored_rule_json(rule, item, error)) {
            return false;
        }
        rendered.push_back(std::move(item));
    }
    output = rendered.dump(2) + "\n";
    return true;
}

bool render_snapshot_rule(
    const ConfigurationSnapshot& snapshot,
    std::string_view profile_id,
    std::string_view rule_id,
    std::string& output,
    std::string& error) {
    const auto* profile = find_stored_profile(snapshot, profile_id);
    if (profile == nullptr) {
        error = "profile does not exist: " + std::string(profile_id);
        return false;
    }
    const auto* rule = find_stored_rule(*profile, rule_id);
    if (rule == nullptr) {
        error = "rule does not exist: " + std::string(rule_id);
        return false;
    }
    Json rendered;
    if (!stored_rule_json(*rule, rendered, error)) {
        return false;
    }
    output = rendered.dump(2) + "\n";
    return true;
}

} // namespace

ConfigCliParseResult parse_config_cli(int argc, char** argv) {
    ConfigCliParseResult result;
    if (argc < 2) {
        result.error = "missing command";
        return result;
    }
    const std::string command = argv[1];
    if (command == "--help" && argc == 2) {
        result.command.kind = ConfigCliCommandKind::Help;
    } else if (command == "--version" && argc == 2) {
        result.command.kind = ConfigCliCommandKind::Version;
    } else if (command == "config") {
        if (!parse_config_command(argc, argv, result)) {
            return result;
        }
    } else if (command == "profile") {
        if (!parse_profile_command(argc, argv, result)) {
            return result;
        }
    } else if (command == "rule") {
        if (!parse_rule_command(argc, argv, result)) {
            return result;
        }
    } else if (command == "storage") {
        if (!parse_storage_command(argc, argv, result)) {
            return result;
        }
    } else if (command == "run") {
        if (!parse_run_command(argc, argv, result)) {
            return result;
        }
    } else {
        result.error = "unknown command: " + command;
        return result;
    }
    result.ok = true;
    return result;
}

bool is_config_cli_management_command(const std::string& command) {
    return command == "config" || command == "profile" || command == "rule"
        || command == "storage";
}

bool execute_config_cli(
    const ConfigCliCommand& command,
    ConfigRepository& repository,
    std::string& output,
    std::string& error) {
    output.clear();
    error.clear();
    if (!repository.loaded()) {
        error = "config repository is not loaded";
        return false;
    }
    if (command.kind == ConfigCliCommandKind::ConfigShow) {
        return render_application(repository.document(), output, error);
    }
    if (command.kind == ConfigCliCommandKind::ProfileList) {
        return render_profile_list(repository.document(), output);
    }

    ConfigEditingService editing(repository);
    if (!editing.begin(error)) {
        return false;
    }
    auto& candidate = editing.draft();
    if (command.kind == ConfigCliCommandKind::ConfigSet) {
        if (!set_application_value(candidate.application, command.key, command.value, error)
            || !validate_config_document(candidate, error)) {
            return false;
        }
        std::string rendered;
        if (!render_application(candidate, rendered, error)) {
            return false;
        }
        return save_candidate(editing, std::move(rendered), output, error);
    }
    if (command.kind == ConfigCliCommandKind::ConfigUnset) {
        if (application_keys().count(command.key) == 0) {
            error = "unknown application config key: " + command.key;
            return false;
        }
        reset_application_value(candidate.application, command.key);
        if (!validate_config_document(candidate, error)) {
            return false;
        }
        std::string rendered;
        if (!render_application(candidate, rendered, error)) {
            return false;
        }
        return save_candidate(editing, std::move(rendered), output, error);
    }

    auto profile = candidate.profiles.find(command.profile_id);
    if (command.kind == ConfigCliCommandKind::ProfileCreate) {
        if (profile != candidate.profiles.end()) {
            error = "profile already exists: " + command.profile_id;
            return false;
        }
        candidate.profiles.emplace(command.profile_id, ProfileDefinition{});
        std::string rendered;
        if (!render_profile(candidate, command.profile_id, rendered, error)) {
            return false;
        }
        return save_candidate(editing, std::move(rendered), output, error);
    }
    if (profile == candidate.profiles.end()) {
        error = "profile does not exist: " + command.profile_id;
        return false;
    }
    if (command.kind == ConfigCliCommandKind::ProfileShow) {
        return render_profile(candidate, command.profile_id, output, error);
    }
    if (command.kind == ConfigCliCommandKind::ProfileRemove) {
        candidate.profiles.erase(profile);
        std::string rendered;
        render_profile_list(candidate, rendered);
        return save_candidate(editing, std::move(rendered), output, error);
    }
    if (command.kind == ConfigCliCommandKind::ProfileEnable
        || command.kind == ConfigCliCommandKind::ProfileDisable) {
        profile->second.enabled = command.kind == ConfigCliCommandKind::ProfileEnable;
    } else if (command.kind == ConfigCliCommandKind::ProfileSet) {
        if (!set_profile_value(profile->second, command.key, command.value)) {
            error = "unknown profile key: " + command.key;
            return false;
        }
    } else if (command.kind == ConfigCliCommandKind::ProfileUnset) {
        if (!unset_profile_value(profile->second, command.key)) {
            error = "profile key is not set: " + command.key;
            return false;
        }
    }

    const bool is_profile_mutation = command.kind == ConfigCliCommandKind::ProfileEnable
        || command.kind == ConfigCliCommandKind::ProfileDisable
        || command.kind == ConfigCliCommandKind::ProfileSet
        || command.kind == ConfigCliCommandKind::ProfileUnset;
    if (is_profile_mutation) {
        if (!validate_config_document(candidate, error)) {
            return false;
        }
        std::string rendered;
        if (!render_profile(candidate, command.profile_id, rendered, error)) {
            return false;
        }
        return save_candidate(editing, std::move(rendered), output, error);
    }

    if (command.kind == ConfigCliCommandKind::RuleList) {
        return render_rule_list(candidate, command.profile_id, output, error);
    }
    auto rule = find_rule(profile->second, command.rule_id);
    if (command.kind == ConfigCliCommandKind::RuleAdd) {
        if (rule != profile->second.rules.end()) {
            error = "rule already exists: " + command.rule_id;
            return false;
        }
        RuleDefinition added;
        added.id.value = command.rule_id;
        added.type = command.rule_type;
        profile->second.rules.push_back(std::move(added));
        rule = std::prev(profile->second.rules.end());
    } else if (rule == profile->second.rules.end()) {
        error = "rule does not exist: " + command.rule_id;
        return false;
    }

    if (command.kind == ConfigCliCommandKind::RuleShow) {
        const auto index = static_cast<std::size_t>(std::distance(profile->second.rules.begin(), rule));
        return render_rule(candidate, command.profile_id, index, output, error);
    }
    if (command.kind == ConfigCliCommandKind::RuleRemove) {
        profile->second.rules.erase(rule);
        std::string rendered;
        if (!render_rule_list(candidate, command.profile_id, rendered, error)) {
            return false;
        }
        return save_candidate(editing, std::move(rendered), output, error);
    }
    if (command.kind == ConfigCliCommandKind::RuleEnable
        || command.kind == ConfigCliCommandKind::RuleDisable) {
        rule->enabled = command.kind == ConfigCliCommandKind::RuleEnable;
    } else if (command.kind == ConfigCliCommandKind::RuleSet) {
        auto value = Json::parse(command.value, nullptr, false);
        if (value.is_discarded()) {
            value = command.value;
        }
        rule->options[command.key] = std::move(value);
    } else if (command.kind == ConfigCliCommandKind::RuleUnset) {
        if (rule->options.erase(command.key) == 0) {
            error = "rule option is not set: " + command.key;
            return false;
        }
    } else if (command.kind == ConfigCliCommandKind::RuleMove) {
        if (command.position > profile->second.rules.size()) {
            error = "rule position exceeds the profile rule count";
            return false;
        }
        RuleDefinition moving = std::move(*rule);
        const auto old_index = static_cast<std::size_t>(std::distance(profile->second.rules.begin(), rule));
        profile->second.rules.erase(profile->second.rules.begin() + static_cast<std::ptrdiff_t>(old_index));
        const auto new_index = command.position - 1;
        profile->second.rules.insert(
            profile->second.rules.begin() + static_cast<std::ptrdiff_t>(new_index),
            std::move(moving));
    }

    if (!validate_config_document(candidate, error)) {
        return false;
    }
    std::string rendered;
    if (command.kind == ConfigCliCommandKind::RuleMove) {
        if (!render_rule_list(candidate, command.profile_id, rendered, error)) {
            return false;
        }
    } else {
        const auto updated = find_rule(profile->second, command.rule_id);
        const auto index = static_cast<std::size_t>(std::distance(profile->second.rules.begin(), updated));
        if (!render_rule(candidate, command.profile_id, index, rendered, error)) {
            return false;
        }
    }
    return save_candidate(editing, std::move(rendered), output, error);
}

bool execute_config_cli(
    const ConfigCliCommand& command,
    CompositeConfigRepository& repository,
    std::string& output,
    std::string& error) {
    output.clear();
    error.clear();
    ConfigurationEditor editor(repository);
    if (!editor.begin(error)) {
        return false;
    }

    if (command.kind == ConfigCliCommandKind::ConfigShow) {
        return render_snapshot_application(editor.draft(), output, error);
    }
    if (command.kind == ConfigCliCommandKind::ProfileList) {
        return render_snapshot_profile_list(editor.draft(), output);
    }

    if (command.kind == ConfigCliCommandKind::ConfigSet
        || command.kind == ConfigCliCommandKind::ConfigUnset) {
        const auto* descriptor = find_configuration_field_descriptor(
            ConfigurationFieldScope::Application, command.key);
        if (descriptor == nullptr) {
            error = "unknown application config key: " + command.key;
            return false;
        }
        if (command.kind == ConfigCliCommandKind::ConfigSet) {
            ConfigurationFieldValue value;
            if (!parse_configuration_field_value(*descriptor, command.value, value, error)
                || !editor.apply({std::nullopt, command.key, std::move(value)}, error)) {
                return false;
            }
        } else if (!editor.apply(
                ResetConfigurationFieldCommand{std::nullopt, command.key}, error)) {
            return false;
        }
        ConfigurationSnapshot committed;
        return editor.commit(committed, error)
            && render_snapshot_application(committed, output, error);
    }

    const auto* selected = editor.find_profile_by_id(command.profile_id);
    if (command.kind == ConfigCliCommandKind::ProfileCreate) {
        if (!editor.create_profile(command.profile_id, error)) {
            return false;
        }
        ConfigurationSnapshot committed;
        return editor.commit(committed, error)
            && render_snapshot_profile(committed, command.profile_id, output, error);
    }
    if (selected == nullptr) {
        error = "profile does not exist: " + command.profile_id;
        return false;
    }
    const auto profile_key = selected->key;
    if (command.kind == ConfigCliCommandKind::ProfileShow) {
        return render_snapshot_profile(editor.draft(), command.profile_id, output, error);
    }
    if (command.kind == ConfigCliCommandKind::ProfileRemove) {
        if (!editor.remove_profile(profile_key, error)) {
            return false;
        }
        ConfigurationSnapshot committed;
        return editor.commit(committed, error)
            && render_snapshot_profile_list(committed, output);
    }
    if (command.kind == ConfigCliCommandKind::ProfileRename) {
        if (!editor.apply(
                {profile_key, "id", ConfigurationFieldValue(command.value)}, error)) {
            return false;
        }
        ConfigurationSnapshot committed;
        return editor.commit(committed, error)
            && render_snapshot_profile(committed, command.value, output, error);
    }
    if (command.kind == ConfigCliCommandKind::ProfileMove) {
        if (!editor.move_profile(profile_key, command.position, error)) {
            return false;
        }
        ConfigurationSnapshot committed;
        return editor.commit(committed, error)
            && render_snapshot_profile_list(committed, output);
    }
    if (command.kind == ConfigCliCommandKind::ProfileEnable
        || command.kind == ConfigCliCommandKind::ProfileDisable
        || command.kind == ConfigCliCommandKind::ProfileSet
        || command.kind == ConfigCliCommandKind::ProfileUnset) {
        if (command.kind == ConfigCliCommandKind::ProfileEnable
            || command.kind == ConfigCliCommandKind::ProfileDisable) {
            const bool enabled = command.kind == ConfigCliCommandKind::ProfileEnable;
            if (!editor.apply(
                    {profile_key, "enabled", ConfigurationFieldValue(enabled)}, error)) {
                return false;
            }
        } else {
            const auto* descriptor = find_configuration_field_descriptor(
                ConfigurationFieldScope::Profile, command.key);
            if (descriptor == nullptr || descriptor->key == "id"
                || descriptor->key == "enabled") {
                error = "unknown profile key: " + command.key;
                return false;
            }
            if (command.kind == ConfigCliCommandKind::ProfileSet) {
                ConfigurationFieldValue value;
                if (!parse_configuration_field_value(
                        *descriptor, command.value, value, error)
                    || !editor.apply(
                        {profile_key, command.key, std::move(value)}, error)) {
                    return false;
                }
            } else if (!editor.apply(
                    ResetConfigurationFieldCommand{profile_key, command.key}, error)) {
                return false;
            }
        }
        ConfigurationSnapshot committed;
        return editor.commit(committed, error)
            && render_snapshot_profile(committed, command.profile_id, output, error);
    }

    if (command.kind == ConfigCliCommandKind::RuleList) {
        return render_snapshot_rules(editor.draft(), command.profile_id, output, error);
    }
    if (command.kind == ConfigCliCommandKind::RuleShow) {
        return render_snapshot_rule(
            editor.draft(), command.profile_id, command.rule_id, output, error);
    }

    bool mutated = false;
    if (command.kind == ConfigCliCommandKind::RuleAdd) {
        mutated = editor.add_rule(
            profile_key, command.rule_id, command.rule_type, error);
    } else if (command.kind == ConfigCliCommandKind::RuleRemove) {
        mutated = editor.remove_rule(profile_key, command.rule_id, error);
    } else if (command.kind == ConfigCliCommandKind::RuleEnable
        || command.kind == ConfigCliCommandKind::RuleDisable) {
        mutated = editor.set_rule_enabled(
            profile_key,
            command.rule_id,
            command.kind == ConfigCliCommandKind::RuleEnable,
            error);
    } else if (command.kind == ConfigCliCommandKind::RuleSet) {
        auto value = Json::parse(command.value, nullptr, false);
        if (value.is_discarded()) {
            value = command.value;
        }
        mutated = editor.set_rule_option(
            profile_key, command.rule_id, command.key, value.dump(), error);
    } else if (command.kind == ConfigCliCommandKind::RuleUnset) {
        mutated = editor.unset_rule_option(
            profile_key, command.rule_id, command.key, error);
    } else if (command.kind == ConfigCliCommandKind::RuleMove) {
        mutated = editor.move_rule(
            profile_key, command.rule_id, command.position, error);
    } else {
        error = "command is not a configuration management action";
        return false;
    }
    if (!mutated) {
        return false;
    }
    ConfigurationSnapshot committed;
    if (!editor.commit(committed, error)) {
        return false;
    }
    if (command.kind == ConfigCliCommandKind::RuleRemove
        || command.kind == ConfigCliCommandKind::RuleMove) {
        return render_snapshot_rules(committed, command.profile_id, output, error);
    }
    return render_snapshot_rule(
        committed, command.profile_id, command.rule_id, output, error);
}

void print_config_cli_help(std::ostream& output) {
    output
        << "ccs-trans " << kVersion << "\n\n"
        << "Usage:\n"
        << "  ccs-trans config show\n"
        << "  ccs-trans config set <key> <value>\n"
        << "  ccs-trans config unset <key>\n"
        << "  ccs-trans profile list\n"
        << "  ccs-trans profile show <profile>\n"
        << "  ccs-trans profile create <profile>\n"
        << "  ccs-trans profile remove <profile>\n"
        << "  ccs-trans profile enable <profile>\n"
        << "  ccs-trans profile disable <profile>\n"
        << "  ccs-trans profile set <profile> <key> <value>\n"
        << "  ccs-trans profile unset <profile> <key>\n"
        << "  ccs-trans profile rename <profile> <new-profile>\n"
        << "  ccs-trans profile move <profile> <1-based-position>\n"
        << "  ccs-trans rule list <profile>\n"
        << "  ccs-trans rule show <profile> <rule>\n"
        << "  ccs-trans rule add <profile> <rule> <type>\n"
        << "  ccs-trans rule remove <profile> <rule>\n"
        << "  ccs-trans rule enable <profile> <rule>\n"
        << "  ccs-trans rule disable <profile> <rule>\n"
        << "  ccs-trans rule set <profile> <rule> <key> <json-or-string>\n"
        << "  ccs-trans rule unset <profile> <rule> <key>\n"
        << "  ccs-trans rule move <profile> <rule> <1-based-position>\n"
        << "  ccs-trans storage status\n"
        << "  ccs-trans storage migrate\n"
        << "  ccs-trans storage verify\n"
        << "  ccs-trans run [--profile <profile>] [--log-level <level>] [--log-path <path>]\n"
        << "  ccs-trans --help\n"
        << "  ccs-trans --version\n\n"
        << "Application keys:\n"
        << "  listener.host, listener.port\n"
        << "  runtime.worker-threads, runtime.max-connections\n"
        << "  runtime.max-request-body-size, runtime.max-response-body-size\n"
        << "  runtime.max-inflight-bytes, runtime.metrics-interval-ms\n"
        << "  timeouts.resolve-ms, timeouts.connect-ms, timeouts.send-ms\n"
        << "  timeouts.response-header-ms, timeouts.stream-idle-ms, timeouts.total-ms\n"
        << "  logging.path, logging.level, logging.body, logging.redact-sensitive\n"
        << "  logging.body-limit, logging.queue-capacity, logging.max-total-size\n"
        << "  logging.flush-interval-ms\n\n"
        << "Profile keys:\n"
        << "  protocol\n"
        << "  local.request-path, local.usage-path\n"
        << "  upstream.base-url, upstream.request-path, upstream.usage-path\n";
}

void print_config_cli_version(std::ostream& output) {
    output << "ccs-trans " << kVersion << "\n";
}

} // namespace ccs
