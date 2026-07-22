#include "hosts/windows/gui_bridge/gui_command_router.hpp"

#ifdef _WIN32

#include "hosts/windows/gui_bridge/gui_snapshot_builder.hpp"

#include <utility>

namespace ccs {

namespace {

MainWindowCommand mapped_command(gui_ipc::GuiCommand command, bool& supported) {
    supported = true;
    switch (command) {
    case gui_ipc::GuiCommand::Refresh: return MainWindowCommand::Refresh;
    case gui_ipc::GuiCommand::StartService: return MainWindowCommand::StartService;
    case gui_ipc::GuiCommand::StopService: return MainWindowCommand::StopService;
    case gui_ipc::GuiCommand::ReloadService: return MainWindowCommand::ReloadService;
    case gui_ipc::GuiCommand::QuitApplication: return MainWindowCommand::QuitApplication;
    case gui_ipc::GuiCommand::ApplyDraft: return MainWindowCommand::ApplyDraft;
    case gui_ipc::GuiCommand::DiscardDraft: return MainWindowCommand::DiscardDraft;
    case gui_ipc::GuiCommand::ReloadDraft: return MainWindowCommand::ReloadDraft;
    case gui_ipc::GuiCommand::SelectProfile: return MainWindowCommand::SelectProfile;
    case gui_ipc::GuiCommand::CreateProfile: return MainWindowCommand::CreateProfile;
    case gui_ipc::GuiCommand::RemoveProfile: return MainWindowCommand::RemoveProfile;
    case gui_ipc::GuiCommand::SaveProfile: return MainWindowCommand::SaveProfile;
    case gui_ipc::GuiCommand::SetProfileEnabled:
        return MainWindowCommand::SetProfileEnabled;
    case gui_ipc::GuiCommand::MoveProfile: return MainWindowCommand::MoveProfile;
    case gui_ipc::GuiCommand::ReplaceRulesText:
        return MainWindowCommand::ReplaceRulesText;
    case gui_ipc::GuiCommand::FormatRulesText:
        return MainWindowCommand::FormatRulesText;
    case gui_ipc::GuiCommand::UpdateApplicationFields:
        return MainWindowCommand::UpdateApplicationFields;
    case gui_ipc::GuiCommand::SetLightweightMode:
        return MainWindowCommand::SetLightweightMode;
    case gui_ipc::GuiCommand::StorageStatus:
    case gui_ipc::GuiCommand::MigrateStorage:
    case gui_ipc::GuiCommand::AddRule:
    case gui_ipc::GuiCommand::RemoveRule:
    case gui_ipc::GuiCommand::SetRuleEnabled:
    case gui_ipc::GuiCommand::MoveRule:
    case gui_ipc::GuiCommand::UpdateRuleOptions:
    case gui_ipc::GuiCommand::PreviewRules:
        supported = false;
        return MainWindowCommand::Refresh;
    }
    supported = false;
    return MainWindowCommand::Refresh;
}

std::optional<UnsavedChangesDecision> mapped_decision(
    std::optional<gui_ipc::UnsavedDecision> decision) {
    if (!decision) return std::nullopt;
    switch (*decision) {
    case gui_ipc::UnsavedDecision::Apply: return UnsavedChangesDecision::Apply;
    case gui_ipc::UnsavedDecision::Discard: return UnsavedChangesDecision::Discard;
    case gui_ipc::UnsavedDecision::Cancel: return UnsavedChangesDecision::Cancel;
    }
    return std::nullopt;
}

ConfigurationFieldValue mapped_value(const gui_ipc::FieldValue& value) {
    return std::visit([](const auto& item) -> ConfigurationFieldValue {
        return item;
    }, value);
}

} // namespace

bool translate_gui_command(
    const gui_ipc::Command& command,
    MainWindowCommandRequest& request,
    gui_ipc::ErrorCode& error_code,
    std::string& error) {
    error.clear();
    error_code = gui_ipc::ErrorCode::None;
    bool supported = false;
    request = {};
    request.command = mapped_command(command.command, supported);
    if (!supported) {
        error_code = gui_ipc::ErrorCode::InvalidArgument;
        error = "the GUI command is reserved for a later feature controller";
        return false;
    }
    request.profile_id = command.profile_id;
    request.replacement_profile_id = command.replacement_profile_id;
    request.profile_key = command.profile_key;
    request.enabled = command.enabled;
    request.position = command.position;
    request.text = command.text;
    request.expected_draft_revision = command.expected_draft_revision;
    request.expected_base_revision = command.expected_base_revision;
    request.unsaved_changes_decision = mapped_decision(command.unsaved_decision);
    request.field_edits.reserve(command.field_edits.size());
    for (const auto& edit : command.field_edits) {
        ConfigurationFieldEdit converted;
        converted.key = edit.key;
        if (edit.value) converted.value = mapped_value(*edit.value);
        request.field_edits.push_back(std::move(converted));
    }
    return true;
}

GuiCommandRouter::GuiCommandRouter(MainWindowViewModel& view_model)
    : view_model_(view_model) {}

GuiCommandCompletion GuiCommandRouter::immediate_failure(
    const gui_ipc::Envelope& envelope,
    const gui_ipc::Command& command,
    gui_ipc::ErrorCode error,
    std::string detail) {
    return {
        envelope.request_id,
        envelope.sequence,
        gui_ipc::CommandStatus{
            envelope.sequence,
            gui_ipc::gui_command_name(command.command),
            gui_ipc::ResultCode::Rejected,
            error,
            command.profile_id,
            {},
            std::move(detail),
            std::nullopt,
        },
    };
}

GuiCommandSubmission GuiCommandRouter::submit(
    const gui_ipc::Envelope& envelope,
    const gui_ipc::Command& command) {
    MainWindowCommandRequest request;
    gui_ipc::ErrorCode error_code;
    std::string error;
    if (!translate_gui_command(command, request, error_code, error)) {
        return {false, immediate_failure(
            envelope, command, error_code, std::move(error))};
    }
    if (!envelope.base_revision.empty()) {
        if (request.expected_base_revision
            && *request.expected_base_revision != envelope.base_revision) {
            return {false, immediate_failure(
                envelope,
                command,
                gui_ipc::ErrorCode::MalformedMessage,
                "command base revisions disagree")};
        }
        request.expected_base_revision = envelope.base_revision;
    }

    const auto state = view_model_.snapshot();
    const auto previous_sequence = state && state->last_command
        ? state->last_command->sequence : 0;
    if (state && state->command_pending) {
        return {false, immediate_failure(
            envelope,
            command,
            gui_ipc::ErrorCode::Busy,
            "another control command is already in progress")};
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) {
            return {false, immediate_failure(
                envelope,
                command,
                gui_ipc::ErrorCode::Busy,
                "another GUI command is awaiting completion")};
        }
        pending_ = Pending{envelope.request_id, envelope.sequence, previous_sequence};
    }
    const bool submitted = view_model_.submit(std::move(request));
    if (submitted) return {true, std::nullopt};

    const auto completed = view_model_.snapshot();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
    }
    if (completed && completed->last_command
        && completed->last_command->sequence > previous_sequence) {
        return {false, GuiCommandCompletion{
            envelope.request_id,
            envelope.sequence,
            build_gui_command_status(
                *completed->last_command, envelope.sequence),
        }};
    }
    return {false, immediate_failure(
        envelope,
        command,
        gui_ipc::ErrorCode::ServiceUnavailable,
        "the GUI command service is not accepting requests")};
}

std::optional<GuiCommandCompletion> GuiCommandRouter::observe(
    const MainWindowState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_ || state.command_pending || !state.last_command
        || state.last_command->sequence <= pending_->previous_model_sequence) {
        return std::nullopt;
    }
    GuiCommandCompletion completion{
        pending_->request_id,
        pending_->client_sequence,
        build_gui_command_status(*state.last_command, pending_->client_sequence),
    };
    pending_.reset();
    return completion;
}

void GuiCommandRouter::disconnect() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.reset();
}

} // namespace ccs

#endif
