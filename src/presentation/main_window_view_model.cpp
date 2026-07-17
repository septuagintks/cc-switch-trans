#include "presentation/main_window_view_model.hpp"

#include "protocols/protocol_registry.hpp"
#include "rules/rule_registry.hpp"

#include <algorithm>
#include <future>
#include <utility>

namespace ccs {

namespace {

std::string with_profile_context(
    const std::string& profile_id,
    std::string detail) {
    const auto prefix = "profile " + profile_id;
    if (detail == prefix
        || detail.starts_with(prefix + " ")
        || detail.starts_with(prefix + ":")) {
        return detail;
    }
    return prefix + ": " + detail;
}

ProfileListItem make_profile_list_item(
    const std::string& profile_id,
    const ProfileDefinition& profile,
    const std::shared_ptr<const ProtocolRegistry>& protocols,
    const std::shared_ptr<const RuleRegistry>& rules) {
    ProfileListItem item;
    item.id = profile_id;
    item.enabled = profile.enabled;
    item.rule_count = profile.rules.size();
    item.enabled_rule_count = static_cast<std::size_t>(std::count_if(
        profile.rules.begin(), profile.rules.end(), [](const RuleDefinition& rule) {
            return rule.enabled;
        }));
    if (profile.protocol) {
        item.protocol = profile.protocol->value;
    }

    std::string error;
    if (!validate_profile_definition(profile_id, profile, false, error)) {
        item.readiness = ProfileReadiness::Invalid;
        item.status_detail = with_profile_context(profile_id, std::move(error));
        return item;
    }
    if (!validate_profile_definition(profile_id, profile, true, error)) {
        item.readiness = ProfileReadiness::Incomplete;
        item.status_detail = with_profile_context(profile_id, std::move(error));
        return item;
    }

    const auto handler = protocols->find(profile.protocol->value);
    if (!handler) {
        item.readiness = ProfileReadiness::Invalid;
        item.status_detail = with_profile_context(
            profile_id, "unknown protocol: " + profile.protocol->value);
        return item;
    }
    if (!protocols->validate_profile(handler, profile_id, profile, error)) {
        item.readiness = ProfileReadiness::Invalid;
        item.status_detail = with_profile_context(profile_id, std::move(error));
        return item;
    }
    std::shared_ptr<const CompiledPipeline> pipeline;
    if (!rules->compile_pipeline(profile.rules, handler, pipeline, error)) {
        item.readiness = ProfileReadiness::Invalid;
        item.status_detail = with_profile_context(profile_id, std::move(error));
        return item;
    }
    item.readiness = ProfileReadiness::Ready;
    return item;
}

} // namespace

MainWindowViewModel::MainWindowViewModel(
    ConfigRepository& repository,
    ConfigEditingService& editing,
    ApplicationControl& application,
    UiPreferencesRepository& preferences,
    MainWindowDispatcher dispatcher,
    ControlExecutor* shared_executor)
    : repository_(repository)
    , editing_(editing)
    , application_(application)
    , preferences_(preferences)
    , preference_values_(make_default_ui_preferences())
    , dispatcher_(std::move(dispatcher))
    , callback_state_(std::make_shared<CallbackState>())
    , owned_executor_(shared_executor == nullptr
              ? std::make_unique<ControlExecutor>()
              : nullptr)
    , executor_(shared_executor != nullptr ? *shared_executor : *owned_executor_) {
    state_.application = application_.status();
    state_.lightweight_mode = preference_values_.lightweight_mode;
}

MainWindowViewModel::~MainWindowViewModel() {
    stop();
}

void MainWindowViewModel::set_update_handler(MainWindowUpdateHandler handler) {
    MainWindowStateSnapshot current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = snapshot_locked();
    }
    {
        std::lock_guard<std::mutex> lock(callback_state_->mutex);
        if (!callback_state_->active) {
            return;
        }
        callback_state_->handler = std::move(handler);
        ++callback_state_->generation;
    }
    notify(current);
}

MainWindowStateSnapshot MainWindowViewModel::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_locked();
}

void MainWindowViewModel::refresh_application_status() {
    const auto status = application_.status();
    publish_state_change([&](MainWindowState& state) {
        state.application = status;
    });
}

