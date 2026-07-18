#pragma once

#include "app/application_status.hpp"
#include "config/field_descriptor.hpp"
#include "config/rules_text.hpp"

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
    MoveProfile,
    SetProfileEnabled,
    UpdateProfileFields,
    UpdateApplicationFields,
    ReplaceRulesText,
    FormatRulesText,
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
    ProfileKey key = 0;
    std::string id;
    bool enabled = false;
    std::optional<std::string> protocol;
    ProfileReadiness readiness = ProfileReadiness::Incomplete;
    std::string status_detail;
    std::size_t rule_count = 0;
    std::size_t enabled_rule_count = 0;

    bool operator==(const ProfileListItem&) const = default;
};

struct ConfigurationFieldEdit {
    std::string key;
    std::optional<ConfigurationFieldValue> value;

    bool operator==(const ConfigurationFieldEdit&) const = default;
};

struct ConfigurationFieldState {
    std::string key;
    ConfigurationFieldScope scope = ConfigurationFieldScope::Application;
    ConfigurationFieldInputKind input_kind = ConfigurationFieldInputKind::Text;
    bool required = true;
    std::optional<std::uint64_t> minimum;
    std::optional<std::uint64_t> maximum;
    std::vector<std::string> enum_values;
    std::string display_name_key;
    RuntimeApplyImpact apply_impact = RuntimeApplyImpact::RuntimeReload;
    std::optional<ConfigurationFieldValue> value;

    bool operator==(const ConfigurationFieldState&) const = default;
};

struct ProfileEditorState {
    ProfileKey key = 0;
    std::string profile_id;
    std::vector<ConfigurationFieldState> fields;

    bool operator==(const ProfileEditorState&) const = default;
};

struct RulesEditorState {
    ProfileKey profile_key = 0;
    std::string profile_id;
    std::string text;
    std::optional<RulesTextError> diagnostic;

    bool operator==(const RulesEditorState&) const = default;
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
    std::vector<ConfigurationFieldState> application_fields;
    std::optional<std::string> selected_profile_id;
    std::optional<ProfileKey> selected_profile_key;
    std::optional<ProfileEditorState> profile_editor;
    std::optional<RulesEditorState> rules_editor;
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
const ProfileListItem* find_profile_list_item(
    const MainWindowState& state,
    ProfileKey profile_key) noexcept;
bool select_profile(MainWindowState& state, std::string_view profile_id);
bool select_profile(MainWindowState& state, ProfileKey profile_key);
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
