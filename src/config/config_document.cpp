#include "config/config_document.hpp"

#include "core/url.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ccs {

namespace {

using Json = nlohmann::json;

constexpr const char* kSchemaVersion = "ccs-trans.config/v2";
constexpr std::uint32_t kMaxWorkerThreads = 1024;
constexpr std::uint32_t kMaxConnections = 65535;
constexpr std::uint64_t kMaxBufferedBodySize = 1024ULL * 1024 * 1024;
constexpr std::uint64_t kMaxLogBufferSize = 1024ULL * 1024 * 1024;
constexpr std::uint64_t kMaxLogTotalSize = 1024ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t kLogRecordHeadroom = 1024ULL * 1024;
constexpr std::uint64_t kJsonEscapeExpansion = 6;
constexpr std::uint32_t kMaxFlushIntervalMs = 60000;
constexpr std::size_t kMaxUrlBytes = 8192;
constexpr std::size_t kMaxRuleOptionDepth = 32;
constexpr std::size_t kMaxRuleOptionNodes = 4096;

bool contains_control(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    });
}

bool path_from_utf8(
    const std::string& value,
    std::filesystem::path& path,
    std::string& error) {
#ifdef _WIN32
    if (value.empty()) {
        path.clear();
        return true;
    }
    const auto required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) {
        error = "logging.path must contain valid UTF-8";
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            wide.data(),
            required)
        != required) {
        error = "failed to convert logging.path from UTF-8";
        return false;
    }
    path = std::filesystem::path(std::move(wide));
#else
    (void)error;
    path = std::filesystem::path(value);
#endif
    return true;
}

bool valid_stable_id(
    const std::string& value,
    bool lower_case_only,
    bool allow_dot) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value.front());
    if ((lower_case_only && !(first >= 'a' && first <= 'z'))
        || (!lower_case_only && std::isalnum(first) == 0)) {
        return false;
    }
    for (const auto raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (lower_case_only) {
            if (!(ch >= 'a' && ch <= 'z')
                && !(ch >= '0' && ch <= '9')
                && ch != '-'
                && ch != '_') {
                return false;
            }
        } else if (std::isalnum(ch) == 0
            && ch != '-'
            && ch != '_'
            && (!allow_dot || ch != '.')) {
            return false;
        }
    }
    return true;
}

bool valid_rule_option_name_impl(const std::string& value) {
    if (value.empty() || value.size() > 64 || value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z')
            || (ch >= '0' && ch <= '9')
            || ch == '_';
    });
}

bool check_object_keys(
    const Json& value,
    const std::string& path,
    std::initializer_list<const char*> allowed,
    std::initializer_list<const char*> required,
    std::string& error) {
    if (!value.is_object()) {
        error = path + " must be a JSON object";
        return false;
    }
    std::set<std::string> allowed_keys;
    for (const auto* key : allowed) {
        allowed_keys.emplace(key);
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (allowed_keys.count(it.key()) == 0) {
            error = path + " contains unknown field: " + it.key();
            return false;
        }
    }
    for (const auto* key : required) {
        if (!value.contains(key)) {
            error = path + " is missing required field: " + key;
            return false;
        }
    }
    return true;
}

bool read_string(
    const Json& object,
    const char* key,
    const std::string& path,
    std::string& target,
    std::string& error) {
    const auto& value = object.at(key);
    if (!value.is_string()) {
        error = path + "." + key + " must be a JSON string";
        return false;
    }
    target = value.get<std::string>();
    return true;
}

bool read_bool(
    const Json& object,
    const char* key,
    const std::string& path,
    bool& target,
    std::string& error) {
    const auto& value = object.at(key);
    if (!value.is_boolean()) {
        error = path + "." + key + " must be a JSON boolean";
        return false;
    }
    target = value.get<bool>();
    return true;
}

bool read_unsigned(
    const Json& object,
    const char* key,
    const std::string& path,
    std::uint64_t maximum,
    std::uint64_t& target,
    std::string& error) {
    const auto& value = object.at(key);
    if (!value.is_number_unsigned()) {
        error = path + "." + key + " must be a non-negative JSON integer";
        return false;
    }
    const auto parsed = value.get<std::uint64_t>();
    if (parsed > maximum) {
        error = path + "." + key + " exceeds the supported maximum of "
            + std::to_string(maximum);
        return false;
    }
    target = parsed;
    return true;
}