bool MainWindowViewModel::submit(MainWindowCommandRequest request) {
    MainWindowStateSnapshot pending_snapshot;
    std::uint64_t sequence = 0;
    const auto command = request.command;
    const auto profile_id = request.profile_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) {
            return false;
        }
        sequence = next_sequence_++;
        if (state_.command_pending) {
            CommandResult result;
            result.sequence = sequence;
            result.command = request.command;
            result.outcome = CommandOutcome::Rejected;
            result.error = MainWindowError::Busy;
            result.profile_id = request.profile_id;
            result.detail = "another control command is already in progress";
            state_.last_command = std::move(result);
            ++state_.revision;
            pending_snapshot = snapshot_locked();
        } else {
            state_.command_pending = true;
            ++state_.revision;
            pending_snapshot = snapshot_locked();
        }
    }
    notify(pending_snapshot);

    if (pending_snapshot->last_command
        && pending_snapshot->last_command->sequence == sequence
        && pending_snapshot->last_command->error == MainWindowError::Busy) {
        return false;
    }
    if (executor_.post([this, sequence, request = std::move(request)]() mutable {
            execute(sequence, std::move(request));
        })) {
        return true;
    }

    publish_state_change([&](MainWindowState& state) {
        state.command_pending = false;
        CommandResult result;
        result.sequence = sequence;
        result.command = command;
        result.outcome = CommandOutcome::Failed;
        result.error = MainWindowError::Internal;
        result.profile_id = profile_id;
        result.detail = "control executor rejected the command";
        state.last_command = std::move(result);
    });
    return false;
}

void MainWindowViewModel::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) {
            return;
        }
        accepting_ = false;
    }
    {
        std::lock_guard<std::mutex> callback_lock(callback_state_->mutex);
        callback_state_->active = false;
        callback_state_->handler = {};
        ++callback_state_->generation;
    }
    if (owned_executor_) {
        owned_executor_->stop();
        return;
    }
    auto drained = std::make_shared<std::promise<void>>();
    auto ready = drained->get_future();
    if (executor_.post([drained]() { drained->set_value(); })) {
        ready.wait();
    }
}

