#include "config/field_descriptor.hpp"

#include "config/config_document.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace ccs {

namespace {

constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kMiB = 1024 * kKiB;
constexpr std::uint64_t kGiB = 1024 * kMiB;
constexpr std::uint64_t kTiB = 1024 * kGiB;

const std::vector<ConfigurationFieldDescriptor>& application_descriptors() {
    static const std::vector<ConfigurationFieldDescriptor> descriptors = {
        {"listener.host", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::Text, true, {}, {}, {},
            "field.listener.host", RuntimeApplyImpact::ServiceRestart},
        {"listener.port", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1, 65535, {},
            "field.listener.port", RuntimeApplyImpact::ServiceRestart},
        {"runtime.worker-threads", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1, 1024, {},
            "field.runtime.worker_threads", RuntimeApplyImpact::ServiceRestart},
        {"runtime.max-connections", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1, 65535, {},
            "field.runtime.max_connections", RuntimeApplyImpact::RuntimeReload},
        {"runtime.max-request-body-size", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1, kGiB, {},
            "field.runtime.max_request_body_size", RuntimeApplyImpact::RuntimeReload},
        {"runtime.max-response-body-size", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1, kGiB, {},
            "field.runtime.max_response_body_size", RuntimeApplyImpact::RuntimeReload},
        {"runtime.max-inflight-bytes", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, kMiB, 64 * kGiB, {},
            "field.runtime.max_inflight_bytes", RuntimeApplyImpact::ServiceRestart},
        {"runtime.metrics-interval-ms", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 0,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()), {},
            "field.runtime.metrics_interval_ms", RuntimeApplyImpact::ServiceRestart},
        {"timeouts.resolve-ms", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()), {},
            "field.timeouts.resolve_ms", RuntimeApplyImpact::RuntimeReload},
        {"timeouts.connect-ms", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()), {},
            "field.timeouts.connect_ms", RuntimeApplyImpact::RuntimeReload},
        {"timeouts.send-ms", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()), {},
            "field.timeouts.send_ms", RuntimeApplyImpact::RuntimeReload},
        {"timeouts.response-header-ms", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()), {},
            "field.timeouts.response_header_ms", RuntimeApplyImpact::RuntimeReload},
        {"timeouts.stream-idle-ms", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()), {},
            "field.timeouts.stream_idle_ms", RuntimeApplyImpact::RuntimeReload},
        {"timeouts.total-ms", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 0,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()), {},
            "field.timeouts.total_ms", RuntimeApplyImpact::RuntimeReload},
        {"logging.path", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::Path, true, {}, {}, {},
            "field.logging.path", RuntimeApplyImpact::RuntimeReload},
        {"logging.level", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::Enumeration, true, {}, {},
            {"trace", "debug", "info", "warn", "error"},
            "field.logging.level", RuntimeApplyImpact::RuntimeReload},
        {"logging.body", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::Boolean, true, {}, {}, {},
            "field.logging.body", RuntimeApplyImpact::RuntimeReload},
        {"logging.redact-sensitive", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::Boolean, true, {}, {}, {},
            "field.logging.redact_sensitive", RuntimeApplyImpact::RuntimeReload},
        {"logging.body-limit", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1, kGiB, {},
            "field.logging.body_limit", RuntimeApplyImpact::RuntimeReload},
        {"logging.queue-capacity", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1, kGiB, {},
            "field.logging.queue_capacity", RuntimeApplyImpact::ServiceRestart},
        {"logging.max-total-size", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1, kTiB, {},
            "field.logging.max_total_size", RuntimeApplyImpact::ServiceRestart},
        {"logging.flush-interval-ms", ConfigurationFieldScope::Application,
            ConfigurationFieldInputKind::UnsignedInteger, true, 1, 60000, {},
            "field.logging.flush_interval_ms", RuntimeApplyImpact::ServiceRestart},
    };
    return descriptors;
}

