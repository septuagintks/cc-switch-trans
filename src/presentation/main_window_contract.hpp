#pragma once

#include "app/application_status.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ccs {

enum class MainWindowCommand {
    OpenWindow,
    CloseWindow,
    QuitApplication,
    Refresh,
    StartService,
    StopService,
    ReloadService,
    SelectProfile,
    LoadDraft,
    ReloadDraft,
    CreateProfile,
    RenameProfile,
    RemoveProfile,
    SetProfileEnabled,
    ApplyDraft,
    DiscardDraft,
    SetLightweightMode,
};

enum class MainWindowError {
    None,
    Busy,
    InvalidArgument,
    ProfileNotFound,
    ProfileAlreadyExists,
    ValidationFailed,
    RouteCollision,
    RepositoryStale,
    PersistenceFailed,
    RuntimeApplyFailed,
    ServiceUnavailable,
    UnsavedChangesDecisionRequired,
    Cancelled,
    Internal,
};

enum class CommandOutcome {
    Succeeded,
    Rejected,
    Failed,
    Cancelled,
    SavedPendingRuntimeApply,
};

struct CommandResult {
    std::uint64_t sequence = 0;
    MainWindowCommand command = MainWindowCommand::Refresh;
    CommandOutcome outcome = CommandOutcome::Succeeded;
    MainWindowError error = MainWindowError::None;
    std::string profile_id;
    std::string field;
    std::string detail;
    std::optional<MainWindowCommand> recovery_command;

    [[nodiscard]] bool succeeded() const noexcept;
    [[nodiscard]] bool configuration_saved() const noexcept;
};

enum class ProfileReadiness {
    Incomplete,
    Ready,
    Invalid,
};

struct ProfileListItem {
    std::string id;
    bool enabled = false;
    std::optional<std::string> protocol;
    ProfileReadiness readiness = ProfileReadiness::Incomplete;
    std::string status_detail;

    bool operator==(const ProfileListItem&) const = default;
};

enum class DraftPhase {
    Unloaded,
    Loading,
    Clean,
    Dirty,
    Validating,
    Applying,
    SavedPendingRuntimeApply,
};

struct DraftState {
    DraftPhase phase = DraftPhase::Unloaded;
    bool runtime_apply_pending = false;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
};

struct ServiceActionState {
    bool can_start = false;
    bool can_stop = false;
    bool can_reload = false;

    bool operator==(const ServiceActionState&) const = default;
};

struct MainWindowState {
    std::uint64_t revision = 0;
    ApplicationStatus application;
    std::vector<ProfileListItem> profiles;
    std::optional<std::string> selected_profile_id;
    DraftState draft;
    std::optional<CommandResult> last_command;
    bool lightweight_mode = true;
    bool command_pending = false;
};

enum class UnsavedChangesDecision {
    Apply,
    Discard,
    Cancel,
};

enum class MainWindowCloseAction {
    PromptForUnsavedChanges,
    ApplyThenHide,
    ApplyThenDestroy,
    DiscardThenHide,
    DiscardThenDestroy,
    Hide,
    Destroy,
    KeepOpen,
};

enum class CachedWindowAction {
    NoChange,
    PromptForUnsavedChanges,
    Destroy,
};

const char* main_window_command_name(MainWindowCommand command) noexcept;
const char* main_window_error_name(MainWindowError error) noexcept;
const char* command_outcome_name(CommandOutcome outcome) noexcept;
const char* profile_readiness_name(ProfileReadiness readiness) noexcept;
const char* draft_phase_name(DraftPhase phase) noexcept;
const char* main_window_close_action_name(MainWindowCloseAction action) noexcept;
const char* cached_window_action_name(CachedWindowAction action) noexcept;

ServiceActionState service_actions_for(ApplicationState state) noexcept;
void sort_profile_list_items(std::vector<ProfileListItem>& profiles);
const ProfileListItem* find_profile_list_item(
    const MainWindowState& state,
    std::string_view profile_id) noexcept;
bool select_profile(MainWindowState& state, std::string_view profile_id);
void clear_profile_selection(MainWindowState& state) noexcept;

MainWindowCloseAction resolve_main_window_close(
    const DraftState& draft,
    bool lightweight_mode,
    std::optional<UnsavedChangesDecision> decision = std::nullopt) noexcept;
CachedWindowAction resolve_cached_main_window(
    const DraftState& draft,
    bool window_exists,
    bool window_visible,
    bool lightweight_mode) noexcept;

} // namespace ccs
