#pragma once

#include "app/application_control.hpp"
#include "app/control_executor.hpp"
#include "config/configuration_editor.hpp"
#include "config/configuration_repository.hpp"
#include "presentation/main_window_contract.hpp"
#include "presentation/ui_preferences_repository.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace ccs {

struct MainWindowCommandRequest {
    MainWindowCommandRequest() = default;
    MainWindowCommandRequest(
        MainWindowCommand command_value,
        std::string profile = {},
        std::string replacement = {},
        bool enabled_value = false,
        std::optional<UnsavedChangesDecision> unsaved_decision = std::nullopt)
        : command(command_value)
        , profile_id(std::move(profile))
        , replacement_profile_id(std::move(replacement))
        , enabled(enabled_value)
        , unsaved_changes_decision(unsaved_decision) {}

    MainWindowCommand command = MainWindowCommand::Refresh;
    std::string profile_id;
    std::string replacement_profile_id;
    bool enabled = false;
    std::optional<UnsavedChangesDecision> unsaved_changes_decision;
    std::optional<ProfileKey> profile_key;
    std::size_t position = 0;
    std::vector<ConfigurationFieldEdit> field_edits;
    std::string text;
    std::optional<std::uint64_t> expected_draft_revision;
    std::optional<std::string> expected_base_revision;
};

using MainWindowStateSnapshot = std::shared_ptr<const MainWindowState>;
using MainWindowUpdateHandler = std::function<void(MainWindowStateSnapshot)>;
using MainWindowDispatcher = std::function<void(std::function<void()>)>;

class MainWindowViewModel {
public:
    MainWindowViewModel(
        ConfigurationRepository& repository,
        ConfigurationEditor& editing,
        ApplicationControl& application,
        UiPreferencesRepository& preferences,
        MainWindowDispatcher dispatcher = {},
        ControlExecutor* shared_executor = nullptr);
    ~MainWindowViewModel();

    MainWindowViewModel(const MainWindowViewModel&) = delete;
    MainWindowViewModel& operator=(const MainWindowViewModel&) = delete;

    void set_update_handler(MainWindowUpdateHandler handler);
    MainWindowStateSnapshot snapshot() const;
    void refresh_application_status();
    bool submit(MainWindowCommandRequest request);
    void stop();

private:
    struct CallbackState {
        std::mutex mutex;
        MainWindowUpdateHandler handler;
        std::uint64_t generation = 0;
        bool active = true;
    };

    struct ExecutionResult {
        CommandOutcome outcome = CommandOutcome::Succeeded;
        MainWindowError error = MainWindowError::None;
        std::string profile_id;
        std::string field;
        std::string detail;
        std::optional<MainWindowCommand> recovery_command;
    };

    void execute(std::uint64_t sequence, MainWindowCommandRequest request);
    ExecutionResult execute_command(const MainWindowCommandRequest& request);
    std::optional<ExecutionResult> check_draft_precondition(
        const MainWindowCommandRequest& request) const;
    ExecutionResult load_draft(
        bool force_reload,
        std::optional<UnsavedChangesDecision> unsaved_decision);
    ExecutionResult mutate_draft(const MainWindowCommandRequest& request);
    ExecutionResult update_fields(const MainWindowCommandRequest& request);
    ExecutionResult update_rules(const MainWindowCommandRequest& request, bool format);
    ExecutionResult apply_draft();
    ExecutionResult discard_draft();
    ExecutionResult set_lightweight_mode(bool enabled);
    ExecutionResult execute_service_command(MainWindowCommand command);

    void rebuild_editor_state(
        std::optional<ProfileKey> preferred_key = std::nullopt,
        std::optional<std::string> preferred_id = std::nullopt,
        bool preserve_rules_text = false);
    void publish_state_change(const std::function<void(MainWindowState&)>& update);
    MainWindowStateSnapshot snapshot_locked() const;
    void notify(const MainWindowStateSnapshot& state) const;

    static MainWindowError classify_editor_error(ConfigurationEditError error);
    static MainWindowError classify_repository_error(ConfigRepositoryFailure failure);

    ConfigurationRepository& repository_;
    ConfigurationEditor& editing_;
    ApplicationControl& application_;
    UiPreferencesRepository& preferences_;
    UiPreferences preference_values_;
    mutable std::mutex mutex_;
    MainWindowState state_;
    MainWindowDispatcher dispatcher_;
    std::shared_ptr<CallbackState> callback_state_;
    std::uint64_t next_sequence_ = 1;
    bool accepting_ = true;
    std::unique_ptr<ControlExecutor> owned_executor_;
    ControlExecutor& executor_;
};

} // namespace ccs
