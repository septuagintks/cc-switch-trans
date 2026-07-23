#include "presentation/main_window_contract.hpp"

#include <algorithm>

namespace ccs {

bool CommandResult::succeeded() const noexcept {
    return outcome == CommandOutcome::Succeeded;
}

bool CommandResult::configuration_saved() const noexcept {
    return command == MainWindowCommand::ApplyDraft
        && (outcome == CommandOutcome::Succeeded
            || outcome == CommandOutcome::SavedPendingRuntimeApply);
}

bool DraftState::loaded() const noexcept {
    return phase != DraftPhase::Unloaded && phase != DraftPhase::Loading;
}

bool DraftState::dirty() const noexcept {
    return phase == DraftPhase::Dirty
        || phase == DraftPhase::Validating
        || phase == DraftPhase::Applying;
}

bool DraftState::busy() const noexcept {
    return phase == DraftPhase::Loading
        || phase == DraftPhase::Validating
        || phase == DraftPhase::Applying;
}

const char* main_window_command_name(MainWindowCommand command) noexcept {
    switch (command) {
    case MainWindowCommand::OpenWindow:
        return "open_window";
    case MainWindowCommand::CloseWindow:
        return "close_window";
    case MainWindowCommand::QuitApplication:
        return "quit_application";
    case MainWindowCommand::Refresh:
        return "refresh";
    case MainWindowCommand::StartService:
        return "start_service";
    case MainWindowCommand::StopService:
        return "stop_service";
    case MainWindowCommand::ReloadService:
        return "reload_service";
    case MainWindowCommand::SelectProfile:
        return "select_profile";
    case MainWindowCommand::LoadDraft:
        return "load_draft";
    case MainWindowCommand::ReloadDraft:
        return "reload_draft";
    case MainWindowCommand::CreateProfile:
        return "create_profile";
    case MainWindowCommand::RenameProfile:
        return "rename_profile";
    case MainWindowCommand::RemoveProfile:
        return "remove_profile";
    case MainWindowCommand::MoveProfile:
        return "move_profile";
    case MainWindowCommand::SetProfileEnabled:
        return "set_profile_enabled";
    case MainWindowCommand::SaveProfile:
        return "save_profile";
    case MainWindowCommand::UpdateProfileFields:
        return "update_profile_fields";
    case MainWindowCommand::UpdateApplicationFields:
        return "update_application_fields";
    case MainWindowCommand::ReplaceRulesText:
        return "replace_rules_text";
    case MainWindowCommand::FormatRulesText:
        return "format_rules_text";
    case MainWindowCommand::ApplyDraft:
        return "apply_draft";
    case MainWindowCommand::DiscardDraft:
        return "discard_draft";
    case MainWindowCommand::SetLightweightMode:
        return "set_lightweight_mode";
    case MainWindowCommand::StorageStatus:
        return "storage_status";
    case MainWindowCommand::MigrateStorage:
        return "migrate_storage";
    }
    return "unknown";
}

const char* main_window_error_name(MainWindowError error) noexcept {
    switch (error) {
    case MainWindowError::None:
        return "none";
    case MainWindowError::Busy:
        return "busy";
    case MainWindowError::InvalidArgument:
        return "invalid_argument";
    case MainWindowError::ProfileNotFound:
        return "profile_not_found";
    case MainWindowError::ProfileAlreadyExists:
        return "profile_already_exists";
    case MainWindowError::ValidationFailed:
        return "validation_failed";
    case MainWindowError::RouteCollision:
        return "route_collision";
    case MainWindowError::RepositoryStale:
        return "repository_stale";
    case MainWindowError::DraftStale:
        return "draft_stale";
    case MainWindowError::PersistenceFailed:
        return "persistence_failed";
    case MainWindowError::RuntimeApplyFailed:
        return "runtime_apply_failed";
    case MainWindowError::ServiceUnavailable:
        return "service_unavailable";
    case MainWindowError::UnsavedChangesDecisionRequired:
        return "unsaved_changes_decision_required";
    case MainWindowError::MigrationRequired:
        return "migration_required";
    case MainWindowError::ReplacementConfirmationRequired:
        return "replacement_confirmation_required";
    case MainWindowError::Cancelled:
        return "cancelled";
    case MainWindowError::Internal:
        return "internal";
    }
    return "unknown";
}

const char* command_outcome_name(CommandOutcome outcome) noexcept {
    switch (outcome) {
    case CommandOutcome::Succeeded:
        return "succeeded";
    case CommandOutcome::Rejected:
        return "rejected";
    case CommandOutcome::Failed:
        return "failed";
    case CommandOutcome::Cancelled:
        return "cancelled";
    case CommandOutcome::SavedPendingRuntimeApply:
        return "saved_pending_runtime_apply";
    }
    return "unknown";
}

const char* profile_readiness_name(ProfileReadiness readiness) noexcept {
    switch (readiness) {
    case ProfileReadiness::Incomplete:
        return "incomplete";
    case ProfileReadiness::Ready:
        return "ready";
    case ProfileReadiness::Invalid:
        return "invalid";
    }
    return "unknown";
}

const char* draft_phase_name(DraftPhase phase) noexcept {
    switch (phase) {
    case DraftPhase::Unloaded:
        return "unloaded";
    case DraftPhase::Loading:
        return "loading";
    case DraftPhase::Clean:
        return "clean";
    case DraftPhase::Dirty:
        return "dirty";
    case DraftPhase::Validating:
        return "validating";
    case DraftPhase::Applying:
        return "applying";
    case DraftPhase::SavedPendingRuntimeApply:
        return "saved_pending_runtime_apply";
    }
    return "unknown";
}

const char* main_window_storage_state_name(
    MainWindowStorageState state) noexcept {
    switch (state) {
    case MainWindowStorageState::Unknown:
        return "unknown";
    case MainWindowStorageState::Uninitialized:
        return "uninitialized";
    case MainWindowStorageState::MigrationRequired:
        return "migration_required";
    case MainWindowStorageState::Ready:
        return "ready";
    case MainWindowStorageState::RecoveryRequired:
        return "recovery_required";
    case MainWindowStorageState::Invalid:
        return "invalid";
    }
    return "unknown";
}

const char* main_window_close_action_name(MainWindowCloseAction action) noexcept {
    switch (action) {
    case MainWindowCloseAction::PromptForUnsavedChanges:
        return "prompt_for_unsaved_changes";
    case MainWindowCloseAction::ApplyThenHide:
        return "apply_then_hide";
    case MainWindowCloseAction::ApplyThenDestroy:
        return "apply_then_destroy";
    case MainWindowCloseAction::DiscardThenHide:
        return "discard_then_hide";
    case MainWindowCloseAction::DiscardThenDestroy:
        return "discard_then_destroy";
    case MainWindowCloseAction::Hide:
        return "hide";
    case MainWindowCloseAction::Destroy:
        return "destroy";
    case MainWindowCloseAction::KeepOpen:
        return "keep_open";
    }
    return "unknown";
}

const char* cached_window_action_name(CachedWindowAction action) noexcept {
    switch (action) {
    case CachedWindowAction::NoChange:
        return "no_change";
    case CachedWindowAction::PromptForUnsavedChanges:
        return "prompt_for_unsaved_changes";
    case CachedWindowAction::Destroy:
        return "destroy";
    }
    return "unknown";
}

ServiceActionState service_actions_for(ApplicationState state) noexcept {
    switch (state) {
    case ApplicationState::Stopped:
    case ApplicationState::Faulted:
        return ServiceActionState{true, false, false};
    case ApplicationState::Running:
        return ServiceActionState{false, true, true};
    case ApplicationState::Starting:
    case ApplicationState::Reloading:
    case ApplicationState::Stopping:
    case ApplicationState::Shutdown:
        return {};
    }
    return {};
}

void sort_profile_list_items(std::vector<ProfileListItem>& profiles) {
    std::sort(profiles.begin(), profiles.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
}

const ProfileListItem* find_profile_list_item(
    const MainWindowState& state,
    std::string_view profile_id) noexcept {
    const auto found = std::find_if(
        state.profiles.begin(), state.profiles.end(), [profile_id](const auto& profile) {
            return profile.id == profile_id;
        });
    return found == state.profiles.end() ? nullptr : &*found;
}

const ProfileListItem* find_profile_list_item(
    const MainWindowState& state,
    ProfileKey profile_key) noexcept {
    const auto found = std::find_if(
        state.profiles.begin(), state.profiles.end(), [profile_key](const auto& profile) {
            return profile.key == profile_key;
        });
    return found == state.profiles.end() ? nullptr : &*found;
}

bool select_profile(MainWindowState& state, std::string_view profile_id) {
    const auto* profile = find_profile_list_item(state, profile_id);
    if (profile == nullptr) {
        return false;
    }
    state.selected_profile_id = profile->id;
    state.selected_profile_key = profile->key;
    return true;
}

bool select_profile(MainWindowState& state, ProfileKey profile_key) {
    const auto* profile = find_profile_list_item(state, profile_key);
    if (profile == nullptr) {
        return false;
    }
    state.selected_profile_id = profile->id;
    state.selected_profile_key = profile->key;
    return true;
}

void clear_profile_selection(MainWindowState& state) noexcept {
    state.selected_profile_id.reset();
    state.selected_profile_key.reset();
    state.profile_editor.reset();
    state.rules_editor.reset();
}

MainWindowCloseAction resolve_main_window_close(
    const DraftState& draft,
    bool lightweight_mode,
    std::optional<UnsavedChangesDecision> decision) noexcept {
    if (draft.busy()) {
        return MainWindowCloseAction::KeepOpen;
    }
    if (!draft.dirty()) {
        return lightweight_mode
            ? MainWindowCloseAction::Destroy
            : MainWindowCloseAction::Hide;
    }
    if (!decision) {
        return MainWindowCloseAction::PromptForUnsavedChanges;
    }
    switch (*decision) {
    case UnsavedChangesDecision::Apply:
        return lightweight_mode
            ? MainWindowCloseAction::ApplyThenDestroy
            : MainWindowCloseAction::ApplyThenHide;
    case UnsavedChangesDecision::Discard:
        return lightweight_mode
            ? MainWindowCloseAction::DiscardThenDestroy
            : MainWindowCloseAction::DiscardThenHide;
    case UnsavedChangesDecision::Cancel:
        return MainWindowCloseAction::KeepOpen;
    }
    return MainWindowCloseAction::KeepOpen;
}

CachedWindowAction resolve_cached_main_window(
    const DraftState& draft,
    bool window_exists,
    bool window_visible,
    bool lightweight_mode) noexcept {
    if (!window_exists || window_visible || !lightweight_mode || draft.busy()) {
        return CachedWindowAction::NoChange;
    }
    if (draft.dirty()) {
        return CachedWindowAction::PromptForUnsavedChanges;
    }
    return CachedWindowAction::Destroy;
}

} // namespace ccs