bool read_required_application_settings(
    const Json& root,
    ApplicationSettings& application,
    std::string& error) {
    const auto& listener = root.at("listener");
    if (!check_object_keys(listener, "$.listener", {"host", "port"}, {"host", "port"}, error)
        || !read_string(listener, "host", "$.listener", application.listener.host, error)) {
        return false;
    }
    std::uint64_t number = 0;
    if (!read_unsigned(listener, "port", "$.listener", 65535, number, error) || number == 0) {
        if (error.empty()) {
            error = "$.listener.port must be between 1 and 65535";
        }
        return false;
    }
    application.listener.port = static_cast<std::uint16_t>(number);

    const auto& runtime = root.at("runtime");
    if (!check_object_keys(
            runtime,
            "$.runtime",
            {"worker_threads", "max_connections", "max_request_body_size", "max_response_body_size", "metrics_interval_ms"},
            {"worker_threads", "max_connections", "max_request_body_size", "max_response_body_size", "metrics_interval_ms"},
            error)) {
        return false;
    }
    if (!read_unsigned(runtime, "worker_threads", "$.runtime", kMaxWorkerThreads, number, error)
        || number == 0) {
        if (error.empty()) {
            error = "$.runtime.worker_threads must be greater than 0";
        }
        return false;
    }
    application.runtime.worker_threads = static_cast<std::uint32_t>(number);
    if (!read_unsigned(runtime, "max_connections", "$.runtime", kMaxConnections, number, error)
        || number == 0) {
        if (error.empty()) {
            error = "$.runtime.max_connections must be greater than 0";
        }
        return false;
    }
    application.runtime.max_connections = static_cast<std::uint32_t>(number);
    if (!read_unsigned(runtime, "max_request_body_size", "$.runtime", kMaxBufferedBodySize, number, error)
        || number == 0) {
        if (error.empty()) {
            error = "$.runtime.max_request_body_size must be greater than 0";
        }
        return false;
    }
    application.runtime.max_request_body_size = number;
    if (!read_unsigned(runtime, "max_response_body_size", "$.runtime", kMaxBufferedBodySize, number, error)
        || number == 0) {
        if (error.empty()) {
            error = "$.runtime.max_response_body_size must be greater than 0";
        }
        return false;
    }
    application.runtime.max_response_body_size = number;
    if (!read_unsigned(
            runtime,
            "metrics_interval_ms",
            "$.runtime",
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()),
            number,
            error)) {
        return false;
    }
    application.runtime.metrics_interval_ms = static_cast<std::uint32_t>(number);

    const auto& timeouts = root.at("timeouts");
    if (!check_object_keys(
            timeouts,
            "$.timeouts",
            {"resolve_ms", "connect_ms", "send_ms", "response_header_ms", "stream_idle_ms", "total_ms"},
            {"resolve_ms", "connect_ms", "send_ms", "response_header_ms", "stream_idle_ms", "total_ms"},
            error)) {
        return false;
    }
    const auto read_timeout = [&](const char* key, int& target, bool allow_zero) {
        if (!read_unsigned(
                timeouts,
                key,
                "$.timeouts",
                static_cast<std::uint64_t>(std::numeric_limits<int>::max()),
                number,
                error)) {
            return false;
        }
        if (!allow_zero && number == 0) {
            error = std::string("$.timeouts.") + key + " must be greater than 0";
            return false;
        }
        target = static_cast<int>(number);
        return true;
    };
    if (!read_timeout("resolve_ms", application.timeouts.resolve_ms, false)
        || !read_timeout("connect_ms", application.timeouts.connect_ms, false)
        || !read_timeout("send_ms", application.timeouts.send_ms, false)
        || !read_timeout("response_header_ms", application.timeouts.response_header_ms, false)
        || !read_timeout("stream_idle_ms", application.timeouts.stream_idle_ms, false)
        || !read_timeout("total_ms", application.timeouts.total_ms, true)) {
        return false;
    }

    const auto& logging = root.at("logging");
    if (!check_object_keys(
            logging,
            "$.logging",
            {"path", "level", "body", "redact_sensitive", "body_limit", "queue_capacity", "max_total_size", "flush_interval_ms"},
            {"path", "level", "body", "redact_sensitive", "body_limit", "queue_capacity", "flush_interval_ms"},
            error)
        || !read_string(logging, "path", "$.logging", application.logging.path, error)
        || !read_string(logging, "level", "$.logging", application.logging.level, error)
        || !read_bool(logging, "body", "$.logging", application.logging.body, error)
        || !read_bool(logging, "redact_sensitive", "$.logging", application.logging.redact_sensitive, error)) {
        return false;
    }
    if (!read_unsigned(logging, "body_limit", "$.logging", kMaxLogBufferSize, number, error)
        || number == 0) {
        if (error.empty()) {
            error = "$.logging.body_limit must be greater than 0";
        }
        return false;
    }
    application.logging.body_limit = number;
    if (!read_unsigned(logging, "queue_capacity", "$.logging", kMaxLogBufferSize, number, error)
        || number == 0) {
        if (error.empty()) {
            error = "$.logging.queue_capacity must be greater than 0";
        }
        return false;
    }
    application.logging.queue_capacity = number;
    if (logging.contains("max_total_size")) {
        if (!read_unsigned(logging, "max_total_size", "$.logging", kMaxLogTotalSize, number, error)
            || number == 0) {
            if (error.empty()) {
                error = "$.logging.max_total_size must be greater than 0";
            }
            return false;
        }
        application.logging.max_total_size = number;
    }
    if (!read_unsigned(logging, "flush_interval_ms", "$.logging", kMaxFlushIntervalMs, number, error)
        || number == 0) {
        if (error.empty()) {
            error = "$.logging.flush_interval_ms must be greater than 0";
        }
        return false;
    }
    application.logging.flush_interval_ms = static_cast<std::uint32_t>(number);
    return true;
}