void MainWindowViewModel::execute(
    std::uint64_t sequence,
    MainWindowCommandRequest request) {
    ExecutionResult executed;
    try {
        executed = execute_command(request);
    } catch (const std::exception& ex) {
        executed.outcome = CommandOutcome::Failed;
        executed.error = MainWindowError::Internal;
        executed.detail = ex.what();
    } catch (...) {
        executed.outcome = CommandOutcome::Failed;
        executed.error = MainWindowError::Internal;
        executed.detail = "unknown control command failure";
    }

    publish_state_change([&](MainWindowState& state) {
        state.command_pending = false;
        state.application = application_.status();
        CommandResult result;
        result.sequence = sequence;
        result.command = request.command;
        result.outcome = executed.outcome;
        result.error = executed.error;
        result.profile_id = executed.profile_id.empty()
            ? request.profile_id
            : executed.profile_id;
        result.field = std::move(executed.field);
        result.detail = std::move(executed.detail);
        result.recovery_command = executed.recovery_command;
        state.last_command = std::move(result);
    });
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::execute_command(
    const MainWindowCommandRequest& request) {
    switch (request.command) {
    case MainWindowCommand::LoadDraft:
        return load_draft(false, request.unsaved_changes_decision);
    case MainWindowCommand::ReloadDraft:
        return load_draft(true, request.unsaved_changes_decision);
    case MainWindowCommand::CreateProfile:
    case MainWindowCommand::RenameProfile:
    case MainWindowCommand::RemoveProfile:
    case MainWindowCommand::SetProfileEnabled:
        return mutate_draft(request);
    case MainWindowCommand::ApplyDraft:
        return apply_draft();
    case MainWindowCommand::DiscardDraft:
        return discard_draft();
    case MainWindowCommand::SetLightweightMode:
        return set_lightweight_mode(request.enabled);
    case MainWindowCommand::StartService:
    case MainWindowCommand::StopService:
    case MainWindowCommand::ReloadService:
    case MainWindowCommand::QuitApplication:
        return execute_service_command(request.command);
    case MainWindowCommand::Refresh:
        rebuild_profile_list();
        return {};
    case MainWindowCommand::SelectProfile:
        if (find_profile_list_item(*snapshot(), request.profile_id) == nullptr) {
            return ExecutionResult{
                CommandOutcome::Rejected,
                MainWindowError::ProfileNotFound,
                request.profile_id,
                {},
                "profile does not exist: " + request.profile_id,
                std::nullopt,
            };
        }
        publish_state_change([&](MainWindowState& state) {
            (void)select_profile(state, request.profile_id);
        });
        return {};
    case MainWindowCommand::OpenWindow:
    case MainWindowCommand::CloseWindow:
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::InvalidArgument,
            {},
            {},
            "window lifecycle commands belong to the platform host",
            std::nullopt,
        };
    }
    return ExecutionResult{
        CommandOutcome::Failed,
        MainWindowError::Internal,
        {},
        {},
        "unknown main window command",
        std::nullopt,
    };
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::load_draft(
    bool force_reload,
    std::optional<UnsavedChangesDecision> unsaved_decision) {
    if (editing_.active()) {
        if (!force_reload) {
            return ExecutionResult{
                CommandOutcome::Rejected,
                MainWindowError::Busy,
                {},
                {},
                "a draft is already loaded",
                std::nullopt,
            };
        }
        const auto current = snapshot();
        if (current->draft.dirty()) {
            if (!unsaved_decision) {
                return ExecutionResult{
                    CommandOutcome::Rejected,
                    MainWindowError::UnsavedChangesDecisionRequired,
                    {},
                    {},
                    "dirty draft requires an explicit Apply, Discard, or Cancel decision",
                    std::nullopt,
                };
            }
            if (*unsaved_decision == UnsavedChangesDecision::Cancel) {
                return ExecutionResult{
                    CommandOutcome::Cancelled,
                    MainWindowError::Cancelled,
                    {},
                    {},
                    "draft reload was cancelled",
                    std::nullopt,
                };
            }
            if (*unsaved_decision == UnsavedChangesDecision::Apply) {
                return ExecutionResult{
                    CommandOutcome::Rejected,
                    MainWindowError::InvalidArgument,
                    {},
                    {},
                    "apply the draft before reloading it",
                    MainWindowCommand::ApplyDraft,
                };
            }
        }
        editing_.discard();
    }
    publish_state_change([](MainWindowState& state) {
        state.draft.phase = DraftPhase::Loading;
    });

    std::string preference_error;
    UiPreferences loaded_preferences = make_default_ui_preferences();
    const bool preferences_loaded = preferences_.load(loaded_preferences, preference_error);
    if (preferences_loaded) {
        preference_values_ = loaded_preferences;
    }

    std::string error;
    if ((force_reload || !repository_.loaded()) && !repository_.load(error)) {
        publish_state_change([&](MainWindowState& state) {
            state.draft.phase = DraftPhase::Unloaded;
            state.profiles.clear();
            state.selected_profile_id.reset();
        });
        return ExecutionResult{
            CommandOutcome::Failed,
            classify_repository_error(repository_.last_failure()),
            {},
            {},
            std::move(error),
            MainWindowCommand::ReloadDraft,
        };
    }
    if (!editing_.begin(error)) {
        publish_state_change([](MainWindowState& state) {
            state.draft.phase = DraftPhase::Unloaded;
            state.profiles.clear();
            state.selected_profile_id.reset();
        });
        return ExecutionResult{
            CommandOutcome::Failed,
            MainWindowError::Internal,
            {},
            {},
            std::move(error),
            MainWindowCommand::ReloadDraft,
        };
    }

    publish_state_change([&](MainWindowState& state) {
        state.lightweight_mode = preference_values_.lightweight_mode;
        state.draft.phase = state.draft.runtime_apply_pending
            ? DraftPhase::SavedPendingRuntimeApply
            : DraftPhase::Clean;
    });
    rebuild_profile_list();
    if (!preferences_loaded) {
        return ExecutionResult{
            CommandOutcome::Failed,
            MainWindowError::PersistenceFailed,
            {},
            "main_window.lightweight_mode",
            std::move(preference_error),
            MainWindowCommand::ReloadDraft,
        };
    }
    return {};
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::mutate_draft(
    const MainWindowCommandRequest& request) {
    if (!editing_.active()) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::ServiceUnavailable,
            request.profile_id,
            {},
            "no draft is loaded",
            MainWindowCommand::LoadDraft,
        };
    }

    auto candidate = editing_.draft();
    auto profile = candidate.profiles.find(request.profile_id);
    std::optional<std::string> preferred_selection;
    if (request.command == MainWindowCommand::CreateProfile) {
        if (!is_valid_profile_id(request.profile_id)) {
            return ExecutionResult{
                CommandOutcome::Rejected,
                MainWindowError::InvalidArgument,
                request.profile_id,
                "profile_id",
                "invalid profile id: " + request.profile_id,
                std::nullopt,
            };
        }
        if (profile != candidate.profiles.end()) {
            return ExecutionResult{
                CommandOutcome::Rejected,
                MainWindowError::ProfileAlreadyExists,
                request.profile_id,
                "profile_id",
                "profile already exists: " + request.profile_id,
                std::nullopt,
            };
        }
        candidate.profiles.emplace(request.profile_id, ProfileDefinition{});
        preferred_selection = request.profile_id;
    } else {
        if (profile == candidate.profiles.end()) {
            return ExecutionResult{
                CommandOutcome::Rejected,
                MainWindowError::ProfileNotFound,
                request.profile_id,
                "profile_id",
                "profile does not exist: " + request.profile_id,
                std::nullopt,
            };
        }
        if (request.command == MainWindowCommand::RenameProfile) {
            if (request.replacement_profile_id == request.profile_id) {
                return ExecutionResult{
                    CommandOutcome::Succeeded,
                    MainWindowError::None,
                    request.profile_id,
                    {},
                    {},
                    std::nullopt,
                };
            }
            if (!is_valid_profile_id(request.replacement_profile_id)) {
                return ExecutionResult{
                    CommandOutcome::Rejected,
                    MainWindowError::InvalidArgument,
                    request.profile_id,
                    "replacement_profile_id",
                    "invalid profile id: " + request.replacement_profile_id,
                    std::nullopt,
                };
            }
            if (candidate.profiles.contains(request.replacement_profile_id)) {
                return ExecutionResult{
                    CommandOutcome::Rejected,
                    MainWindowError::ProfileAlreadyExists,
                    request.replacement_profile_id,
                    "replacement_profile_id",
                    "profile already exists: " + request.replacement_profile_id,
                    std::nullopt,
                };
            }
            auto node = candidate.profiles.extract(profile);
            node.key() = request.replacement_profile_id;
            candidate.profiles.insert(std::move(node));
            preferred_selection = request.replacement_profile_id;
        } else if (request.command == MainWindowCommand::RemoveProfile) {
            candidate.profiles.erase(profile);
        } else {
            if (profile->second.enabled == request.enabled) {
                return ExecutionResult{
                    CommandOutcome::Succeeded,
                    MainWindowError::None,
                    request.profile_id,
                    {},
                    {},
                    std::nullopt,
                };
            }
            profile->second.enabled = request.enabled;
            preferred_selection = request.profile_id;
        }
    }

    std::string error;
    if (!validate_config_candidate(candidate, repository_.paths().root, error)) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            classify_validation_error(error),
            request.profile_id,
            {},
            std::move(error),
            std::nullopt,
        };
    }
    editing_.draft() = std::move(candidate);
    publish_state_change([](MainWindowState& state) {
        state.draft.phase = DraftPhase::Dirty;
    });
    rebuild_profile_list(preferred_selection);
    return ExecutionResult{
        CommandOutcome::Succeeded,
        MainWindowError::None,
        preferred_selection.value_or(request.profile_id),
        {},
        {},
        std::nullopt,
    };
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::apply_draft() {
    if (!editing_.active()) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::ServiceUnavailable,
            {},
            {},
            "no draft is loaded",
            MainWindowCommand::LoadDraft,
        };
    }
    const auto draft_state = snapshot()->draft;
    if (!draft_state.dirty()) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::InvalidArgument,
            {},
            {},
            "the current draft has no unsaved changes",
            std::nullopt,
        };
    }
    const auto application_status = application_.status();
    if (application_status.state == ApplicationState::Starting
        || application_status.state == ApplicationState::Reloading
        || application_status.state == ApplicationState::Stopping
        || application_status.state == ApplicationState::Shutdown) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::Busy,
            {},
            {},
            "configuration cannot be applied while the service is "
                + std::string(application_state_name(application_status.state)),
            std::nullopt,
        };
    }

    publish_state_change([](MainWindowState& state) {
        state.draft.phase = DraftPhase::Validating;
    });
    std::string error;
    if (!editing_.validate(error)) {
        publish_state_change([](MainWindowState& state) {
            state.draft.phase = DraftPhase::Dirty;
        });
        return ExecutionResult{
            CommandOutcome::Rejected,
            classify_validation_error(error),
            {},
            {},
            std::move(error),
            std::nullopt,
        };
    }
    publish_state_change([](MainWindowState& state) {
        state.draft.phase = DraftPhase::Applying;
    });
    if (!editing_.commit(error)) {
        publish_state_change([](MainWindowState& state) {
            state.draft.phase = DraftPhase::Dirty;
        });
        const auto repository_error = classify_repository_error(repository_.last_failure());
        return ExecutionResult{
            CommandOutcome::Failed,
            repository_error,
            {},
            {},
            std::move(error),
            repository_error == MainWindowError::RepositoryStale
                ? std::optional<MainWindowCommand>{MainWindowCommand::ReloadDraft}
                : std::nullopt,
        };
    }

    const bool runtime_was_running = application_status.state == ApplicationState::Running;
    std::string runtime_error;
    const bool runtime_applied = !runtime_was_running || application_.reload(runtime_error);
    if (!editing_.begin(error)) {
        publish_state_change([&](MainWindowState& state) {
            state.draft.phase = runtime_applied
                ? DraftPhase::Unloaded
                : DraftPhase::SavedPendingRuntimeApply;
            state.draft.runtime_apply_pending = !runtime_applied;
            state.profiles.clear();
            state.selected_profile_id.reset();
        });
        return ExecutionResult{
            CommandOutcome::Failed,
            MainWindowError::Internal,
            {},
            {},
            "configuration saved but the editor could not reload it: " + error,
            MainWindowCommand::ReloadDraft,
        };
    }
    publish_state_change([&](MainWindowState& state) {
        state.draft.phase = runtime_applied
            ? DraftPhase::Clean
            : DraftPhase::SavedPendingRuntimeApply;
        state.draft.runtime_apply_pending = !runtime_applied;
    });
    rebuild_profile_list();
    if (!runtime_applied) {
        const auto failed_status = application_.status();
        const auto recovery = failed_status.state == ApplicationState::Running
            ? MainWindowCommand::ReloadService
            : MainWindowCommand::StartService;
        return ExecutionResult{
            CommandOutcome::SavedPendingRuntimeApply,
            MainWindowError::RuntimeApplyFailed,
            {},
            {},
            std::move(runtime_error),
            recovery,
        };
    }
    return {};
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::discard_draft() {
    if (!editing_.active()) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::ServiceUnavailable,
            {},
            {},
            "no draft is loaded",
            MainWindowCommand::LoadDraft,
        };
    }
    editing_.discard();
    std::string error;
    if (!editing_.begin(error)) {
        publish_state_change([](MainWindowState& state) {
            state.draft.phase = DraftPhase::Unloaded;
            state.profiles.clear();
            state.selected_profile_id.reset();
        });
        return ExecutionResult{
            CommandOutcome::Failed,
            MainWindowError::Internal,
            {},
            {},
            std::move(error),
            MainWindowCommand::ReloadDraft,
        };
    }
    publish_state_change([](MainWindowState& state) {
        state.draft.phase = state.draft.runtime_apply_pending
            ? DraftPhase::SavedPendingRuntimeApply
            : DraftPhase::Clean;
    });
    rebuild_profile_list();
    return {};
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::set_lightweight_mode(bool enabled) {
    auto candidate = preference_values_;
    candidate.lightweight_mode = enabled;
    std::string error;
    if (!preferences_.save(candidate, error)) {
        return ExecutionResult{
            CommandOutcome::Failed,
            MainWindowError::PersistenceFailed,
            {},
            "main_window.lightweight_mode",
            std::move(error),
            std::nullopt,
        };
    }
    preference_values_ = candidate;
    publish_state_change([&](MainWindowState& state) {
        state.lightweight_mode = enabled;
    });
    return {};
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::execute_service_command(
    MainWindowCommand command) {
    std::string error;
    bool succeeded = false;
    switch (command) {
    case MainWindowCommand::StartService:
        succeeded = application_.start(error);
        break;
    case MainWindowCommand::StopService:
        succeeded = application_.stop(error);
        break;
    case MainWindowCommand::ReloadService:
        succeeded = application_.reload(error);
        break;
    case MainWindowCommand::QuitApplication:
        succeeded = application_.shutdown(error);
        break;
    default:
        break;
    }
    if (!succeeded) {
        return ExecutionResult{
            CommandOutcome::Failed,
            MainWindowError::ServiceUnavailable,
            {},
            {},
            std::move(error),
            std::nullopt,
        };
    }
    if (command == MainWindowCommand::StartService
        || command == MainWindowCommand::ReloadService) {
        publish_state_change([](MainWindowState& state) {
            state.draft.runtime_apply_pending = false;
            if (state.draft.phase == DraftPhase::SavedPendingRuntimeApply) {
                state.draft.phase = DraftPhase::Clean;
            }
        });
    }
    return {};
}

void MainWindowViewModel::rebuild_profile_list(
    const std::optional<std::string>& preferred_selection) {
    std::vector<ProfileListItem> profiles;
    if (editing_.active()) {
        const auto protocols = builtin_protocol_registry();
        const auto rules = builtin_rule_registry();
        profiles.reserve(editing_.draft().profiles.size());
        for (const auto& [profile_id, profile] : editing_.draft().profiles) {
            profiles.push_back(make_profile_list_item(
                profile_id, profile, protocols, rules));
        }
    }
    sort_profile_list_items(profiles);
    publish_state_change([&](MainWindowState& state) {
        state.profiles = std::move(profiles);
        const auto selection_exists = [&](const std::optional<std::string>& selection) {
            return selection && find_profile_list_item(state, *selection) != nullptr;
        };
        if (selection_exists(preferred_selection)) {
            state.selected_profile_id = preferred_selection;
        } else if (!selection_exists(state.selected_profile_id)) {
            state.selected_profile_id = state.profiles.empty()
                ? std::nullopt
                : std::optional<std::string>{state.profiles.front().id};
        }
    });
}

void MainWindowViewModel::publish_state_change(
    const std::function<void(MainWindowState&)>& update) {
    MainWindowStateSnapshot current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        update(state_);
        ++state_.revision;
        current = snapshot_locked();
    }
    notify(current);
}