const std::vector<ConfigurationFieldDescriptor>& profile_descriptors() {
    static const std::vector<ConfigurationFieldDescriptor> descriptors = {
        {"id", ConfigurationFieldScope::Profile, ConfigurationFieldInputKind::Text,
            true, {}, {}, {}, "field.profile.id", RuntimeApplyImpact::RuntimeReload},
        {"enabled", ConfigurationFieldScope::Profile, ConfigurationFieldInputKind::Boolean,
            true, {}, {}, {}, "field.profile.enabled", RuntimeApplyImpact::RuntimeReload},
        {"protocol", ConfigurationFieldScope::Profile,
            ConfigurationFieldInputKind::Enumeration, false, {}, {},
            {"chat", "messages", "responses"}, "field.profile.protocol",
            RuntimeApplyImpact::RuntimeReload},
        {"local.request-path", ConfigurationFieldScope::Profile,
            ConfigurationFieldInputKind::Path, false, {}, {}, {},
            "field.profile.local_request_path", RuntimeApplyImpact::RuntimeReload},
        {"local.usage-path", ConfigurationFieldScope::Profile,
            ConfigurationFieldInputKind::Path, false, {}, {}, {},
            "field.profile.local_usage_path", RuntimeApplyImpact::RuntimeReload},
        {"upstream.base-url", ConfigurationFieldScope::Profile,
            ConfigurationFieldInputKind::Url, false, {}, {}, {},
            "field.profile.upstream_base_url", RuntimeApplyImpact::RuntimeReload},
        {"upstream.request-path", ConfigurationFieldScope::Profile,
            ConfigurationFieldInputKind::Path, false, {}, {}, {},
            "field.profile.upstream_request_path", RuntimeApplyImpact::RuntimeReload},
        {"upstream.usage-path", ConfigurationFieldScope::Profile,
            ConfigurationFieldInputKind::Path, false, {}, {}, {},
            "field.profile.upstream_usage_path", RuntimeApplyImpact::RuntimeReload},
    };
    return descriptors;
}

bool require_scope(
    const ConfigurationFieldDescriptor& descriptor,
    ConfigurationFieldScope expected,
    std::string& error) {
    if (descriptor.scope == expected) {
        return true;
    }
    error = "field descriptor has the wrong scope: " + std::string(descriptor.key);
    return false;
}

bool validate_typed_value(
    const ConfigurationFieldDescriptor& descriptor,
    const ConfigurationFieldValue& value,
    std::string& error) {
    if (descriptor.input_kind == ConfigurationFieldInputKind::UnsignedInteger) {
        const auto* number = std::get_if<std::uint64_t>(&value);
        if (number == nullptr) {
            error = std::string(descriptor.key) + " requires an unsigned integer";
            return false;
        }
        if (descriptor.minimum && *number < *descriptor.minimum) {
            error = std::string(descriptor.key) + " is below its supported minimum";
            return false;
        }
        if (descriptor.maximum && *number > *descriptor.maximum) {
            error = std::string(descriptor.key) + " exceeds its supported maximum";
            return false;
        }
        return true;
    }
    if (descriptor.input_kind == ConfigurationFieldInputKind::Boolean) {
        if (std::holds_alternative<bool>(value)) {
            return true;
        }
        error = std::string(descriptor.key) + " requires a boolean";
        return false;
    }
    const auto* text = std::get_if<std::string>(&value);
    if (text == nullptr) {
        error = std::string(descriptor.key) + " requires text";
        return false;
    }
    if (descriptor.input_kind == ConfigurationFieldInputKind::Enumeration
        && std::find(descriptor.enum_values.begin(), descriptor.enum_values.end(), *text)
            == descriptor.enum_values.end()) {
        error = std::string(descriptor.key) + " has an unsupported value: " + *text;
        return false;
    }
    return true;
}

} // namespace

std::span<const ConfigurationFieldDescriptor> application_field_descriptors() {
    return application_descriptors();
}

std::span<const ConfigurationFieldDescriptor> profile_field_descriptors() {
    return profile_descriptors();
}

const ConfigurationFieldDescriptor* find_configuration_field_descriptor(
    ConfigurationFieldScope scope,
    std::string_view key) {
    const auto descriptors = scope == ConfigurationFieldScope::Application
        ? application_field_descriptors()
        : profile_field_descriptors();
    const auto found = std::find_if(
        descriptors.begin(), descriptors.end(), [key](const auto& descriptor) {
            return descriptor.key == key;
        });
    return found == descriptors.end() ? nullptr : &*found;
}

bool parse_configuration_field_value(
    const ConfigurationFieldDescriptor& descriptor,
    std::string_view raw,
    ConfigurationFieldValue& value,
    std::string& error) {
    error.clear();
    if (descriptor.input_kind == ConfigurationFieldInputKind::UnsignedInteger) {
        std::uint64_t number = 0;
        const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), number);
        if (raw.empty() || raw.front() == '+' || raw.front() == '-'
            || parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size()) {
            error = std::string(descriptor.key) + " must be a non-negative integer";
            return false;
        }
        value = number;
    } else if (descriptor.input_kind == ConfigurationFieldInputKind::Boolean) {
        if (raw == "true") {
            value = true;
        } else if (raw == "false") {
            value = false;
        } else {
            error = std::string(descriptor.key) + " must be true or false";
            return false;
        }
    } else {
        value = std::string(raw);
    }
    return validate_typed_value(descriptor, value, error);
}