bool validate_rule_json(
    const Json& value,
    std::size_t depth,
    std::size_t& nodes,
    std::string& error) {
    ++nodes;
    if (nodes > kMaxRuleOptionNodes) {
        error = "rule options exceed the maximum JSON node count";
        return false;
    }
    if (depth > kMaxRuleOptionDepth) {
        error = "rule options exceed the maximum JSON nesting depth";
        return false;
    }
    if (value.is_binary()
        || value.is_discarded()
        || (value.is_number_float() && !std::isfinite(value.get<double>()))) {
        error = "rule options must be losslessly representable as JSON text";
        return false;
    }
    if (value.is_array()) {
        for (const auto& child : value) {
            if (!validate_rule_json(child, depth + 1, nodes, error)) {
                return false;
            }
        }
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key().size() > 256 || contains_control(it.key())) {
                error = "rule option object contains an invalid key";
                return false;
            }
            if (!validate_rule_json(it.value(), depth + 1, nodes, error)) {
                return false;
            }
        }
    }
    return true;
}

bool parse_rule(
    const Json& source,
    const std::string& path,
    RuleDefinition& rule,
    std::string& error) {
    if (!source.is_object()) {
        error = path + " must be a JSON object";
        return false;
    }
    if (!source.contains("id") || !source.contains("type")) {
        error = path + " must contain id and type";
        return false;
    }
    if (!read_string(source, "id", path, rule.id.value, error)
        || !read_string(source, "type", path, rule.type, error)) {
        return false;
    }
    if (source.contains("enabled")
        && !read_bool(source, "enabled", path, rule.enabled, error)) {
        return false;
    }
    for (auto it = source.begin(); it != source.end(); ++it) {
        if (it.key() == "id" || it.key() == "enabled" || it.key() == "type") {
            continue;
        }
        if (!is_valid_rule_option_name(it.key())) {
            error = path + " contains an invalid rule option name: " + it.key();
            return false;
        }
        std::size_t nodes = 0;
        if (!validate_rule_json(it.value(), 0, nodes, error)) {
            error = path + "." + it.key() + ": " + error;
            return false;
        }
        rule.options.emplace(it.key(), it.value());
    }
    return true;
}

