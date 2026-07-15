#include "presentation/ui_preferences.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ccs {

namespace {

using Json = nlohmann::json;

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
        if (!allowed_keys.contains(it.key())) {
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

} // namespace

UiPreferences make_default_ui_preferences() {
    return UiPreferences{};
}

bool parse_ui_preferences(
    std::string_view content,
    UiPreferences& preferences,
    std::string& error) {
    error.clear();
    if (content.size() > kMaxUiPreferencesBytes) {
        error = "UI preferences exceed the 64 KiB limit";
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
            error = "UI preferences contain duplicate JSON object key: " + duplicate_key;
            return false;
        }
        if (!check_object_keys(
                root,
                "$",
                {"schema_version", "main_window"},
                {"schema_version", "main_window"},
                error)) {
            return false;
        }
        if (!root.at("schema_version").is_string()
            || root.at("schema_version").get<std::string>() != kUiPreferencesSchemaVersion) {
            error = "unsupported UI preference schema_version; expected ccs-trans.ui/v1";
            return false;
        }
        const auto& main_window = root.at("main_window");
        if (!check_object_keys(
                main_window,
                "$.main_window",
                {"lightweight_mode"},
                {"lightweight_mode"},
                error)) {
            return false;
        }
        if (!main_window.at("lightweight_mode").is_boolean()) {
            error = "$.main_window.lightweight_mode must be a JSON boolean";
            return false;
        }

        UiPreferences candidate;
        candidate.lightweight_mode = main_window.at("lightweight_mode").get<bool>();
        preferences = std::move(candidate);
        return true;
    } catch (const Json::exception& ex) {
        error = "failed to parse UI preferences: " + std::string(ex.what());
        return false;
    } catch (const std::exception& ex) {
        error = "failed to load UI preferences: " + std::string(ex.what());
        return false;
    }
}

bool serialize_ui_preferences(
    const UiPreferences& preferences,
    std::string& content,
    std::string& error) {
    error.clear();
    try {
        Json root = {
            {"schema_version", kUiPreferencesSchemaVersion},
            {"main_window", {
                {"lightweight_mode", preferences.lightweight_mode},
            }},
        };
        auto serialized = root.dump(2) + "\n";
        if (serialized.size() > kMaxUiPreferencesBytes) {
            error = "serialized UI preferences exceed the 64 KiB limit";
            return false;
        }
        content = std::move(serialized);
        return true;
    } catch (const Json::exception& ex) {
        error = "failed to serialize UI preferences: " + std::string(ex.what());
        return false;
    }
}

} // namespace ccs
