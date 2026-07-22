#include "gui_ipc/protocol_types.hpp"

#include <array>
#include <utility>

namespace ccs::gui_ipc {

namespace {

template <typename Enum, std::size_t Size>
const char* enum_name(
    Enum value,
    const std::array<std::pair<Enum, const char*>, Size>& values) noexcept {
    for (const auto& [candidate, name] : values) {
        if (candidate == value) {
            return name;
        }
    }
    return "unknown";
}

template <typename Enum, std::size_t Size>
bool parse_enum(
    std::string_view value,
    const std::array<std::pair<Enum, const char*>, Size>& values,
    Enum& parsed) noexcept {
    for (const auto& [candidate, name] : values) {
        if (value == name) {
            parsed = candidate;
            return true;
        }
    }
    return false;
}

constexpr std::array kMessageKinds = {
    std::pair{MessageKind::Hello, "hello"},
    std::pair{MessageKind::HelloResult, "hello_result"},
    std::pair{MessageKind::Activate, "activate"},
    std::pair{MessageKind::Shutdown, "shutdown"},
    std::pair{MessageKind::Ping, "ping"},
    std::pair{MessageKind::Pong, "pong"},
    std::pair{MessageKind::SnapshotRequest, "snapshot_request"},
    std::pair{MessageKind::Snapshot, "snapshot"},
    std::pair{MessageKind::StateChanged, "state_changed"},
    std::pair{MessageKind::Command, "command"},
    std::pair{MessageKind::CommandStatus, "command_status"},
    std::pair{MessageKind::MaintenanceRequest, "maintenance_request"},
    std::pair{MessageKind::MaintenanceResult, "maintenance_result"},
};

constexpr std::array kResultCodes = {
    std::pair{ResultCode::Accepted, "accepted"},
    std::pair{ResultCode::Succeeded, "succeeded"},
    std::pair{ResultCode::Rejected, "rejected"},
    std::pair{ResultCode::Failed, "failed"},
    std::pair{ResultCode::Cancelled, "cancelled"},
    std::pair{ResultCode::SavedPendingRuntimeApply, "saved_pending_runtime_apply"},
};

constexpr std::array kErrorCodes = {
    std::pair{ErrorCode::None, "none"},
    std::pair{ErrorCode::MalformedFrame, "malformed_frame"},
    std::pair{ErrorCode::MalformedMessage, "malformed_message"},
    std::pair{ErrorCode::UnsupportedProtocol, "unsupported_protocol"},
    std::pair{ErrorCode::VersionMismatch, "version_mismatch"},
    std::pair{ErrorCode::SourceMismatch, "source_mismatch"},
    std::pair{ErrorCode::AuthenticationFailed, "authentication_failed"},
    std::pair{ErrorCode::SessionMismatch, "session_mismatch"},
    std::pair{ErrorCode::SequenceRejected, "sequence_rejected"},
    std::pair{ErrorCode::RevisionStale, "revision_stale"},
    std::pair{ErrorCode::Busy, "busy"},
    std::pair{ErrorCode::InvalidArgument, "invalid_argument"},
    std::pair{ErrorCode::ProfileNotFound, "profile_not_found"},
    std::pair{ErrorCode::ProfileAlreadyExists, "profile_already_exists"},
    std::pair{ErrorCode::ValidationFailed, "validation_failed"},
    std::pair{ErrorCode::RouteCollision, "route_collision"},
    std::pair{ErrorCode::RepositoryStale, "repository_stale"},
    std::pair{ErrorCode::DraftStale, "draft_stale"},
    std::pair{ErrorCode::PersistenceFailed, "persistence_failed"},
    std::pair{ErrorCode::RuntimeApplyFailed, "runtime_apply_failed"},
    std::pair{ErrorCode::ServiceUnavailable, "service_unavailable"},
    std::pair{ErrorCode::UnsavedChangesDecisionRequired,
        "unsaved_changes_decision_required"},
    std::pair{ErrorCode::MigrationRequired, "migration_required"},
    std::pair{ErrorCode::ReplacementConfirmationRequired,
        "replacement_confirmation_required"},
    std::pair{ErrorCode::IpcBackpressure, "ipc_backpressure"},
    std::pair{ErrorCode::Disconnected, "disconnected"},
    std::pair{ErrorCode::Internal, "internal"},
};

constexpr std::array kGuiCommands = {
    std::pair{GuiCommand::Refresh, "refresh"},
    std::pair{GuiCommand::StartService, "start"},
    std::pair{GuiCommand::StopService, "stop"},
    std::pair{GuiCommand::ReloadService, "reload"},
    std::pair{GuiCommand::QuitApplication, "quit_application"},
    std::pair{GuiCommand::ApplyDraft, "apply"},
    std::pair{GuiCommand::DiscardDraft, "discard"},
    std::pair{GuiCommand::ReloadDraft, "reload_draft"},
    std::pair{GuiCommand::SelectProfile, "select"},
    std::pair{GuiCommand::CreateProfile, "create"},
    std::pair{GuiCommand::RemoveProfile, "remove"},
    std::pair{GuiCommand::SaveProfile, "save"},
    std::pair{GuiCommand::SetProfileEnabled, "set_enabled"},
    std::pair{GuiCommand::MoveProfile, "move"},
    std::pair{GuiCommand::ReplaceRulesText, "replace_text"},
    std::pair{GuiCommand::FormatRulesText, "format_text"},
    std::pair{GuiCommand::UpdateApplicationFields, "update_application_fields"},
    std::pair{GuiCommand::SetLightweightMode, "set_lightweight_mode"},
    std::pair{GuiCommand::StorageStatus, "storage_status"},
    std::pair{GuiCommand::MigrateStorage, "migrate"},
    std::pair{GuiCommand::AddRule, "add_rule"},
    std::pair{GuiCommand::RemoveRule, "remove_rule"},
    std::pair{GuiCommand::SetRuleEnabled, "set_rule_enabled"},
    std::pair{GuiCommand::MoveRule, "move_rule"},
    std::pair{GuiCommand::UpdateRuleOptions, "update_rule_options"},
    std::pair{GuiCommand::PreviewRules, "preview_rules"},
};

constexpr std::array kUnsavedDecisions = {
    std::pair{UnsavedDecision::Apply, "apply"},
    std::pair{UnsavedDecision::Discard, "discard"},
    std::pair{UnsavedDecision::Cancel, "cancel"},
};

constexpr std::array kMaintenanceCommands = {
    std::pair{MaintenanceCommand::QueryVersion, "query_version"},
    std::pair{MaintenanceCommand::RequestShutdown, "request_shutdown"},
    std::pair{MaintenanceCommand::WaitForRelease, "wait_for_release"},
};

} // namespace

bool StateDelta::empty() const noexcept {
    return !application && !profiles && !application_fields && !selection
        && !profile_editor_changed && !rules_editor_changed && !draft
        && !last_command_changed && !lightweight_mode && !command_pending;
}

const char* message_kind_name(MessageKind value) noexcept {
    return enum_name(value, kMessageKinds);
}

const char* result_code_name(ResultCode value) noexcept {
    return enum_name(value, kResultCodes);
}

const char* error_code_name(ErrorCode value) noexcept {
    return enum_name(value, kErrorCodes);
}

const char* gui_command_name(GuiCommand value) noexcept {
    return enum_name(value, kGuiCommands);
}

const char* unsaved_decision_name(UnsavedDecision value) noexcept {
    return enum_name(value, kUnsavedDecisions);
}

const char* maintenance_command_name(MaintenanceCommand value) noexcept {
    return enum_name(value, kMaintenanceCommands);
}

bool parse_message_kind(std::string_view value, MessageKind& parsed) noexcept {
    return parse_enum(value, kMessageKinds, parsed);
}

bool parse_result_code(std::string_view value, ResultCode& parsed) noexcept {
    return parse_enum(value, kResultCodes, parsed);
}

bool parse_error_code(std::string_view value, ErrorCode& parsed) noexcept {
    return parse_enum(value, kErrorCodes, parsed);
}

bool parse_gui_command(std::string_view value, GuiCommand& parsed) noexcept {
    return parse_enum(value, kGuiCommands, parsed);
}

bool parse_unsaved_decision(std::string_view value, UnsavedDecision& parsed) noexcept {
    return parse_enum(value, kUnsavedDecisions, parsed);
}

bool parse_maintenance_command(
    std::string_view value,
    MaintenanceCommand& parsed) noexcept {
    return parse_enum(value, kMaintenanceCommands, parsed);
}

bool validate_envelope(const Envelope& envelope, std::string& error) {
    error.clear();
    const bool maintenance = envelope.kind == MessageKind::MaintenanceRequest
        || envelope.kind == MessageKind::MaintenanceResult;
    const auto expected_protocol = maintenance ? kMaintenanceProtocol : kProtocol;
    if (envelope.protocol != expected_protocol) {
        error = "unsupported IPC protocol";
        return false;
    }
    if (envelope.request_id.empty()) {
        error = "IPC request_id must not be empty";
        return false;
    }
    if (envelope.payload_json.empty()) {
        error = "IPC payload must not be empty";
        return false;
    }
    if (envelope.kind == MessageKind::Hello) {
        if (!envelope.session_id.empty() || envelope.sequence != 0) {
            error = "hello must not claim a session or sequence";
            return false;
        }
        return true;
    }
    if (envelope.session_id.empty()) {
        error = "IPC session_id must not be empty after hello";
        return false;
    }
    if (envelope.kind != MessageKind::HelloResult && envelope.sequence == 0) {
        error = "IPC sequence must be positive after hello";
        return false;
    }
    if ((envelope.kind == MessageKind::HelloResult
            || envelope.kind == MessageKind::CommandStatus
            || envelope.kind == MessageKind::MaintenanceResult)
        && !envelope.result) {
        error = "IPC result message is missing result";
        return false;
    }
    return true;
}

} // namespace ccs::gui_ipc