bool parse_profile(
    const Json& source,
    const std::string& profile_id,
    ProfileDefinition& profile,
    std::string& error) {
    const auto path = "$.profiles." + profile_id;
    if (!check_object_keys(
            source,
            path,
            {"enabled", "protocol", "local", "upstream", "rules"},
            {},
            error)) {
        return false;
    }
    if (source.contains("enabled")
        && !read_bool(source, "enabled", path, profile.enabled, error)) {
        return false;
    }
    if (source.contains("protocol")) {
        ProtocolId protocol;
        if (!read_string(source, "protocol", path, protocol.value, error)) {
            return false;
        }
        profile.protocol = std::move(protocol);
    }
    if (source.contains("local")) {
        const auto& local = source.at("local");
        if (!check_object_keys(local, path + ".local", {"request_path", "usage_path"}, {}, error)) {
            return false;
        }
        if (local.contains("request_path")) {
            std::string value;
            if (!read_string(local, "request_path", path + ".local", value, error)) {
                return false;
            }
            profile.local.request_path = std::move(value);
        }
        if (local.contains("usage_path")) {
            std::string value;
            if (!read_string(local, "usage_path", path + ".local", value, error)) {
                return false;
            }
            profile.local.usage_path = std::move(value);
        }
    }
    if (source.contains("upstream")) {
        const auto& upstream = source.at("upstream");
        if (!check_object_keys(
                upstream,
                path + ".upstream",
                {"base_url", "request_path", "usage_path"},
                {},
                error)) {
            return false;
        }
        if (upstream.contains("base_url")) {
            std::string value;
            if (!read_string(upstream, "base_url", path + ".upstream", value, error)) {
                return false;
            }
            profile.upstream.base_url = std::move(value);
        }
        if (upstream.contains("request_path")) {
            std::string value;
            if (!read_string(upstream, "request_path", path + ".upstream", value, error)) {
                return false;
            }
            profile.upstream.request_path = std::move(value);
        }
        if (upstream.contains("usage_path")) {
            std::string value;
            if (!read_string(upstream, "usage_path", path + ".upstream", value, error)) {
                return false;
            }
            profile.upstream.usage_path = std::move(value);
        }
    }
    if (source.contains("rules")) {
        const auto& rules = source.at("rules");
        if (!rules.is_array()) {
            error = path + ".rules must be a JSON array";
            return false;
        }
        if (rules.size() > kMaxRulesPerProfile) {
            error = path + ".rules exceeds the maximum of " + std::to_string(kMaxRulesPerProfile);
            return false;
        }
        std::unordered_set<std::string> ids;
        profile.rules.reserve(rules.size());
        for (std::size_t index = 0; index < rules.size(); ++index) {
            RuleDefinition rule;
            if (!parse_rule(rules[index], path + ".rules[" + std::to_string(index) + "]", rule, error)) {
                return false;
            }
            if (!ids.emplace(rule.id.value).second) {
                error = path + ".rules contains duplicate rule id: " + rule.id.value;
                return false;
            }
            profile.rules.push_back(std::move(rule));
        }
    }
    return true;
}

bool validate_route_path(
    const std::string& value,
    bool local,
    const std::string& label,
    std::string& error) {
    std::string canonical;
    std::string path_error;
    if (!canonicalize_http_path(value, canonical, path_error)) {
        error = label + ": " + path_error;
        return false;
    }
    if (local && (canonical == "/_ccs-trans" || canonical.rfind("/_ccs-trans/", 0) == 0)) {
        error = label + " uses the reserved /_ccs-trans management namespace";
        return false;
    }
    return true;
}

