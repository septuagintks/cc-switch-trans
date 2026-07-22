#include "gui_ipc/json_codec.hpp"

#include "gui_ipc/json_state_types.hpp"

#include <utility>
#include <vector>

namespace ccs::gui_ipc {

using namespace json_detail;

bool serialize_snapshot(
    const Snapshot& value,
    std::string& content,
    std::string& error) {
    return serialize_with([&] { return snapshot_json(value); }, content, error);
}

bool parse_snapshot(
    std::string_view content,
    Snapshot& value,
    std::string& error) {
    return parse_payload(content, value, error, parse_snapshot_json);
}

bool serialize_state_delta(
    const StateDelta& value,
    std::string& content,
    std::string& error) {
    if (value.revision <= value.from_revision || value.empty()) {
        error = "state delta must be non-empty and advance revision";
        return false;
    }
    return serialize_with([&] {
        Json root{
            {"from_revision", value.from_revision},
            {"revision", value.revision},
        };
        if (value.application) {
            root["application"] = application_json(*value.application);
        }
        if (value.profiles) {
            root["profiles"] = Json::array();
            for (const auto& item : *value.profiles) {
                root["profiles"].push_back(profile_summary_json(item));
            }
        }
        if (value.application_fields) {
            root["application_fields"] = Json::array();
            for (const auto& item : *value.application_fields) {
                root["application_fields"].push_back(field_state_json(item));
            }
        }
        if (value.selection) {
            root["selection"] = selection_json(*value.selection);
        }
        if (value.profile_editor_changed) {
            root["profile_editor"] = value.profile_editor
                ? profile_editor_json(*value.profile_editor) : Json(nullptr);
        }
        if (value.rules_editor_changed) {
            root["rules_editor"] = value.rules_editor
                ? rules_editor_json(*value.rules_editor) : Json(nullptr);
        }
        if (value.draft) root["draft"] = draft_json(*value.draft);
        if (value.last_command_changed) {
            root["last_command"] = value.last_command
                ? command_status_json(*value.last_command) : Json(nullptr);
        }
        if (value.lightweight_mode) {
            root["lightweight_mode"] = *value.lightweight_mode;
        }
        if (value.command_pending) {
            root["command_pending"] = *value.command_pending;
        }
        return root;
    }, content, error);
}

bool parse_state_delta(
    std::string_view content,
    StateDelta& value,
    std::string& error) {
    return parse_payload(content, value, error,
        [](const Json& root, StateDelta& parsed, std::string_view path,
            std::string& failure) {
            if (!check_keys(root,
                    {"from_revision", "revision", "application", "profiles",
                        "application_fields", "selection", "profile_editor",
                        "rules_editor", "draft", "last_command",
                        "lightweight_mode", "command_pending"},
                    {"from_revision", "revision"}, path, failure)
                || !read_integer(root.at("from_revision"), parsed.from_revision,
                    "$.from_revision", failure)
                || !read_integer(root.at("revision"), parsed.revision,
                    "$.revision", failure)) {
                return false;
            }
            if (root.contains("application")) {
                ApplicationStatus status;
                if (!parse_application(
                        root.at("application"), status, "$.application", failure)) {
                    return false;
                }
                parsed.application = std::move(status);
            }
            if (root.contains("profiles")) {
                if (!root.at("profiles").is_array()) {
                    failure = "$.profiles must be a JSON array";
                    return false;
                }
                std::vector<ProfileSummary> profiles;
                for (std::size_t index = 0;
                     index < root.at("profiles").size();
                     ++index) {
                    ProfileSummary profile;
                    if (!parse_profile_summary(
                            root.at("profiles")[index],
                            profile,
                            "$.profiles[" + std::to_string(index) + "]",
                            failure)) {
                        return false;
                    }
                    profiles.push_back(std::move(profile));
                }
                parsed.profiles = std::move(profiles);
            }
            if (root.contains("application_fields")) {
                if (!root.at("application_fields").is_array()) {
                    failure = "$.application_fields must be a JSON array";
                    return false;
                }
                std::vector<FieldState> fields;
                for (std::size_t index = 0;
                     index < root.at("application_fields").size();
                     ++index) {
                    FieldState field;
                    if (!parse_field_state(
                            root.at("application_fields")[index],
                            field,
                            "$.application_fields[" + std::to_string(index) + "]",
                            failure)) {
                        return false;
                    }
                    fields.push_back(std::move(field));
                }
                parsed.application_fields = std::move(fields);
            }
            if (root.contains("selection")) {
                Selection selection;
                if (!parse_selection(
                        root.at("selection"), selection, "$.selection", failure)) {
                    return false;
                }
                parsed.selection = std::move(selection);
            }
            if (root.contains("profile_editor")) {
                parsed.profile_editor_changed = true;
                if (!root.at("profile_editor").is_null()) {
                    ProfileEditor editor;
                    if (!parse_profile_editor(root.at("profile_editor"), editor,
                            "$.profile_editor", failure)) {
                        return false;
                    }
                    parsed.profile_editor = std::move(editor);
                }
            }
            if (root.contains("rules_editor")) {
                parsed.rules_editor_changed = true;
                if (!root.at("rules_editor").is_null()) {
                    RulesEditor editor;
                    if (!parse_rules_editor(root.at("rules_editor"), editor,
                            "$.rules_editor", failure)) {
                        return false;
                    }
                    parsed.rules_editor = std::move(editor);
                }
            }
            if (root.contains("draft")) {
                DraftStatus draft;
                if (!parse_draft(root.at("draft"), draft, "$.draft", failure)) {
                    return false;
                }
                parsed.draft = std::move(draft);
            }
            if (root.contains("last_command")) {
                parsed.last_command_changed = true;
                if (!root.at("last_command").is_null()) {
                    CommandStatus status;
                    if (!parse_command_status_json(
                            root.at("last_command"), status,
                            "$.last_command", failure)) {
                        return false;
                    }
                    parsed.last_command = std::move(status);
                }
            }
            if (root.contains("lightweight_mode")) {
                bool setting = false;
                if (!read_bool(root.at("lightweight_mode"), setting,
                        "$.lightweight_mode", failure)) {
                    return false;
                }
                parsed.lightweight_mode = setting;
            }
            if (root.contains("command_pending")) {
                bool pending = false;
                if (!read_bool(root.at("command_pending"), pending,
                        "$.command_pending", failure)) {
                    return false;
                }
                parsed.command_pending = pending;
            }
            if (parsed.revision <= parsed.from_revision || parsed.empty()) {
                failure = "state delta must be non-empty and advance revision";
                return false;
            }
            return true;
        });
}

} // namespace ccs::gui_ipc