MainWindowStateSnapshot MainWindowViewModel::snapshot_locked() const {
    return std::make_shared<const MainWindowState>(state_);
}

void MainWindowViewModel::notify(const MainWindowStateSnapshot& state) const {
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(callback_state_->mutex);
        if (!callback_state_->active || !callback_state_->handler) {
            return;
        }
        generation = callback_state_->generation;
    }
    auto deliver = [callbacks = callback_state_, generation, state]() {
        MainWindowUpdateHandler handler;
        {
            std::lock_guard<std::mutex> lock(callbacks->mutex);
            if (!callbacks->active
                || callbacks->generation != generation
                || !callbacks->handler) {
                return;
            }
            handler = callbacks->handler;
        }
        try {
            handler(state);
        } catch (...) {
            // UI callback failures cannot alter command execution or shared state.
        }
    };
    if (dispatcher_) {
        try {
            dispatcher_(std::move(deliver));
        } catch (...) {
            // A platform dispatcher failure is isolated from the control executor.
        }
    } else {
        deliver();
    }
}

MainWindowError MainWindowViewModel::classify_validation_error(const std::string& error) {
    if (error.find("route collision") != std::string::npos) {
        return MainWindowError::RouteCollision;
    }
    return MainWindowError::ValidationFailed;
}

MainWindowError MainWindowViewModel::classify_repository_error(
    ConfigRepositoryFailure failure) {
    switch (failure) {
    case ConfigRepositoryFailure::Busy:
        return MainWindowError::Busy;
    case ConfigRepositoryFailure::Stale:
        return MainWindowError::RepositoryStale;
    case ConfigRepositoryFailure::InvalidDocument:
        return MainWindowError::ValidationFailed;
    case ConfigRepositoryFailure::MigrationRequired:
    case ConfigRepositoryFailure::RecoveryRequired:
    case ConfigRepositoryFailure::Constraint:
    case ConfigRepositoryFailure::Corrupt:
    case ConfigRepositoryFailure::UnsupportedSchema:
    case ConfigRepositoryFailure::None:
    case ConfigRepositoryFailure::NotLoaded:
    case ConfigRepositoryFailure::Io:
        return MainWindowError::PersistenceFailed;
    }
    return MainWindowError::PersistenceFailed;
}

} // namespace ccs