bool validate_base_url(const std::string& value, const std::string& label, std::string& error) {
    if (value.empty() || value.size() > kMaxUrlBytes || contains_control(value)) {
        error = label + " is empty, too long, or contains control characters";
        return false;
    }
    if (value.find('?') != std::string::npos || value.find('#') != std::string::npos) {
        error = label + " must not contain a query or fragment";
        return false;
    }
    try {
        const auto parsed = parse_http_url(value);
        if (std::any_of(parsed.host.begin(), parsed.host.end(), [](unsigned char ch) {
                return std::isspace(ch) != 0;
            })) {
            error = label + " contains an invalid host";
            return false;
        }
        if (!validate_route_path(parsed.base_path, false, label + " base path", error)) {
            return false;
        }
    } catch (const std::exception& ex) {
        error = label + ": " + ex.what();
        return false;
    }
    return true;
}

bool validate_application(const ApplicationSettings& application, std::string& error) {
    if (application.listener.host.empty()
        || application.listener.host.size() > 255
        || contains_control(application.listener.host)
        || std::any_of(application.listener.host.begin(), application.listener.host.end(), [](unsigned char ch) {
               return std::isspace(ch) != 0;
           })) {
        error = "listener.host must be a non-empty host without whitespace";
        return false;
    }
    if (application.listener.port == 0) {
        error = "listener.port must be between 1 and 65535";
        return false;
    }
    if (application.runtime.worker_threads == 0
        || application.runtime.worker_threads > kMaxWorkerThreads) {
        error = "runtime.worker_threads must be between 1 and " + std::to_string(kMaxWorkerThreads);
        return false;
    }
    if (application.runtime.max_connections < application.runtime.worker_threads
        || application.runtime.max_connections > kMaxConnections) {
        error = "runtime.max_connections must be between worker_threads and "
            + std::to_string(kMaxConnections);
        return false;
    }
    if (application.runtime.max_request_body_size == 0
        || application.runtime.max_request_body_size > kMaxBufferedBodySize
        || application.runtime.max_response_body_size == 0
        || application.runtime.max_response_body_size > kMaxBufferedBodySize) {
        error = "runtime body size limits must be between 1 byte and 1 GiB";
        return false;
    }
    if (application.runtime.metrics_interval_ms
        > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        error = "runtime.metrics_interval_ms exceeds the supported integer range";
        return false;
    }
    if (application.timeouts.resolve_ms <= 0
        || application.timeouts.connect_ms <= 0
        || application.timeouts.send_ms <= 0
        || application.timeouts.response_header_ms <= 0
        || application.timeouts.stream_idle_ms <= 0
        || application.timeouts.total_ms < 0) {
        error = "stage timeouts must be positive; total_ms may be 0";
        return false;
    }
    static const std::set<std::string> levels = {"trace", "debug", "info", "warn", "error"};
    if (levels.count(application.logging.level) == 0) {
        error = "logging.level must be one of trace, debug, info, warn, error";
        return false;
    }
    if (application.logging.path.empty()
        || application.logging.path.size() > 32768
        || contains_control(application.logging.path)) {
        error = "logging.path must name a log file";
        return false;
    }
    try {
        std::filesystem::path parsed_path;
        if (!path_from_utf8(application.logging.path, parsed_path, error)) {
            return false;
        }
        const auto path = parsed_path.lexically_normal();
        const auto filename = path.filename();
        if (filename.empty() || filename == "." || filename == "..") {
            error = "logging.path must name a log file";
            return false;
        }
        if (!path.is_absolute()) {
            if (path.has_root_path()
                || (!path.empty() && *path.begin() == "..")) {
                error = "relative logging.path must stay within the application root";
                return false;
            }
        }
    } catch (const std::exception& ex) {
        error = "logging.path is invalid: " + std::string(ex.what());
        return false;
    }
    if (application.logging.body_limit == 0
        || application.logging.body_limit > kMaxLogBufferSize
        || application.logging.queue_capacity == 0
        || application.logging.queue_capacity > kMaxLogBufferSize
        || application.logging.flush_interval_ms == 0
        || application.logging.flush_interval_ms > kMaxFlushIntervalMs) {
        error = "logging size fields must be 1 byte to 1 GiB and flush_interval_ms must be 1 to 60000";
        return false;
    }
    if (application.logging.max_total_size == 0
        || application.logging.max_total_size > kMaxLogTotalSize) {
        error = "logging.max_total_size must be between 1 byte and 1 TiB";
        return false;
    }
    const auto minimum_total_size = std::max(
        application.logging.queue_capacity,
        application.logging.body_limit * kJsonEscapeExpansion + kLogRecordHeadroom);
    if (application.logging.max_total_size < minimum_total_size) {
        error = "logging.max_total_size is too small for body_limit and queue_capacity";
        return false;
    }
    return true;
}