bool apply_application_field(
    ApplicationSettings& application,
    const ConfigurationFieldDescriptor& descriptor,
    const ConfigurationFieldValue& value,
    std::string& error) {
    error.clear();
    if (!require_scope(descriptor, ConfigurationFieldScope::Application, error)
        || !validate_typed_value(descriptor, value, error)) {
        return false;
    }
    const auto key = descriptor.key;
    if (key == "listener.host") {
        application.listener.host = std::get<std::string>(value);
    } else if (key == "listener.port") {
        application.listener.port = static_cast<std::uint16_t>(std::get<std::uint64_t>(value));
    } else if (key == "runtime.worker-threads") {
        application.runtime.worker_threads = static_cast<std::uint32_t>(std::get<std::uint64_t>(value));
    } else if (key == "runtime.max-connections") {
        application.runtime.max_connections = static_cast<std::uint32_t>(std::get<std::uint64_t>(value));
    } else if (key == "runtime.max-request-body-size") {
        application.runtime.max_request_body_size = std::get<std::uint64_t>(value);
    } else if (key == "runtime.max-response-body-size") {
        application.runtime.max_response_body_size = std::get<std::uint64_t>(value);
    } else if (key == "runtime.max-inflight-bytes") {
        application.runtime.max_inflight_bytes = std::get<std::uint64_t>(value);
    } else if (key == "runtime.metrics-interval-ms") {
        application.runtime.metrics_interval_ms = static_cast<std::uint32_t>(std::get<std::uint64_t>(value));
    } else if (key == "timeouts.resolve-ms") {
        application.timeouts.resolve_ms = static_cast<int>(std::get<std::uint64_t>(value));
    } else if (key == "timeouts.connect-ms") {
        application.timeouts.connect_ms = static_cast<int>(std::get<std::uint64_t>(value));
    } else if (key == "timeouts.send-ms") {
        application.timeouts.send_ms = static_cast<int>(std::get<std::uint64_t>(value));
    } else if (key == "timeouts.response-header-ms") {
        application.timeouts.response_header_ms = static_cast<int>(std::get<std::uint64_t>(value));
    } else if (key == "timeouts.stream-idle-ms") {
        application.timeouts.stream_idle_ms = static_cast<int>(std::get<std::uint64_t>(value));
    } else if (key == "timeouts.total-ms") {
        application.timeouts.total_ms = static_cast<int>(std::get<std::uint64_t>(value));
    } else if (key == "logging.path") {
        application.logging.path = std::get<std::string>(value);
    } else if (key == "logging.level") {
        application.logging.level = std::get<std::string>(value);
    } else if (key == "logging.body") {
        application.logging.body = std::get<bool>(value);
    } else if (key == "logging.redact-sensitive") {
        application.logging.redact_sensitive = std::get<bool>(value);
    } else if (key == "logging.body-limit") {
        application.logging.body_limit = std::get<std::uint64_t>(value);
    } else if (key == "logging.queue-capacity") {
        application.logging.queue_capacity = std::get<std::uint64_t>(value);
    } else if (key == "logging.max-total-size") {
        application.logging.max_total_size = std::get<std::uint64_t>(value);
    } else if (key == "logging.flush-interval-ms") {
        application.logging.flush_interval_ms = static_cast<std::uint32_t>(std::get<std::uint64_t>(value));
    } else {
        error = "unsupported application field: " + std::string(key);
        return false;
    }
    return true;
}

bool reset_application_field(
    ApplicationSettings& application,
    const ConfigurationFieldDescriptor& descriptor,
    std::string& error) {
    ApplicationSettings defaults;
    ConfigurationFieldValue value;
    const auto key = descriptor.key;
    if (key == "listener.host") value = defaults.listener.host;
    else if (key == "listener.port") value = static_cast<std::uint64_t>(defaults.listener.port);
    else if (key == "runtime.worker-threads") value = static_cast<std::uint64_t>(defaults.runtime.worker_threads);
    else if (key == "runtime.max-connections") value = static_cast<std::uint64_t>(defaults.runtime.max_connections);
    else if (key == "runtime.max-request-body-size") value = defaults.runtime.max_request_body_size;
    else if (key == "runtime.max-response-body-size") value = defaults.runtime.max_response_body_size;
    else if (key == "runtime.max-inflight-bytes") value = defaults.runtime.max_inflight_bytes;
    else if (key == "runtime.metrics-interval-ms") value = static_cast<std::uint64_t>(defaults.runtime.metrics_interval_ms);
    else if (key == "timeouts.resolve-ms") value = static_cast<std::uint64_t>(defaults.timeouts.resolve_ms);
    else if (key == "timeouts.connect-ms") value = static_cast<std::uint64_t>(defaults.timeouts.connect_ms);
    else if (key == "timeouts.send-ms") value = static_cast<std::uint64_t>(defaults.timeouts.send_ms);
    else if (key == "timeouts.response-header-ms") value = static_cast<std::uint64_t>(defaults.timeouts.response_header_ms);
    else if (key == "timeouts.stream-idle-ms") value = static_cast<std::uint64_t>(defaults.timeouts.stream_idle_ms);
    else if (key == "timeouts.total-ms") value = static_cast<std::uint64_t>(defaults.timeouts.total_ms);
    else if (key == "logging.path") value = defaults.logging.path;
    else if (key == "logging.level") value = defaults.logging.level;
    else if (key == "logging.body") value = defaults.logging.body;
    else if (key == "logging.redact-sensitive") value = defaults.logging.redact_sensitive;
    else if (key == "logging.body-limit") value = defaults.logging.body_limit;
    else if (key == "logging.queue-capacity") value = defaults.logging.queue_capacity;
    else if (key == "logging.max-total-size") value = defaults.logging.max_total_size;
    else if (key == "logging.flush-interval-ms") value = static_cast<std::uint64_t>(defaults.logging.flush_interval_ms);
    else {
        error = "unsupported application field: " + std::string(key);
        return false;
    }
    return apply_application_field(application, descriptor, value, error);
}

