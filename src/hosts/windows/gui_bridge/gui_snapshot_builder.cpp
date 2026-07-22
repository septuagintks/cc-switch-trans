#include "hosts/windows/gui_bridge/gui_snapshot_builder.hpp"

#ifdef _WIN32

#include <utility>

namespace ccs {

namespace {

gui_ipc::FieldValue convert_value(const ConfigurationFieldValue& value) {
    return std::visit([](const auto& item) -> gui_ipc::FieldValue {
        return item;
    }, value);
}

gui_ipc::FieldState convert_field(const ConfigurationFieldState& field) {
    gui_ipc::FieldState converted;
    converted.key = field.key;
    converted.scope = field.scope == ConfigurationFieldScope::Application
        ? "application" : "profile";
    converted.input_kind = configuration_field_input_kind_name(field.input_kind);
    converted.required = field.required;
    converted.minimum = field.minimum;
    converted.maximum = field.maximum;
    converted.enum_values = field.enum_values;
    converted.display_name_key = field.display_name_key;
    converted.apply_impact = runtime_apply_impact_name(field.apply_impact);
    if (field.value) {
        converted.value = convert_value(*field.value);
    }
    return converted;
}

std::vector<gui_ipc::FieldState> convert_fields(
    const std::vector<ConfigurationFieldState>& fields) {
    std::vector<gui_ipc::FieldState> converted;
    converted.reserve(fields.size());
    for (const auto& field : fields) {
        converted.push_back(convert_field(field));
    }
    return converted;
}

gui_ipc::ApplicationStatus convert_application(const ApplicationStatus& status) {
    return {
        application_state_name(status.state),
        status.listener_host,
        status.listener_port,
        status.last_error,
        status.last_exit_code,
    };
}

gui_ipc::ProfileSummary convert_profile(const ProfileListItem& profile) {
    return {
        profile.key,
        profile.id,
        profile.enabled,
        profile.protocol,
        profile_readiness_name(profile.readiness),
        profile.status_detail,
        profile.rule_count,
        profile.enabled_rule_count,
    };
}

gui_ipc::ProfileEditor convert_profile_editor(const ProfileEditorState& editor) {
    return {editor.key, editor.profile_id, convert_fields(editor.fields)};
}

gui_ipc::RulesEditor convert_rules_editor(const RulesEditorState& editor) {
    gui_ipc::RulesEditor converted{
        editor.profile_key,
        editor.profile_id,
        editor.text,
        std::nullopt,
    };
    if (editor.diagnostic) {
        converted.diagnostic = gui_ipc::RulesDiagnostic{
            editor.diagnostic->message,
            editor.diagnostic->line,
            editor.diagnostic->column,
            editor.diagnostic->rule_id,
            editor.diagnostic->rule_type,
            editor.diagnostic->option,
        };
    }
    return converted;
}

gui_ipc::DraftStatus convert_draft(const DraftState& draft) {
    return {
        draft_phase_name(draft.phase),
        draft.runtime_apply_pending,
        draft.revision,
        draft.base_revision,
    };
}

} // namespace

gui_ipc::ErrorCode gui_error_code(MainWindowError error) noexcept {
    switch (error) {
    case MainWindowError::None: return gui_ipc::ErrorCode::None;
    case MainWindowError::Busy: return gui_ipc::ErrorCode::Busy;
    case MainWindowError::InvalidArgument: return gui_ipc::ErrorCode::InvalidArgument;
    case MainWindowError::ProfileNotFound: return gui_ipc::ErrorCode::ProfileNotFound;
    case MainWindowError::ProfileAlreadyExists:
        return gui_ipc::ErrorCode::ProfileAlreadyExists;
    case MainWindowError::ValidationFailed: return gui_ipc::ErrorCode::ValidationFailed;
    case MainWindowError::RouteCollision: return gui_ipc::ErrorCode::RouteCollision;
    case MainWindowError::RepositoryStale: return gui_ipc::ErrorCode::RepositoryStale;
    case MainWindowError::DraftStale: return gui_ipc::ErrorCode::DraftStale;
    case MainWindowError::PersistenceFailed: return gui_ipc::ErrorCode::PersistenceFailed;
    case MainWindowError::RuntimeApplyFailed: return gui_ipc::ErrorCode::RuntimeApplyFailed;
    case MainWindowError::ServiceUnavailable: return gui_ipc::ErrorCode::ServiceUnavailable;
    case MainWindowError::UnsavedChangesDecisionRequired:
        return gui_ipc::ErrorCode::UnsavedChangesDecisionRequired;
    case MainWindowError::Cancelled: return gui_ipc::ErrorCode::None;
    case MainWindowError::Internal: return gui_ipc::ErrorCode::Internal;
    }
    return gui_ipc::ErrorCode::Internal;
}

gui_ipc::ResultCode gui_result_code(CommandOutcome outcome) noexcept {
    switch (outcome) {
    case CommandOutcome::Succeeded: return gui_ipc::ResultCode::Succeeded;
    case CommandOutcome::Rejected: return gui_ipc::ResultCode::Rejected;
    case CommandOutcome::Failed: return gui_ipc::ResultCode::Failed;
    case CommandOutcome::Cancelled: return gui_ipc::ResultCode::Cancelled;
    case CommandOutcome::SavedPendingRuntimeApply:
        return gui_ipc::ResultCode::SavedPendingRuntimeApply;
    }
    return gui_ipc::ResultCode::Failed;
}

gui_ipc::CommandStatus build_gui_command_status(
    const CommandResult& result,
    std::uint64_t client_sequence) {
    gui_ipc::CommandStatus status;
    status.sequence = client_sequence;
    status.command = main_window_command_name(result.command);
    status.outcome = gui_result_code(result.outcome);
    status.error = gui_error_code(result.error);
    status.profile_id = result.profile_id;
    status.field = result.field;
    status.detail = result.detail;
    if (result.recovery_command) {
        status.recovery_command = main_window_command_name(*result.recovery_command);
    }
    return status;
}

gui_ipc::Snapshot build_gui_snapshot(const MainWindowState& state) {
    gui_ipc::Snapshot snapshot;
    snapshot.revision = state.revision;
    snapshot.application = convert_application(state.application);
    snapshot.profiles.reserve(state.profiles.size());
    for (const auto& profile : state.profiles) {
        snapshot.profiles.push_back(convert_profile(profile));
    }
    snapshot.application_fields = convert_fields(state.application_fields);
    snapshot.selection = {state.selected_profile_id, state.selected_profile_key};
    if (state.profile_editor) {
        snapshot.profile_editor = convert_profile_editor(*state.profile_editor);
    }
    if (state.rules_editor) {
        snapshot.rules_editor = convert_rules_editor(*state.rules_editor);
    }
    snapshot.draft = convert_draft(state.draft);
    if (state.last_command) {
        snapshot.last_command = build_gui_command_status(
            *state.last_command, state.last_command->sequence);
    }
    snapshot.lightweight_mode = state.lightweight_mode;
    snapshot.command_pending = state.command_pending;
    return snapshot;
}

gui_ipc::StateDelta build_gui_state_delta(
    const gui_ipc::Snapshot& previous,
    const gui_ipc::Snapshot& current) {
    gui_ipc::StateDelta delta;
    delta.from_revision = previous.revision;
    delta.revision = current.revision;
    if (previous.application != current.application) delta.application = current.application;
    if (previous.profiles != current.profiles) delta.profiles = current.profiles;
    if (previous.application_fields != current.application_fields) {
        delta.application_fields = current.application_fields;
    }
    if (previous.selection != current.selection) delta.selection = current.selection;
    if (previous.profile_editor != current.profile_editor) {
        delta.profile_editor_changed = true;
        delta.profile_editor = current.profile_editor;
    }
    if (previous.rules_editor != current.rules_editor) {
        delta.rules_editor_changed = true;
        delta.rules_editor = current.rules_editor;
    }
    if (previous.draft != current.draft) delta.draft = current.draft;
    if (previous.last_command != current.last_command) {
        delta.last_command_changed = true;
        delta.last_command = current.last_command;
    }
    if (previous.lightweight_mode != current.lightweight_mode) {
        delta.lightweight_mode = current.lightweight_mode;
    }
    if (previous.command_pending != current.command_pending) {
        delta.command_pending = current.command_pending;
    }
    return delta;
}

} // namespace ccs

#endif