Json document_to_json(const ConfigDocument& document) {
    Json root = Json::object();
    root["schema_version"] = kSchemaVersion;
    root["listener"] = {
        {"host", document.application.listener.host},
        {"port", document.application.listener.port},
    };
    root["runtime"] = {
        {"worker_threads", document.application.runtime.worker_threads},
        {"max_connections", document.application.runtime.max_connections},
        {"max_request_body_size", document.application.runtime.max_request_body_size},
        {"max_response_body_size", document.application.runtime.max_response_body_size},
        {"metrics_interval_ms", document.application.runtime.metrics_interval_ms},
    };
    root["timeouts"] = {
        {"resolve_ms", document.application.timeouts.resolve_ms},
        {"connect_ms", document.application.timeouts.connect_ms},
        {"send_ms", document.application.timeouts.send_ms},
        {"response_header_ms", document.application.timeouts.response_header_ms},
        {"stream_idle_ms", document.application.timeouts.stream_idle_ms},
        {"total_ms", document.application.timeouts.total_ms},
    };
    root["logging"] = {
        {"path", document.application.logging.path},
        {"level", document.application.logging.level},
        {"body", document.application.logging.body},
        {"redact_sensitive", document.application.logging.redact_sensitive},
        {"body_limit", document.application.logging.body_limit},
        {"queue_capacity", document.application.logging.queue_capacity},
        {"max_total_size", document.application.logging.max_total_size},
        {"flush_interval_ms", document.application.logging.flush_interval_ms},
    };
    root["profiles"] = Json::object();
    for (const auto& [profile_id, profile] : document.profiles) {
        Json profile_json = Json::object();
        profile_json["enabled"] = profile.enabled;
        if (profile.protocol) {
            profile_json["protocol"] = profile.protocol->value;
        }
        if (profile.local.request_path || profile.local.usage_path) {
            profile_json["local"] = Json::object();
            if (profile.local.request_path) {
                profile_json["local"]["request_path"] = *profile.local.request_path;
            }
            if (profile.local.usage_path) {
                profile_json["local"]["usage_path"] = *profile.local.usage_path;
            }
        }
        if (profile.upstream.base_url
            || profile.upstream.request_path
            || profile.upstream.usage_path) {
            profile_json["upstream"] = Json::object();
            if (profile.upstream.base_url) {
                profile_json["upstream"]["base_url"] = *profile.upstream.base_url;
            }
            if (profile.upstream.request_path) {
                profile_json["upstream"]["request_path"] = *profile.upstream.request_path;
            }
            if (profile.upstream.usage_path) {
                profile_json["upstream"]["usage_path"] = *profile.upstream.usage_path;
            }
        }
        profile_json["rules"] = Json::array();
        for (const auto& rule : profile.rules) {
            Json rule_json = Json::object();
            rule_json["id"] = rule.id.value;
            rule_json["enabled"] = rule.enabled;
            rule_json["type"] = rule.type;
            for (const auto& [key, value] : rule.options) {
                rule_json[key] = value;
            }
            profile_json["rules"].push_back(std::move(rule_json));
        }
        root["profiles"][profile_id] = std::move(profile_json);
    }
    return root;
}

} // namespace

ConfigDocument make_default_config_document() {
    return {};
}

bool is_valid_profile_id(const std::string& value) {
    return valid_stable_id(value, false, true);
}

bool is_valid_protocol_id(const std::string& value) {
    return valid_stable_id(value, true, false);
}

bool is_valid_rule_id(const std::string& value) {
    return valid_stable_id(value, false, true);
}

bool is_valid_rule_type(const std::string& value) {
    return valid_stable_id(value, true, false) && value.find('-') == std::string::npos;
}

