#include "config/rules_text.hpp"

#include "config/config_document.hpp"
#include "rules/rule_registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ccs {

namespace {

using Json = nlohmann::ordered_json;

std::pair<std::size_t, std::size_t> line_column(
    std::string_view content,
    std::size_t offset) {
    offset = std::min(offset, content.size());
    std::size_t line = 1;
    std::size_t column = 1;
    for (std::size_t index = 0; index < offset; ++index) {
        if (content[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return {line, column};
}

void set_error(
    RulesTextError& error,
    std::string message,
    std::string_view content,
    std::size_t offset = 0,
    std::string rule_id = {},
    std::string rule_type = {},
    std::string option = {}) {
    const auto [line, column] = line_column(content, offset);
    error.message = std::move(message);
    error.line = line;
    error.column = column;
    error.rule_id = std::move(rule_id);
    error.rule_type = std::move(rule_type);
    error.option = std::move(option);
}

std::size_t locate_value(std::string_view content, std::string_view value) {
    const auto found = content.find(value);
    return found == std::string_view::npos ? 0 : found;
}

bool exact_keys(
    const Json& object,
    const std::set<std::string>& expected,
    std::string_view context,
    std::string& message) {
    if (!object.is_object()) {
        message = std::string(context) + " must be a JSON object";
        return false;
    }
    for (auto item = object.begin(); item != object.end(); ++item) {
        if (expected.count(item.key()) == 0) {
            message = std::string(context) + " contains unknown field: " + item.key();
            return false;
        }
    }
    for (const auto& key : expected) {
        if (!object.contains(key)) {
            message = std::string(context) + " is missing required field: " + key;
            return false;
        }
    }
    return true;
}

bool parse_strict_json(
    std::string_view content,
    Json& root,
    RulesTextError& error) {
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
            set_error(
                error,
                "rules text contains duplicate JSON object key: " + duplicate_key,
                content,
                locate_value(content, "\"" + duplicate_key + "\""));
            return false;
        }
        return true;
    } catch (const Json::parse_error& exception) {
        const auto offset = exception.byte == 0
            ? 0
            : static_cast<std::size_t>(exception.byte - 1);
        set_error(
            error,
            "failed to parse rules text: " + std::string(exception.what()),
            content,
            offset);
        return false;
    } catch (const Json::exception& exception) {
        set_error(
            error,
            "failed to parse rules text: " + std::string(exception.what()),
            content);
        return false;
    }
}

bool validate_known_options(
    const RuleDescriptor& descriptor,
    const Json& options,
    bool enabled,
    std::string_view content,
    std::string_view rule_id,
    RulesTextError& error) {
    std::unordered_map<std::string_view, const RuleOptionDescriptor*> by_name;
    for (const auto& option : descriptor.options) {
        by_name.emplace(option.name, &option);
    }
    for (auto item = options.begin(); item != options.end(); ++item) {
        const auto found = by_name.find(item.key());
        if (found == by_name.end()) {
            set_error(
                error,
                "rule " + std::string(rule_id) + " (" + descriptor.type
                    + ") has unknown option: " + item.key(),
                content,
                locate_value(content, "\"" + item.key() + "\""),
                std::string(rule_id),
                descriptor.type,
                item.key());
            return false;
        }
        const auto& option = *found->second;
        const bool valid = option.value_type == RuleOptionValueType::JsonValue
            || ((option.value_type == RuleOptionValueType::String
                    || option.value_type == RuleOptionValueType::JsonPointer)
                && item.value().is_string());
        if (!valid) {
            set_error(
                error,
                "rule " + std::string(rule_id) + " option " + option.name
                    + " must be " + rule_option_value_type_name(option.value_type),
                content,
                locate_value(content, "\"" + option.name + "\""),
                std::string(rule_id),
                descriptor.type,
                option.name);
            return false;
        }
        if (option.value_type == RuleOptionValueType::JsonPointer) {
            try {
                (void)nlohmann::json::json_pointer(item.value().get<std::string>());
            } catch (const nlohmann::json::exception&) {
                set_error(
                    error,
                    "rule " + std::string(rule_id) + " option " + option.name
                        + " must be a valid JSON pointer",
                    content,
                    locate_value(content, "\"" + option.name + "\""),
                    std::string(rule_id),
                    descriptor.type,
                    option.name);
                return false;
            }
        }
    }
    if (enabled) {
        for (const auto& option : descriptor.options) {
            if (option.required && !options.contains(option.name)) {
                set_error(
                    error,
                    "rule " + std::string(rule_id) + " (" + descriptor.type
                        + ") is missing required option: " + option.name,
                    content,
                    locate_value(content, std::string(rule_id)),
                    std::string(rule_id),
                    descriptor.type,
                    option.name);
                return false;
            }
        }
    }
    return true;
}

Json sorted_options(const nlohmann::json& options) {
    Json result = Json::object();
    for (auto item = options.begin(); item != options.end(); ++item) {
        result[item.key()] = item.value();
    }
    return result;
}

} // namespace

bool parse_rules_text(
    std::string_view content,
    const std::vector<StoredRule>& existing,
    std::vector<StoredRule>& rules,
    RulesTextError& error) {
    error = {};
    rules.clear();
    if (content.size() > kMaxStoredProfileRulesTextBytes) {
        set_error(error, "rules text exceeds the 4 MiB limit", content);
        return false;
    }

    Json root;
    if (!parse_strict_json(content, root, error)) {
        return false;
    }
    std::string structural_error;
    if (!exact_keys(root, {"rules", "schema_version"}, "rules text root", structural_error)) {
        set_error(error, structural_error, content);
        return false;
    }
    if (!root.at("schema_version").is_string()
        || root.at("schema_version").get<std::string_view>() != kRulesTextSchema) {
        set_error(
            error,
            "unsupported rules text schema_version; expected ccs-trans.rules/v1",
            content,
            locate_value(content, "schema_version"));
        return false;
    }
    if (!root.at("rules").is_array()) {
        set_error(error, "rules text rules field must be an array", content);
        return false;
    }
    if (root.at("rules").size() > kMaxRulesPerProfile) {
        set_error(error, "rules text exceeds the maximum rule count", content);
        return false;
    }

    std::unordered_map<std::string, RuleKey> existing_keys;
    for (const auto& rule : existing) {
        existing_keys.emplace(rule.rule_id, rule.key);
    }
    std::unordered_set<std::string> ids;
    const auto registry = builtin_rule_registry();
    rules.reserve(root.at("rules").size());
    for (const auto& item : root.at("rules")) {
        if (!exact_keys(item, {"enabled", "id", "options", "type"},
                "rules text rule", structural_error)) {
            set_error(error, structural_error, content);
            return false;
        }
        if (!item.at("id").is_string() || !item.at("enabled").is_boolean()
            || !item.at("type").is_string() || !item.at("options").is_object()) {
            set_error(error, "rules text rule fields have invalid JSON types", content);
            return false;
        }
        const auto id = item.at("id").get<std::string>();
        const auto type = item.at("type").get<std::string>();
        const bool enabled = item.at("enabled").get<bool>();
        const auto location = locate_value(content, id);
        if (!is_valid_rule_id(id)) {
            set_error(error, "invalid rule id: " + id, content, location, id, type);
            return false;
        }
        if (!is_valid_rule_type(type)) {
            set_error(error, "rule " + id + " has an invalid type", content, location, id, type);
            return false;
        }
        if (!ids.emplace(id).second) {
            set_error(error, "duplicate rule id: " + id, content, location, id, type);
            return false;
        }
        if (const auto* descriptor = registry->find_descriptor(type);
            descriptor != nullptr
            && !validate_known_options(
                *descriptor, item.at("options"), enabled, content, id, error)) {
            return false;
        }

        const auto normalized_options = nlohmann::json::parse(item.at("options").dump());
        StoredRule rule;
        if (const auto found = existing_keys.find(id); found != existing_keys.end()) {
            rule.key = found->second;
        }
        rule.rule_id = id;
        rule.enabled = enabled;
        rule.type = type;
        rule.options_json = normalized_options.dump();
        rules.push_back(std::move(rule));
    }
    return true;
}

bool format_rules_text(
    const std::vector<StoredRule>& rules,
    std::string& content,
    RulesTextError& error) {
    error = {};
    Json root = Json::object();
    root["schema_version"] = kRulesTextSchema;
    root["rules"] = Json::array();
    std::unordered_set<std::string> ids;
    try {
        for (const auto& rule : rules) {
            if (!is_valid_rule_id(rule.rule_id) || !is_valid_rule_type(rule.type)
                || !ids.emplace(rule.rule_id).second) {
                set_error(
                    error,
                    "stored rules contain an invalid or duplicate rule: " + rule.rule_id,
                    {});
                return false;
            }
            const auto parsed_options = nlohmann::json::parse(rule.options_json);
            if (!parsed_options.is_object()) {
                set_error(
                    error,
                    "stored rule options must be a JSON object: " + rule.rule_id,
                    {}, 0, rule.rule_id, rule.type);
                return false;
            }
            Json item = Json::object();
            item["id"] = rule.rule_id;
            item["enabled"] = rule.enabled;
            item["type"] = rule.type;
            item["options"] = sorted_options(parsed_options);
            root["rules"].push_back(std::move(item));
        }
        content = root.dump(2, ' ', false, Json::error_handler_t::strict) + "\n";
        if (content.size() > kMaxStoredProfileRulesTextBytes) {
            set_error(error, "formatted rules text exceeds the 4 MiB limit", {});
            return false;
        }
        return true;
    } catch (const nlohmann::json::exception& exception) {
        set_error(
            error,
            "failed to format stored rules: " + std::string(exception.what()),
            {});
        return false;
    }
}

} // namespace ccs