bool apply_profile_field(
    StoredProfile& profile,
    const ConfigurationFieldDescriptor& descriptor,
    const ConfigurationFieldValue& value,
    std::string& error) {
    error.clear();
    if (!require_scope(descriptor, ConfigurationFieldScope::Profile, error)
        || !validate_typed_value(descriptor, value, error)) {
        return false;
    }
    const auto key = descriptor.key;
    if (key == "id") {
        const auto& id = std::get<std::string>(value);
        if (!is_valid_profile_id(id)) {
            error = "profile id must be 1-64 characters using letters, digits, ., _, or -";
            return false;
        }
        profile.profile_id = id;
    } else if (key == "enabled") {
        profile.enabled = std::get<bool>(value);
    } else if (key == "protocol") {
        profile.protocol = std::get<std::string>(value);
    } else if (key == "local.request-path") {
        profile.local_request_path = std::get<std::string>(value);
    } else if (key == "local.usage-path") {
        profile.local_usage_path = std::get<std::string>(value);
    } else if (key == "upstream.base-url") {
        profile.upstream_base_url = std::get<std::string>(value);
    } else if (key == "upstream.request-path") {
        profile.upstream_request_path = std::get<std::string>(value);
    } else if (key == "upstream.usage-path") {
        profile.upstream_usage_path = std::get<std::string>(value);
    } else {
        error = "unsupported profile field: " + std::string(key);
        return false;
    }
    return true;
}

bool reset_profile_field(
    StoredProfile& profile,
    const ConfigurationFieldDescriptor& descriptor,
    std::string& error) {
    error.clear();
    if (!require_scope(descriptor, ConfigurationFieldScope::Profile, error)) {
        return false;
    }
    const auto key = descriptor.key;
    if (key == "enabled") profile.enabled = false;
    else if (key == "protocol" && profile.protocol) profile.protocol.reset();
    else if (key == "local.request-path" && profile.local_request_path) profile.local_request_path.reset();
    else if (key == "local.usage-path" && profile.local_usage_path) profile.local_usage_path.reset();
    else if (key == "upstream.base-url" && profile.upstream_base_url) profile.upstream_base_url.reset();
    else if (key == "upstream.request-path" && profile.upstream_request_path) profile.upstream_request_path.reset();
    else if (key == "upstream.usage-path" && profile.upstream_usage_path) profile.upstream_usage_path.reset();
    else if (key != "id") {
        error = "profile field is not set: " + std::string(key);
        return false;
    }
    else {
        error = "profile id cannot be unset";
        return false;
    }
    return true;
}

const char* configuration_field_input_kind_name(
    ConfigurationFieldInputKind kind) noexcept {
    switch (kind) {
    case ConfigurationFieldInputKind::Text: return "text";
    case ConfigurationFieldInputKind::UnsignedInteger: return "unsigned_integer";
    case ConfigurationFieldInputKind::Boolean: return "boolean";
    case ConfigurationFieldInputKind::Enumeration: return "enumeration";
    case ConfigurationFieldInputKind::Path: return "path";
    case ConfigurationFieldInputKind::Url: return "url";
    }
    return "unknown";
}

const char* runtime_apply_impact_name(RuntimeApplyImpact impact) noexcept {
    switch (impact) {
    case RuntimeApplyImpact::RuntimeReload: return "runtime_reload";
    case RuntimeApplyImpact::ServiceRestart: return "service_restart";
    }
    return "unknown";
}

} // namespace ccs