bool is_valid_rule_option_name(const std::string& value) {
    return value != "id"
        && value != "enabled"
        && value != "type"
        && valid_rule_option_name_impl(value);
}

bool resolve_application_log_path(
    const ApplicationSettings& application,
    const std::filesystem::path& application_root,
    std::filesystem::path& log_path,
    std::string& error) {
    error.clear();
    std::filesystem::path parsed;
    if (!path_from_utf8(application.logging.path, parsed, error)) {
        return false;
    }
    parsed = parsed.lexically_normal();
    if (parsed.is_absolute()) {
        log_path = std::move(parsed);
        return true;
    }
    if (parsed.has_root_path()
        || parsed.empty()
        || *parsed.begin() == "..") {
        error = "relative logging.path must stay within the application root";
        return false;
    }
    const auto root = application_root.lexically_normal();
    const auto resolved = (root / parsed).lexically_normal();
    const auto relative = resolved.lexically_relative(root);
    if (relative.empty() || relative == "." || *relative.begin() == "..") {
        error = "relative logging.path must stay within the application root";
        return false;
    }
    log_path = resolved;
    return true;
}

bool validate_profile_definition(
    const std::string& profile_id,
    const ProfileDefinition& profile,
    bool require_complete,
    std::string& error) {
    error.clear();
    const auto label = "profile " + profile_id;
    if (!is_valid_profile_id(profile_id)) {
        error = "invalid profile id: " + profile_id;
        return false;
    }
    if (profile.protocol && !is_valid_protocol_id(profile.protocol->value)) {
        error = label + " has an invalid protocol id";
        return false;
    }
    if (profile.local.request_path
        && !validate_route_path(*profile.local.request_path, true, label + " local.request_path", error)) {
        return false;
    }
    if (profile.local.usage_path
        && !validate_route_path(*profile.local.usage_path, true, label + " local.usage_path", error)) {
        return false;
    }
    if (profile.upstream.base_url
        && !validate_base_url(*profile.upstream.base_url, label + " upstream.base_url", error)) {
        return false;
    }
    if (profile.upstream.request_path
        && !validate_route_path(*profile.upstream.request_path, false, label + " upstream.request_path", error)) {
        return false;
    }
    if (profile.upstream.usage_path
        && !validate_route_path(*profile.upstream.usage_path, false, label + " upstream.usage_path", error)) {
        return false;
    }
    if (profile.rules.size() > kMaxRulesPerProfile) {
        error = label + " exceeds the maximum rule count";
        return false;
    }
    std::unordered_set<std::string> rule_ids;
    for (const auto& rule : profile.rules) {
        if (!is_valid_rule_id(rule.id.value)) {
            error = label + " has an invalid rule id: " + rule.id.value;
            return false;
        }
        if (!rule_ids.emplace(rule.id.value).second) {
            error = label + " has duplicate rule id: " + rule.id.value;
            return false;
        }
        if (!is_valid_rule_type(rule.type)) {
            error = label + " rule " + rule.id.value + " has an invalid type";
            return false;
        }
        for (const auto& [key, value] : rule.options) {
            if (!is_valid_rule_option_name(key)) {
                error = label + " rule " + rule.id.value + " has an invalid option name: " + key;
                return false;
            }
            std::size_t nodes = 0;
            if (!validate_rule_json(value, 0, nodes, error)) {
                error = label + " rule " + rule.id.value + " option " + key + ": " + error;
                return false;
            }
        }
    }
    if (!require_complete) {
        return true;
    }
    if (!profile.protocol
        || !profile.local.request_path
        || !profile.upstream.base_url
        || !profile.upstream.request_path) {
        error = label + " is enabled but missing protocol, local.request_path, upstream.base_url, or upstream.request_path";
        return false;
    }
    const bool has_local_usage = profile.local.usage_path.has_value();
    const bool has_upstream_usage = profile.upstream.usage_path.has_value();
    if (has_local_usage != has_upstream_usage) {
        error = label + " must configure both local.usage_path and upstream.usage_path";
        return false;
    }
    return true;
}

