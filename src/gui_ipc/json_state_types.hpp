#pragma once

#include "gui_ipc/json_codec_support.hpp"

namespace ccs::gui_ipc::json_detail {

Json application_json(const ApplicationStatus& value);
bool parse_application(
    const Json& root,
    ApplicationStatus& value,
    std::string_view path,
    std::string& error);

Json field_state_json(const FieldState& value);
bool parse_field_state(
    const Json& root,
    FieldState& value,
    std::string_view path,
    std::string& error);

Json profile_summary_json(const ProfileSummary& value);
bool parse_profile_summary(
    const Json& root,
    ProfileSummary& value,
    std::string_view path,
    std::string& error);

Json profile_editor_json(const ProfileEditor& value);
bool parse_profile_editor(
    const Json& root,
    ProfileEditor& value,
    std::string_view path,
    std::string& error);

Json rules_editor_json(const RulesEditor& value);
bool parse_rules_editor(
    const Json& root,
    RulesEditor& value,
    std::string_view path,
    std::string& error);

Json draft_json(const DraftStatus& value);
bool parse_draft(
    const Json& root,
    DraftStatus& value,
    std::string_view path,
    std::string& error);

Json command_status_json(const CommandStatus& value);
bool parse_command_status_json(
    const Json& root,
    CommandStatus& value,
    std::string_view path,
    std::string& error);

Json selection_json(const Selection& value);
bool parse_selection(
    const Json& root,
    Selection& value,
    std::string_view path,
    std::string& error);

Json snapshot_json(const Snapshot& value);
bool parse_snapshot_json(
    const Json& root,
    Snapshot& value,
    std::string_view path,
    std::string& error);

} // namespace ccs::gui_ipc::json_detail
