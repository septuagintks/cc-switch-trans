#pragma once

#include "app/application_control.hpp"
#include "app/control_executor.hpp"
#include "config/config_editing_service.hpp"
#include "presentation/main_window_contract.hpp"
#include "presentation/ui_preferences_repository.hpp"

#include <functional>
#include <memory>
#include <mutex>
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
};

using MainWindowStateSnapshot = std::shared_ptr<const MainWindowState>;
using MainWindowUpdateHandler = std::function<void(MainWindowStateSnapshot)>;
using MainWindowDispatcher = std::function<void(std::function<void()>)>;

class MainWindowViewModel {
public:
    MainWindowViewModel(
        ConfigRepository& repository,
        ConfigEditingService& editing,
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
    ExecutionResult load_draft(
        bool force_reload,
        std::optional<UnsavedChangesDecision> unsaved_decision);
    ExecutionResult mutate_draft(const MainWindowCommandRequest& request);
    ExecutionResult apply_draft();
    ExecutionResult discard_draft();
    ExecutionResult set_lightweight_mode(bool enabled);
    ExecutionResult execute_service_command(MainWindowCommand command);

    void rebuild_profile_list(const std::optional<std::string>& preferred_selection = std::nullopt);
    void publish_state_change(const std::function<void(MainWindowState&)>& update);
    MainWindowStateSnapshot snapshot_locked() const;
    void notify(const MainWindowStateSnapshot& state) const;

    static MainWindowError classify_validation_error(const std::string& error);
    static MainWindowError classify_repository_error(ConfigRepositoryFailure failure);

    ConfigRepository& repository_;
    ConfigEditingService& editing_;
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