bool validate_config_document(const ConfigDocument& document, std::string& error) {
    error.clear();
    if (!validate_application(document.application, error)) {
        return false;
    }
    if (document.profiles.size() > kMaxConfigProfiles) {
        error = "config exceeds the maximum of " + std::to_string(kMaxConfigProfiles) + " profiles";
        return false;
    }
    std::size_t route_count = 0;
    for (const auto& [profile_id, profile] : document.profiles) {
        if (!validate_profile_definition(profile_id, profile, profile.enabled, error)) {
            return false;
        }
        route_count += profile.local.request_path ? 1 : 0;
        route_count += profile.local.usage_path ? 1 : 0;
    }
    if (route_count > kMaxConfigRoutes) {
        error = "config exceeds the maximum of " + std::to_string(kMaxConfigRoutes) + " routes";
        return false;
    }
    return true;
}

bool parse_config_document(
    std::string_view content,
    ConfigDocument& document,
    std::string& error) {
    error.clear();
    if (content.size() > kMaxConfigDocumentBytes) {
        error = "config document exceeds the 4 MiB limit";
        return false;
    }
    try {
        std::vector<std::unordered_set<std::string>> object_keys;
        std::string duplicate_key;
        const auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
            if (event == Json::parse_event_t::object_start) {
                object_keys.emplace_back();
            } else if (event == Json::parse_event_t::key && !object_keys.empty()) {
                const auto& key = parsed.get_ref<const std::string&>();
                if (!object_keys.back().emplace(key).second && duplicate_key.empty()) {
                    duplicate_key = key;
                }
            } else if (event == Json::parse_event_t::object_end && !object_keys.empty()) {
                object_keys.pop_back();
            }
            return true;
        };
        const auto root = Json::parse(content.begin(), content.end(), callback);
        if (!duplicate_key.empty()) {
            error = "config contains duplicate JSON object key: " + duplicate_key;
            return false;
        }
        if (!root.is_object()) {
            error = "config root must be a JSON object";
            return false;
        }
        if (!root.contains("schema_version") || !root.at("schema_version").is_string()) {
            error = "config schema_version must be a JSON string";
            return false;
        }
        if (root.at("schema_version").get<std::string>() != kSchemaVersion) {
            error = "unsupported config schema_version; expected ccs-trans.config/v2";
            return false;
        }
        if (!check_object_keys(
                root,
                "$",
                {"schema_version", "listener", "runtime", "timeouts", "logging", "profiles"},
                {"schema_version", "listener", "runtime", "timeouts", "logging", "profiles"},
                error)) {
            return false;
        }

        ConfigDocument candidate;
        if (!read_required_application_settings(root, candidate.application, error)) {
            return false;
        }
        const auto& profiles = root.at("profiles");
        if (!profiles.is_object()) {
            error = "$.profiles must be a JSON object";
            return false;
        }
        if (profiles.size() > kMaxConfigProfiles) {
            error = "config exceeds the maximum of " + std::to_string(kMaxConfigProfiles) + " profiles";
            return false;
        }
        for (auto it = profiles.begin(); it != profiles.end(); ++it) {
            ProfileDefinition profile;
            if (!parse_profile(it.value(), it.key(), profile, error)) {
                return false;
            }
            candidate.profiles.emplace(it.key(), std::move(profile));
        }
        if (!validate_config_document(candidate, error)) {
            return false;
        }
        document = std::move(candidate);
        return true;
    } catch (const Json::exception& ex) {
        error = "failed to parse config document: " + std::string(ex.what());
        return false;
    } catch (const std::exception& ex) {
        error = "failed to load config document: " + std::string(ex.what());
        return false;
    }
}

bool serialize_config_document(
    const ConfigDocument& document,
    std::string& content,
    std::string& error) {
    error.clear();
    if (!validate_config_document(document, error)) {
        return false;
    }
    try {
        auto serialized = document_to_json(document).dump(2) + "\n";
        if (serialized.size() > kMaxConfigDocumentBytes) {
            error = "serialized config document exceeds the 4 MiB limit";
            return false;
        }
        content = std::move(serialized);
        return true;
    } catch (const Json::exception& ex) {
        error = "failed to serialize config document: " + std::string(ex.what());
        return false;
    }
}

} // namespace ccs
