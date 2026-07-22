#include "gui_ipc/json_codec.hpp"

#include "gui_ipc/json_state_types.hpp"

#include <utility>

namespace ccs::gui_ipc {

using namespace json_detail;

bool serialize_command(
    const Command& value,
    std::string& content,
    std::string& error) {
    return serialize_with([&] {
        Json edits = Json::array();
        for (const auto& edit : value.field_edits) {
            edits.push_back({
                {"key", edit.key},
                {"value", edit.value
                        ? field_value_json(*edit.value) : Json(nullptr)},
            });
        }
        return Json{
            {"command", gui_command_name(value.command)},
            {"profile_id", value.profile_id},
            {"replacement_profile_id", value.replacement_profile_id},
            {"profile_key", value.profile_key
                    ? Json(*value.profile_key) : Json(nullptr)},
            {"enabled", value.enabled},
            {"position", value.position},
            {"field_edits", std::move(edits)},
            {"text", value.text},
            {"expected_draft_revision", value.expected_draft_revision
                    ? Json(*value.expected_draft_revision) : Json(nullptr)},
            {"expected_base_revision", value.expected_base_revision
                    ? Json(*value.expected_base_revision) : Json(nullptr)},
            {"unsaved_decision", value.unsaved_decision
                    ? Json(unsaved_decision_name(*value.unsaved_decision))
                    : Json(nullptr)},
            {"replace_existing_storage", value.replace_existing_storage},
            {"replacement_confirmation", value.replacement_confirmation},
        };
    }, content, error);
}

bool parse_command(
    std::string_view content,
    Command& value,
    std::string& error) {
    return parse_payload(content, value, error,
        [](const Json& root, Command& parsed, std::string_view path,
            std::string& failure) {
            if (!check_keys(root,
                    {"command", "profile_id", "replacement_profile_id",
                        "profile_key", "enabled", "position", "field_edits",
                        "text", "expected_draft_revision",
                        "expected_base_revision", "unsaved_decision",
                        "replace_existing_storage", "replacement_confirmation"},
                    {"command", "profile_id", "replacement_profile_id",
                        "profile_key", "enabled", "position", "field_edits",
                        "text", "expected_draft_revision",
                        "expected_base_revision", "unsaved_decision",
                        "replace_existing_storage", "replacement_confirmation"},
                    path, failure)) {
                return false;
            }
            std::string command;
            if (!read_string(
                    root.at("command"), command, "$.command", failure)
                || !parse_gui_command(command, parsed.command)
                || !read_string(root.at("profile_id"), parsed.profile_id,
                    "$.profile_id", failure)
                || !read_string(root.at("replacement_profile_id"),
                    parsed.replacement_profile_id,
                    "$.replacement_profile_id", failure)
                || !read_bool(root.at("enabled"), parsed.enabled,
                    "$.enabled", failure)
                || !read_integer(root.at("position"), parsed.position,
                    "$.position", failure)
                || !root.at("field_edits").is_array()
                || !read_string(root.at("text"), parsed.text,
                    "$.text", failure)
                || !read_bool(root.at("replace_existing_storage"),
                    parsed.replace_existing_storage,
                    "$.replace_existing_storage", failure)
                || !read_string(root.at("replacement_confirmation"),
                    parsed.replacement_confirmation,
                    "$.replacement_confirmation", failure)) {
                if (failure.empty()) {
                    failure = !parse_gui_command(command, parsed.command)
                        ? "$.command contains an unknown GUI command"
                        : "$.field_edits must be a JSON array";
                }
                return false;
            }
            if (!root.at("profile_key").is_null()) {
                std::int64_t key = 0;
                if (!read_integer(
                        root.at("profile_key"), key, "$.profile_key", failure)) {
                    return false;
                }
                parsed.profile_key = key;
            }
            if (!root.at("expected_draft_revision").is_null()) {
                std::uint64_t revision = 0;
                if (!read_integer(root.at("expected_draft_revision"), revision,
                        "$.expected_draft_revision", failure)) {
                    return false;
                }
                parsed.expected_draft_revision = revision;
            }
            if (!root.at("expected_base_revision").is_null()) {
                std::string revision;
                if (!read_string(root.at("expected_base_revision"), revision,
                        "$.expected_base_revision", failure)) {
                    return false;
                }
                parsed.expected_base_revision = std::move(revision);
            }
            if (!root.at("unsaved_decision").is_null()) {
                std::string decision;
                UnsavedDecision parsed_decision;
                if (!read_string(root.at("unsaved_decision"), decision,
                        "$.unsaved_decision", failure)
                    || !parse_unsaved_decision(decision, parsed_decision)) {
                    if (failure.empty()) {
                        failure = "$.unsaved_decision is unknown";
                    }
                    return false;
                }
                parsed.unsaved_decision = parsed_decision;
            }
            for (std::size_t index = 0;
                 index < root.at("field_edits").size();
                 ++index) {
                const auto& item = root.at("field_edits")[index];
                const auto item_path = "$.field_edits["
                    + std::to_string(index) + "]";
                if (!check_keys(item, {"key", "value"}, {"key", "value"},
                        item_path, failure)) {
                    return false;
                }
                FieldEdit edit;
                if (!read_string(item.at("key"), edit.key,
                        item_path + ".key", failure)) {
                    return false;
                }
                if (!item.at("value").is_null()) {
                    FieldValue field_value;
                    if (!parse_field_value(item.at("value"), field_value,
                            item_path + ".value", failure)) {
                        return false;
                    }
                    edit.value = std::move(field_value);
                }
                parsed.field_edits.push_back(std::move(edit));
            }
            return true;
        });
}

bool serialize_command_status(
    const CommandStatus& value,
    std::string& content,
    std::string& error) {
    return serialize_with(
        [&] { return command_status_json(value); }, content, error);
}

bool parse_command_status(
    std::string_view content,
    CommandStatus& value,
    std::string& error) {
    return parse_payload(content, value, error, parse_command_status_json);
}

bool serialize_maintenance_request(
    const MaintenanceRequest& value,
    std::string& content,
    std::string& error) {
    return serialize_with([&] { return Json{
        {"command", maintenance_command_name(value.command)},
        {"timeout_ms", value.timeout_ms},
    }; }, content, error);
}

bool parse_maintenance_request(
    std::string_view content,
    MaintenanceRequest& value,
    std::string& error) {
    return parse_payload(content, value, error,
        [](const Json& root, MaintenanceRequest& parsed, std::string_view path,
            std::string& failure) {
            std::string command;
            if (!check_keys(root, {"command", "timeout_ms"},
                    {"command", "timeout_ms"}, path, failure)
                || !read_string(root.at("command"), command,
                    "$.command", failure)
                || !parse_maintenance_command(command, parsed.command)
                || !read_integer(root.at("timeout_ms"), parsed.timeout_ms,
                    "$.timeout_ms", failure)) {
                if (failure.empty()) {
                    failure = "$.command contains an unknown maintenance command";
                }
                return false;
            }
            return true;
        });
}

bool serialize_maintenance_result(
    const MaintenanceResult& value,
    std::string& content,
    std::string& error) {
    return serialize_with([&] { return Json{
        {"succeeded", value.succeeded},
        {"version", value.version},
        {"source_commit", value.source_commit},
        {"state", value.state},
        {"detail", value.detail},
    }; }, content, error);
}

bool parse_maintenance_result(
    std::string_view content,
    MaintenanceResult& value,
    std::string& error) {
    return parse_payload(content, value, error,
        [](const Json& root, MaintenanceResult& parsed, std::string_view path,
            std::string& failure) {
            return check_keys(root,
                       {"succeeded", "version", "source_commit", "state",
                           "detail"},
                       {"succeeded", "version", "source_commit", "state",
                           "detail"},
                       path, failure)
                && read_bool(root.at("succeeded"), parsed.succeeded,
                    "$.succeeded", failure)
                && read_string(root.at("version"), parsed.version,
                    "$.version", failure)
                && read_string(root.at("source_commit"), parsed.source_commit,
                    "$.source_commit", failure)
                && read_string(root.at("state"), parsed.state,
                    "$.state", failure)
                && read_string(root.at("detail"), parsed.detail,
                    "$.detail", failure);
        });
}

} // namespace ccs::gui_ipc
