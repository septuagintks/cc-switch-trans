#include "gui_ipc/json_state_types.hpp"

#include <utility>

namespace ccs::gui_ipc::json_detail {

namespace {

bool parse_optional_unsigned(
    const Json& value,
    std::optional<std::uint64_t>& parsed,
    std::string_view path,
    std::string& error) {
    if (value.is_null()) {
        parsed.reset();
        return true;
    }
    std::uint64_t result = 0;
    if (!read_integer(value, result, path, error)) return false;
    parsed = result;
    return true;
}

Json diagnostic_json(const RulesDiagnostic& value) {
    return {
        {"message", value.message}, {"line", value.line},
        {"column", value.column}, {"rule_id", value.rule_id},
        {"rule_type", value.rule_type}, {"option", value.option},
    };
}

bool parse_diagnostic(
    const Json& root,
    RulesDiagnostic& value,
    std::string_view path,
    std::string& error) {
    if (!check_keys(root,
            {"message", "line", "column", "rule_id", "rule_type", "option"},
            {"message", "line", "column", "rule_id", "rule_type", "option"},
            path, error)) {
        return false;
    }
    return read_string(root.at("message"), value.message,
               std::string(path) + ".message", error)
        && read_integer(root.at("line"), value.line,
            std::string(path) + ".line", error)
        && read_integer(root.at("column"), value.column,
            std::string(path) + ".column", error)
        && read_string(root.at("rule_id"), value.rule_id,
            std::string(path) + ".rule_id", error)
        && read_string(root.at("rule_type"), value.rule_type,
            std::string(path) + ".rule_type", error)
        && read_string(root.at("option"), value.option,
            std::string(path) + ".option", error);
}

} // namespace

Json application_json(const ApplicationStatus& value) {
    return {
        {"state", value.state},
        {"listener_host", value.listener_host},
        {"listener_port", value.listener_port},
        {"last_error", value.last_error},
        {"last_exit_code", value.last_exit_code},
    };
}

bool parse_application(
    const Json& root,
    ApplicationStatus& value,
    std::string_view path,
    std::string& error) {
    if (!check_keys(root,
            {"state", "listener_host", "listener_port", "last_error",
                "last_exit_code"},
            {"state", "listener_host", "listener_port", "last_error",
                "last_exit_code"},
            path, error)) {
        return false;
    }
    return read_string(root.at("state"), value.state,
               std::string(path) + ".state", error)
        && read_string(root.at("listener_host"), value.listener_host,
            std::string(path) + ".listener_host", error)
        && read_integer(root.at("listener_port"), value.listener_port,
            std::string(path) + ".listener_port", error)
        && read_string(root.at("last_error"), value.last_error,
            std::string(path) + ".last_error", error)
        && read_integer(root.at("last_exit_code"), value.last_exit_code,
            std::string(path) + ".last_exit_code", error);
}

Json storage_json(const StorageStatus& value) {
    return {
        {"state", value.state},
        {"database_exists", value.database_exists},
        {"detail", value.detail},
        {"database_path", value.database_path},
        {"backup_directory", value.backup_directory},
    };
}

bool parse_storage(
    const Json& root,
    StorageStatus& value,
    std::string_view path,
    std::string& error) {
    if (!check_keys(root,
            {"state", "database_exists", "detail", "database_path",
                "backup_directory"},
            {"state", "database_exists", "detail", "database_path",
                "backup_directory"},
            path, error)) {
        return false;
    }
    return read_string(root.at("state"), value.state,
               std::string(path) + ".state", error)
        && read_bool(root.at("database_exists"), value.database_exists,
            std::string(path) + ".database_exists", error)
        && read_string(root.at("detail"), value.detail,
            std::string(path) + ".detail", error)
        && read_string(root.at("database_path"), value.database_path,
            std::string(path) + ".database_path", error)
        && read_string(root.at("backup_directory"), value.backup_directory,
            std::string(path) + ".backup_directory", error);
}

Json field_state_json(const FieldState& value) {
    return {
        {"key", value.key},
        {"scope", value.scope},
        {"input_kind", value.input_kind},
        {"required", value.required},
        {"minimum", value.minimum ? Json(*value.minimum) : Json(nullptr)},
        {"maximum", value.maximum ? Json(*value.maximum) : Json(nullptr)},
        {"enum_values", value.enum_values},
        {"display_name_key", value.display_name_key},
        {"apply_impact", value.apply_impact},
        {"value", value.value ? field_value_json(*value.value) : Json(nullptr)},
    };
}

bool parse_field_state(
    const Json& root,
    FieldState& value,
    std::string_view path,
    std::string& error) {
    if (!check_keys(root,
            {"key", "scope", "input_kind", "required", "minimum", "maximum",
                "enum_values", "display_name_key", "apply_impact", "value"},
            {"key", "scope", "input_kind", "required", "minimum", "maximum",
                "enum_values", "display_name_key", "apply_impact", "value"},
            path, error)) {
        return false;
    }
    if (!read_string(root.at("key"), value.key,
            std::string(path) + ".key", error)
        || !read_string(root.at("scope"), value.scope,
            std::string(path) + ".scope", error)
        || !read_string(root.at("input_kind"), value.input_kind,
            std::string(path) + ".input_kind", error)
        || !read_bool(root.at("required"), value.required,
            std::string(path) + ".required", error)
        || !parse_optional_unsigned(root.at("minimum"), value.minimum,
            std::string(path) + ".minimum", error)
        || !parse_optional_unsigned(root.at("maximum"), value.maximum,
            std::string(path) + ".maximum", error)
        || !root.at("enum_values").is_array()
        || !read_string(root.at("display_name_key"), value.display_name_key,
            std::string(path) + ".display_name_key", error)
        || !read_string(root.at("apply_impact"), value.apply_impact,
            std::string(path) + ".apply_impact", error)) {
        if (error.empty()) {
            error = std::string(path) + ".enum_values must be a JSON array";
        }
        return false;
    }
    value.enum_values.clear();
    for (std::size_t index = 0; index < root.at("enum_values").size(); ++index) {
        std::string item;
        if (!read_string(root.at("enum_values")[index], item,
                std::string(path) + ".enum_values[" + std::to_string(index)
                    + "]",
                error)) {
            return false;
        }
        value.enum_values.push_back(std::move(item));
    }
    if (root.at("value").is_null()) {
        value.value.reset();
        return true;
    }
    FieldValue parsed;
    if (!parse_field_value(root.at("value"), parsed,
            std::string(path) + ".value", error)) {
        return false;
    }
    value.value = std::move(parsed);
    return true;
}

Json profile_summary_json(const ProfileSummary& value) {
    return {
        {"key", value.key},
        {"id", value.id},
        {"enabled", value.enabled},
        {"protocol", value.protocol ? Json(*value.protocol) : Json(nullptr)},
        {"readiness", value.readiness},
        {"status_detail", value.status_detail},
        {"rule_count", value.rule_count},
        {"enabled_rule_count", value.enabled_rule_count},
    };
}

bool parse_profile_summary(
    const Json& root,
    ProfileSummary& value,
    std::string_view path,
    std::string& error) {
    if (!check_keys(root,
            {"key", "id", "enabled", "protocol", "readiness", "status_detail",
                "rule_count", "enabled_rule_count"},
            {"key", "id", "enabled", "protocol", "readiness", "status_detail",
                "rule_count", "enabled_rule_count"},
            path, error)) {
        return false;
    }
    if (!read_integer(root.at("key"), value.key,
            std::string(path) + ".key", error)
        || !read_string(root.at("id"), value.id,
            std::string(path) + ".id", error)
        || !read_bool(root.at("enabled"), value.enabled,
            std::string(path) + ".enabled", error)
        || !read_string(root.at("readiness"), value.readiness,
            std::string(path) + ".readiness", error)
        || !read_string(root.at("status_detail"), value.status_detail,
            std::string(path) + ".status_detail", error)
        || !read_integer(root.at("rule_count"), value.rule_count,
            std::string(path) + ".rule_count", error)
        || !read_integer(root.at("enabled_rule_count"), value.enabled_rule_count,
            std::string(path) + ".enabled_rule_count", error)) {
        return false;
    }
    if (root.at("protocol").is_null()) {
        value.protocol.reset();
        return true;
    }
    std::string protocol;
    if (!read_string(root.at("protocol"), protocol,
            std::string(path) + ".protocol", error)) {
        return false;
    }
    value.protocol = std::move(protocol);
    return true;
}

Json profile_editor_json(const ProfileEditor& value) {
    Json fields = Json::array();
    for (const auto& field : value.fields) {
        fields.push_back(field_state_json(field));
    }
    return {
        {"key", value.key},
        {"profile_id", value.profile_id},
        {"fields", fields},
    };
}

bool parse_profile_editor(
    const Json& root,
    ProfileEditor& value,
    std::string_view path,
    std::string& error) {
    if (!check_keys(root, {"key", "profile_id", "fields"},
            {"key", "profile_id", "fields"}, path, error)
        || !read_integer(root.at("key"), value.key,
            std::string(path) + ".key", error)
        || !read_string(root.at("profile_id"), value.profile_id,
            std::string(path) + ".profile_id", error)) {
        return false;
    }
    if (!root.at("fields").is_array()) {
        error = std::string(path) + ".fields must be a JSON array";
        return false;
    }
    value.fields.clear();
    for (std::size_t index = 0; index < root.at("fields").size(); ++index) {
        FieldState field;
        if (!parse_field_state(root.at("fields")[index], field,
                std::string(path) + ".fields[" + std::to_string(index) + "]",
                error)) {
            return false;
        }
        value.fields.push_back(std::move(field));
    }
    return true;
}

Json rules_editor_json(const RulesEditor& value) {
    return {
        {"profile_key", value.profile_key},
        {"profile_id", value.profile_id},
        {"text", value.text},
        {"diagnostic", value.diagnostic
                ? diagnostic_json(*value.diagnostic) : Json(nullptr)},
    };
}

bool parse_rules_editor(
    const Json& root,
    RulesEditor& value,
    std::string_view path,
    std::string& error) {
    if (!check_keys(root,
            {"profile_key", "profile_id", "text", "diagnostic"},
            {"profile_key", "profile_id", "text", "diagnostic"}, path, error)
        || !read_integer(root.at("profile_key"), value.profile_key,
            std::string(path) + ".profile_key", error)
        || !read_string(root.at("profile_id"), value.profile_id,
            std::string(path) + ".profile_id", error)
        || !read_string(root.at("text"), value.text,
            std::string(path) + ".text", error)) {
        return false;
    }
    if (root.at("diagnostic").is_null()) {
        value.diagnostic.reset();
        return true;
    }
    RulesDiagnostic diagnostic;
    if (!parse_diagnostic(root.at("diagnostic"), diagnostic,
            std::string(path) + ".diagnostic", error)) {
        return false;
    }
    value.diagnostic = std::move(diagnostic);
    return true;
}

Json draft_json(const DraftStatus& value) {
    return {
        {"phase", value.phase},
        {"runtime_apply_pending", value.runtime_apply_pending},
        {"revision", value.revision},
        {"base_revision", value.base_revision},
    };
}

bool parse_draft(
    const Json& root,
    DraftStatus& value,
    std::string_view path,
    std::string& error) {
    if (!check_keys(root,
            {"phase", "runtime_apply_pending", "revision", "base_revision"},
            {"phase", "runtime_apply_pending", "revision", "base_revision"},
            path, error)) {
        return false;
    }
    return read_string(root.at("phase"), value.phase,
               std::string(path) + ".phase", error)
        && read_bool(root.at("runtime_apply_pending"),
            value.runtime_apply_pending,
            std::string(path) + ".runtime_apply_pending", error)
        && read_integer(root.at("revision"), value.revision,
            std::string(path) + ".revision", error)
        && read_string(root.at("base_revision"), value.base_revision,
            std::string(path) + ".base_revision", error);
}

Json command_status_json(const CommandStatus& value) {
    return {
        {"sequence", value.sequence},
        {"command", value.command},
        {"outcome", result_code_name(value.outcome)},
        {"error", error_code_name(value.error)},
        {"profile_id", value.profile_id},
        {"field", value.field},
        {"detail", value.detail},
        {"recovery_command", value.recovery_command
                ? Json(*value.recovery_command) : Json(nullptr)},
    };
}

bool parse_command_status_json(
    const Json& root,
    CommandStatus& value,
    std::string_view path,
    std::string& error) {
    if (!check_keys(root,
            {"sequence", "command", "outcome", "error", "profile_id", "field",
                "detail", "recovery_command"},
            {"sequence", "command", "outcome", "error", "profile_id", "field",
                "detail", "recovery_command"},
            path, error)) {
        return false;
    }
    std::string outcome;
    std::string error_name;
    if (!read_integer(root.at("sequence"), value.sequence,
            std::string(path) + ".sequence", error)
        || !read_string(root.at("command"), value.command,
            std::string(path) + ".command", error)
        || !read_string(root.at("outcome"), outcome,
            std::string(path) + ".outcome", error)
        || !parse_result_code(outcome, value.outcome)
        || !read_string(root.at("error"), error_name,
            std::string(path) + ".error", error)
        || !parse_error_code(error_name, value.error)
        || !read_string(root.at("profile_id"), value.profile_id,
            std::string(path) + ".profile_id", error)
        || !read_string(root.at("field"), value.field,
            std::string(path) + ".field", error)
        || !read_string(root.at("detail"), value.detail,
            std::string(path) + ".detail", error)) {
        if (error.empty()) {
            error = std::string(path)
                + " contains an unknown outcome or error code";
        }
        return false;
    }
    if (root.at("recovery_command").is_null()) {
        value.recovery_command.reset();
        return true;
    }
    std::string recovery;
    if (!read_string(root.at("recovery_command"), recovery,
            std::string(path) + ".recovery_command", error)) {
        return false;
    }
    value.recovery_command = std::move(recovery);
    return true;
}

Json selection_json(const Selection& value) {
    return {
        {"profile_id", value.profile_id
                ? Json(*value.profile_id) : Json(nullptr)},
        {"profile_key", value.profile_key
                ? Json(*value.profile_key) : Json(nullptr)},
    };
}

bool parse_selection(
    const Json& root,
    Selection& value,
    std::string_view path,
    std::string& error) {
    if (!check_keys(root, {"profile_id", "profile_key"},
            {"profile_id", "profile_key"}, path, error)) {
        return false;
    }
    if (root.at("profile_id").is_null()) {
        value.profile_id.reset();
    } else {
        std::string id;
        if (!read_string(root.at("profile_id"), id,
                std::string(path) + ".profile_id", error)) {
            return false;
        }
        value.profile_id = std::move(id);
    }
    if (root.at("profile_key").is_null()) {
        value.profile_key.reset();
    } else {
        std::int64_t key = 0;
        if (!read_integer(root.at("profile_key"), key,
                std::string(path) + ".profile_key", error)) {
            return false;
        }
        value.profile_key = key;
    }
    if (value.profile_id.has_value() != value.profile_key.has_value()) {
        error = std::string(path)
            + " must contain both profile_id and profile_key or neither";
        return false;
    }
    return true;
}

Json snapshot_json(const Snapshot& value) {
    Json profiles = Json::array();
    for (const auto& profile : value.profiles) {
        profiles.push_back(profile_summary_json(profile));
    }
    Json fields = Json::array();
    for (const auto& field : value.application_fields) {
        fields.push_back(field_state_json(field));
    }
    return {
        {"revision", value.revision},
        {"application", application_json(value.application)},
        {"profiles", std::move(profiles)},
        {"application_fields", std::move(fields)},
        {"selection", selection_json(value.selection)},
        {"profile_editor", value.profile_editor
                ? profile_editor_json(*value.profile_editor) : Json(nullptr)},
        {"rules_editor", value.rules_editor
                ? rules_editor_json(*value.rules_editor) : Json(nullptr)},
        {"draft", draft_json(value.draft)},
        {"last_command", value.last_command
                ? command_status_json(*value.last_command) : Json(nullptr)},
        {"storage", storage_json(value.storage)},
        {"lightweight_mode", value.lightweight_mode},
        {"command_pending", value.command_pending},
    };
}

bool parse_snapshot_json(
    const Json& root,
    Snapshot& value,
    std::string_view path,
    std::string& error) {
    if (!check_keys(root,
            {"revision", "application", "profiles", "application_fields",
                "selection", "profile_editor", "rules_editor", "draft",
                "last_command", "storage", "lightweight_mode", "command_pending"},
            {"revision", "application", "profiles", "application_fields",
                "selection", "profile_editor", "rules_editor", "draft",
                "last_command", "storage", "lightweight_mode", "command_pending"},
            path, error)
        || !read_integer(root.at("revision"), value.revision,
            std::string(path) + ".revision", error)
        || !parse_application(root.at("application"), value.application,
            std::string(path) + ".application", error)
        || !root.at("profiles").is_array()
        || !root.at("application_fields").is_array()
        || !parse_selection(root.at("selection"), value.selection,
            std::string(path) + ".selection", error)
        || !parse_draft(root.at("draft"), value.draft,
            std::string(path) + ".draft", error)
        || !parse_storage(root.at("storage"), value.storage,
            std::string(path) + ".storage", error)
        || !read_bool(root.at("lightweight_mode"), value.lightweight_mode,
            std::string(path) + ".lightweight_mode", error)
        || !read_bool(root.at("command_pending"), value.command_pending,
            std::string(path) + ".command_pending", error)) {
        if (error.empty()) {
            error = std::string(path)
                + " contains a non-array profiles or application_fields";
        }
        return false;
    }
    value.profiles.clear();
    for (std::size_t index = 0; index < root.at("profiles").size(); ++index) {
        ProfileSummary profile;
        if (!parse_profile_summary(root.at("profiles")[index], profile,
                std::string(path) + ".profiles[" + std::to_string(index) + "]",
                error)) {
            return false;
        }
        value.profiles.push_back(std::move(profile));
    }
    value.application_fields.clear();
    for (std::size_t index = 0;
         index < root.at("application_fields").size();
         ++index) {
        FieldState field;
        if (!parse_field_state(root.at("application_fields")[index], field,
                std::string(path) + ".application_fields["
                    + std::to_string(index) + "]",
                error)) {
            return false;
        }
        value.application_fields.push_back(std::move(field));
    }
    if (root.at("profile_editor").is_null()) {
        value.profile_editor.reset();
    } else {
        ProfileEditor editor;
        if (!parse_profile_editor(root.at("profile_editor"), editor,
                std::string(path) + ".profile_editor", error)) {
            return false;
        }
        value.profile_editor = std::move(editor);
    }
    if (root.at("rules_editor").is_null()) {
        value.rules_editor.reset();
    } else {
        RulesEditor editor;
        if (!parse_rules_editor(root.at("rules_editor"), editor,
                std::string(path) + ".rules_editor", error)) {
            return false;
        }
        value.rules_editor = std::move(editor);
    }
    if (root.at("last_command").is_null()) {
        value.last_command.reset();
    } else {
        CommandStatus status;
        if (!parse_command_status_json(root.at("last_command"), status,
                std::string(path) + ".last_command", error)) {
            return false;
        }
        value.last_command = std::move(status);
    }
    return true;
}

} // namespace ccs::gui_ipc::json_detail
