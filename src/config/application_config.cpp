#include "config/application_config.hpp"

#include "config/config_document.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ccs {

namespace {

using Json = nlohmann::json;

bool parse_root(std::string_view content, Json& root, std::string& error) {
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
        root = Json::parse(content.begin(), content.end(), callback);
        if (!duplicate_key.empty()) {
            error = "config contains duplicate JSON object key: " + duplicate_key;
            return false;
        }
        if (!root.is_object()) {
            error = "config root must be a JSON object";
            return false;
        }
        return true;
    } catch (const Json::exception& exception) {
        error = "failed to parse config document: " + std::string(exception.what());
        return false;
    }
}

bool exact_root_keys(const Json& root, std::string& error) {
    static const std::set<std::string> expected = {
        "schema_version",
        "listener",
        "runtime",
        "timeouts",
        "logging",
    };
    for (auto item = root.begin(); item != root.end(); ++item) {
        if (expected.count(item.key()) == 0) {
            error = "config contains unknown root field: " + item.key();
            return false;
        }
    }
    for (const auto& key : expected) {
        if (!root.contains(key)) {
            error = "config is missing required root field: " + key;
            return false;
        }
    }
    return true;
}

} // namespace

ApplicationConfigDocument make_default_application_config_document() {
    return {};
}

bool detect_config_schema(
    std::string_view content,
    ConfigSchemaKind& schema,
    std::string& error) {
    error.clear();
    Json root;
    if (!parse_root(content, root, error)) {
        return false;
    }
    if (!root.contains("schema_version") || !root.at("schema_version").is_string()) {
        error = "config schema_version must be a JSON string";
        return false;
    }
    const auto value = root.at("schema_version").get<std::string>();
    if (value == "ccs-trans.config/v2") {
        schema = ConfigSchemaKind::V2;
    } else if (value == kApplicationConfigSchema) {
        schema = ConfigSchemaKind::V3;
    } else {
        schema = ConfigSchemaKind::Unsupported;
    }
    return true;
}

bool validate_application_config_document(
    const ApplicationConfigDocument& document,
    std::string& error) {
    return validate_application_settings(document.application, error);
}

bool parse_application_config_document(
    std::string_view content,
    ApplicationConfigDocument& document,
    std::string& error) {
    error.clear();
    Json root;
    if (!parse_root(content, root, error)) {
        return false;
    }
    if (!root.contains("schema_version")
        || !root.at("schema_version").is_string()
        || root.at("schema_version").get<std::string>() != kApplicationConfigSchema) {
        error = "unsupported config schema_version; expected ccs-trans.config/v3";
        return false;
    }
    if (!exact_root_keys(root, error)) {
        return false;
    }
    if (!root.at("runtime").is_object()) {
        error = "$.runtime must be a JSON object";
        return false;
    }
    auto& runtime = root.at("runtime");
    if (!runtime.contains("max_inflight_bytes")
        || !runtime.at("max_inflight_bytes").is_number_unsigned()) {
        error = "$.runtime.max_inflight_bytes must be a non-negative JSON integer";
        return false;
    }

    try {
        const auto max_inflight_bytes = runtime.at("max_inflight_bytes").get<std::uint64_t>();
        runtime.erase("max_inflight_bytes");
        root["schema_version"] = "ccs-trans.config/v2";
        root["profiles"] = Json::object();

        ConfigDocument legacy;
        const auto translated = root.dump();
        if (!parse_config_document(translated, legacy, error)) {
            error = "invalid v3 application settings: " + error;
            return false;
        }
        legacy.application.runtime.max_inflight_bytes = max_inflight_bytes;
        ApplicationConfigDocument candidate{legacy.application};
        if (!validate_application_config_document(candidate, error)) {
            return false;
        }
        document = std::move(candidate);
        return true;
    } catch (const Json::exception& exception) {
        error = "failed to read v3 application settings: " + std::string(exception.what());
        return false;
    }
}

bool serialize_application_config_document(
    const ApplicationConfigDocument& document,
    std::string& content,
    std::string& error) {
    error.clear();
    if (!validate_application_config_document(document, error)) {
        return false;
    }
    try {
        ConfigDocument legacy;
        legacy.application = document.application;
        std::string serialized_legacy;
        if (!serialize_config_document(legacy, serialized_legacy, error)) {
            return false;
        }
        auto root = Json::parse(serialized_legacy);
        root["schema_version"] = kApplicationConfigSchema;
        root.erase("profiles");
        root.at("runtime")["max_inflight_bytes"] =
            document.application.runtime.max_inflight_bytes;
        auto serialized = root.dump(2) + "\n";
        if (serialized.size() > kMaxConfigDocumentBytes) {
            error = "serialized config document exceeds the 4 MiB limit";
            return false;
        }
        content = std::move(serialized);
        return true;
    } catch (const Json::exception& exception) {
        error = "failed to serialize v3 application settings: "
            + std::string(exception.what());
        return false;
    }
}

} // namespace ccs
