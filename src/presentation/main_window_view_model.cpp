#include "presentation/main_window_view_model.hpp"

#include "config/composite_config_repository.hpp"
#include "config/configuration_conversion.hpp"
#include "protocols/protocol_registry.hpp"
#include "rules/rule_registry.hpp"

#include <algorithm>
#include <filesystem>
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
    const StoredProfile& stored,
    const ProfileDefinition& profile,
    const std::shared_ptr<const ProtocolRegistry>& protocols,
    const std::shared_ptr<const RuleRegistry>& rules) {
    ProfileListItem item;
    const auto& profile_id = stored.profile_id;
    item.key = stored.key;
    item.id = stored.profile_id;
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

ConfigurationFieldState make_field_state(
    const ConfigurationFieldDescriptor& descriptor,
    std::optional<ConfigurationFieldValue> value) {
    ConfigurationFieldState state;
    state.key = descriptor.key;
    state.scope = descriptor.scope;
    state.input_kind = descriptor.input_kind;
    state.required = descriptor.required;
    state.minimum = descriptor.minimum;
    state.maximum = descriptor.maximum;
    state.enum_values.reserve(descriptor.enum_values.size());
    for (const auto entry : descriptor.enum_values) {
        state.enum_values.emplace_back(entry);
    }
    state.display_name_key = descriptor.display_name_key;
    state.apply_impact = descriptor.apply_impact;
    state.value = std::move(value);
    return state;
}

bool build_application_fields(
    const ApplicationSettings& application,
    std::vector<ConfigurationFieldState>& fields,
    std::string& error) {
    fields.clear();
    fields.reserve(application_field_descriptors().size());
    for (const auto& descriptor : application_field_descriptors()) {
        ConfigurationFieldValue value;
        if (!read_application_field(application, descriptor, value, error)) {
            return false;
        }
        fields.push_back(make_field_state(descriptor, std::move(value)));
    }
    return true;
}

bool build_profile_editor(
    const StoredProfile& profile,
    ProfileEditorState& editor,
    std::string& error) {
    editor = {};
    editor.key = profile.key;
    editor.profile_id = profile.profile_id;
    editor.fields.reserve(profile_field_descriptors().size());
    for (const auto& descriptor : profile_field_descriptors()) {
        std::optional<ConfigurationFieldValue> value;
        if (!read_profile_field(profile, descriptor, value, error)) {
            return false;
        }
        editor.fields.push_back(make_field_state(descriptor, std::move(value)));
    }
    return true;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()),
        reinterpret_cast<const char*>(value.data() + value.size()));
}

MainWindowStorageState storage_state_from(StorageState state) noexcept {
    switch (state) {
    case StorageState::Uninitialized:
        return MainWindowStorageState::Uninitialized;
    case StorageState::MigrationRequired:
        return MainWindowStorageState::MigrationRequired;
    case StorageState::Ready:
        return MainWindowStorageState::Ready;
    case StorageState::RecoveryRequired:
        return MainWindowStorageState::RecoveryRequired;
    case StorageState::Invalid:
        return MainWindowStorageState::Invalid;
    }
    return MainWindowStorageState::Unknown;
}

} // namespace

MainWindowViewModel::MainWindowViewModel(
    ConfigurationRepository& repository,
    ConfigurationEditor& editing,
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
            result.source = request.source;
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
        result.source = request.source;
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
        result.source = request.source;
        state.last_command = std::move(result);
    });
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::execute_command(
    const MainWindowCommandRequest& request) {
    if (const auto precondition = check_draft_precondition(request)) {
        return *precondition;
    }
    switch (request.command) {
    case MainWindowCommand::LoadDraft:
        return load_draft(false, request.unsaved_changes_decision);
    case MainWindowCommand::ReloadDraft:
        return load_draft(true, request.unsaved_changes_decision);
    case MainWindowCommand::CreateProfile:
    case MainWindowCommand::RenameProfile:
    case MainWindowCommand::RemoveProfile:
    case MainWindowCommand::MoveProfile:
    case MainWindowCommand::SetProfileEnabled:
        return mutate_draft(request);
    case MainWindowCommand::SaveProfile:
    case MainWindowCommand::UpdateProfileFields:
    case MainWindowCommand::UpdateApplicationFields:
        return update_fields(request);
    case MainWindowCommand::ReplaceRulesText:
        return update_rules(request, false);
    case MainWindowCommand::FormatRulesText:
        return update_rules(request, true);
    case MainWindowCommand::ApplyDraft:
        return apply_draft();
    case MainWindowCommand::DiscardDraft:
        return discard_draft();
    case MainWindowCommand::SetLightweightMode:
        return set_lightweight_mode(request.enabled);
    case MainWindowCommand::StorageStatus:
        return inspect_storage();
    case MainWindowCommand::MigrateStorage:
        return migrate_storage(request);
    case MainWindowCommand::StartService:
    case MainWindowCommand::StopService:
    case MainWindowCommand::ReloadService:
    case MainWindowCommand::QuitApplication:
        return execute_service_command(request.command);
    case MainWindowCommand::Refresh:
        rebuild_editor_state({}, {}, true);
        return {};
    case MainWindowCommand::SelectProfile:
        if (request.profile_key) {
            if (find_profile_list_item(*snapshot(), *request.profile_key) == nullptr) {
                return ExecutionResult{
                    CommandOutcome::Rejected,
                    MainWindowError::ProfileNotFound,
                    request.profile_id,
                    {},
                    "profile key no longer exists",
                    std::nullopt,
                };
            }
            publish_state_change([&](MainWindowState& state) {
                (void)select_profile(state, *request.profile_key);
            });
            rebuild_editor_state(*request.profile_key);
            return {};
        }
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
        rebuild_editor_state({}, request.profile_id);
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

std::optional<MainWindowViewModel::ExecutionResult>
MainWindowViewModel::check_draft_precondition(
    const MainWindowCommandRequest& request) const {
    if (!request.expected_draft_revision && !request.expected_base_revision) {
        return std::nullopt;
    }
    const auto current = snapshot();
    if (request.expected_draft_revision
        && *request.expected_draft_revision != current->draft.revision) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::DraftStale,
            request.profile_id,
            {},
            "draft revision changed",
            MainWindowCommand::ReloadDraft,
        };
    }
    if (request.expected_base_revision
        && *request.expected_base_revision != current->draft.base_revision) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::RepositoryStale,
            request.profile_id,
            {},
            "repository base revision changed",
            MainWindowCommand::ReloadDraft,
        };
    }
    return std::nullopt;
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
            ++state.draft.revision;
            state.draft.base_revision.clear();
            state.profiles.clear();
            state.application_fields.clear();
            clear_profile_selection(state);
        });
        std::string storage_error;
        (void)refresh_storage_status(storage_error);
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
            ++state.draft.revision;
            state.draft.base_revision.clear();
            state.profiles.clear();
            state.application_fields.clear();
            clear_profile_selection(state);
        });
        std::string storage_error;
        (void)refresh_storage_status(storage_error);
        return ExecutionResult{
            CommandOutcome::Failed,
            MainWindowError::Internal,
            {},
            {},
            std::move(error),
            MainWindowCommand::ReloadDraft,
        };
    }

    const auto base_revision = repository_revision_token(editing_.draft().revision);
    publish_state_change([&](MainWindowState& state) {
        state.lightweight_mode = preference_values_.lightweight_mode;
        state.draft.phase = state.draft.runtime_apply_pending
            ? DraftPhase::SavedPendingRuntimeApply
            : DraftPhase::Clean;
        ++state.draft.revision;
        state.draft.base_revision = base_revision;
    });
    rebuild_editor_state();
    std::string storage_error;
    (void)refresh_storage_status(storage_error);
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

    const auto before = editing_.draft();
    std::string error;
    std::optional<ProfileKey> preferred_key = request.profile_key;
    std::string result_profile_id = request.profile_id;

    if (request.command == MainWindowCommand::CreateProfile) {
        const bool profile_already_exists =
            editing_.find_profile_by_id(request.profile_id) != nullptr;
        ProfileKey created_key = 0;
        if (!editing_.create_profile(request.profile_id, created_key, error)) {
            return ExecutionResult{
                CommandOutcome::Rejected,
                profile_already_exists
                    ? MainWindowError::ProfileAlreadyExists
                    : MainWindowError::InvalidArgument,
                request.profile_id,
                "profile_id",
                std::move(error),
                std::nullopt,
            };
        }
        preferred_key = created_key;
    } else {
        const StoredProfile* selected = nullptr;
        if (request.profile_key) {
            const auto found = std::find_if(
                editing_.draft().profiles.begin(),
                editing_.draft().profiles.end(),
                [&](const auto& profile) { return profile.key == *request.profile_key; });
            if (found != editing_.draft().profiles.end()) {
                selected = &*found;
            }
        } else {
            selected = editing_.find_profile_by_id(request.profile_id);
        }
        if (selected == nullptr) {
            return ExecutionResult{
                CommandOutcome::Rejected,
                MainWindowError::ProfileNotFound,
                request.profile_id,
                "profile_id",
                "profile does not exist: " + request.profile_id,
                std::nullopt,
            };
        }
        const auto profile_key = selected->key;
        preferred_key = profile_key;
        result_profile_id = selected->profile_id;
        if (request.command == MainWindowCommand::RenameProfile) {
            const std::array set_commands = {SetConfigurationFieldCommand{
                profile_key,
                "id",
                request.replacement_profile_id,
            }};
            if (!editing_.apply_batch(set_commands, {}, false, error)) {
                const auto failure = editing_.last_failure();
                return ExecutionResult{
                    CommandOutcome::Rejected,
                    classify_editor_error(failure.code),
                    result_profile_id,
                    failure.field.empty() ? "id" : failure.field,
                    std::move(error),
                    std::nullopt,
                };
            }
            result_profile_id = request.replacement_profile_id;
        } else if (request.command == MainWindowCommand::RemoveProfile) {
            if (!editing_.remove_profile(profile_key, error)) {
                return ExecutionResult{
                    CommandOutcome::Rejected,
                    MainWindowError::ProfileNotFound,
                    result_profile_id,
                    {},
                    std::move(error),
                    std::nullopt,
                };
            }
            preferred_key.reset();
        } else if (request.command == MainWindowCommand::MoveProfile) {
            if (!editing_.move_profile(profile_key, request.position, error)) {
                return ExecutionResult{
                    CommandOutcome::Rejected,
                    MainWindowError::InvalidArgument,
                    result_profile_id,
                    "position",
                    std::move(error),
                    std::nullopt,
                };
            }
        } else {
            const std::array set_commands = {SetConfigurationFieldCommand{
                profile_key,
                "enabled",
                request.enabled,
            }};
            if (!editing_.apply_batch(set_commands, {}, true, error)) {
                const auto failure = editing_.last_failure();
                return ExecutionResult{
                    CommandOutcome::Rejected,
                    classify_editor_error(failure.code),
                    result_profile_id,
                    failure.field.empty() ? "enabled" : failure.field,
                    std::move(error),
                    std::nullopt,
                };
            }
        }
    }

    if (editing_.draft() == before) {
        return ExecutionResult{
            CommandOutcome::Succeeded,
            MainWindowError::None,
            result_profile_id,
            {},
            {},
            std::nullopt,
        };
    }
    publish_state_change([](MainWindowState& state) {
        state.draft.phase = DraftPhase::Dirty;
        ++state.draft.revision;
    });
    rebuild_editor_state(preferred_key, result_profile_id);
    return ExecutionResult{
        CommandOutcome::Succeeded,
        MainWindowError::None,
        result_profile_id,
        {},
        {},
        std::nullopt,
    };
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::update_fields(
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
    std::optional<ProfileKey> profile_key;
    if (request.command == MainWindowCommand::SaveProfile
        || request.command == MainWindowCommand::UpdateProfileFields) {
        profile_key = request.profile_key;
        if (!profile_key) {
            const auto* profile = editing_.find_profile_by_id(request.profile_id);
            if (profile != nullptr) {
                profile_key = profile->key;
            }
        }
        if (!profile_key) {
            return ExecutionResult{
                CommandOutcome::Rejected,
                MainWindowError::ProfileNotFound,
                request.profile_id,
                {},
                "selected profile no longer exists",
                std::nullopt,
            };
        }
    }

    std::vector<SetConfigurationFieldCommand> set_commands;
    std::vector<ResetConfigurationFieldCommand> reset_commands;
    set_commands.reserve(request.field_edits.size());
    reset_commands.reserve(request.field_edits.size());
    for (const auto& edit : request.field_edits) {
        if (edit.value) {
            set_commands.push_back({profile_key, edit.key, *edit.value});
        } else {
            reset_commands.push_back({profile_key, edit.key});
        }
    }
    const auto before = editing_.draft();
    std::string error;
    if (!editing_.apply_batch(set_commands, reset_commands, true, error)) {
        const auto failure = editing_.last_failure();
        return ExecutionResult{
            CommandOutcome::Rejected,
            classify_editor_error(failure.code),
            request.profile_id,
            failure.field,
            std::move(error),
            std::nullopt,
        };
    }
    if (editing_.draft() != before) {
        publish_state_change([](MainWindowState& state) {
            state.draft.phase = DraftPhase::Dirty;
            ++state.draft.revision;
        });
    }
    std::string result_profile_id = request.profile_id;
    if (profile_key) {
        const auto profile = std::find_if(
            editing_.draft().profiles.begin(),
            editing_.draft().profiles.end(),
            [&](const auto& candidate) { return candidate.key == *profile_key; });
        if (profile != editing_.draft().profiles.end()) {
            result_profile_id = profile->profile_id;
        }
    }
    rebuild_editor_state(profile_key, result_profile_id);
    return ExecutionResult{
        CommandOutcome::Succeeded,
        MainWindowError::None,
        result_profile_id,
        {},
        {},
        std::nullopt,
    };
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::update_rules(
    const MainWindowCommandRequest& request,
    bool format) {
    if (!editing_.active() || !request.profile_key) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::ServiceUnavailable,
            request.profile_id,
            "rules",
            "no selected Profile draft is available",
            MainWindowCommand::LoadDraft,
        };
    }
    const auto before = editing_.draft();
    const auto normalized_text = normalize_rules_text_newlines(request.text);
    RulesTextError diagnostic;
    std::string error;
    if (!editing_.replace_rules_text(
            *request.profile_key, normalized_text, diagnostic, error)) {
        publish_state_change([&](MainWindowState& state) {
            state.rules_editor = RulesEditorState{
                *request.profile_key,
                request.profile_id,
                normalized_text,
                diagnostic,
            };
            ++state.draft.revision;
        });
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::ValidationFailed,
            request.profile_id,
            "rules",
            std::move(error),
            std::nullopt,
        };
    }
    std::string displayed = normalized_text;
    if (format
        && !editing_.format_rules_text(*request.profile_key, displayed, diagnostic, error)) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::ValidationFailed,
            request.profile_id,
            "rules",
            std::move(error),
            std::nullopt,
        };
    }
    if (editing_.draft() != before) {
        publish_state_change([](MainWindowState& state) {
            state.draft.phase = DraftPhase::Dirty;
        });
    }
    rebuild_editor_state(*request.profile_key, request.profile_id);
    publish_state_change([&](MainWindowState& state) {
        if (state.rules_editor && state.rules_editor->profile_key == *request.profile_key) {
            state.rules_editor->text = std::move(displayed);
            state.rules_editor->diagnostic.reset();
        }
        ++state.draft.revision;
    });
    return {};
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
        const auto failure = editing_.last_failure();
        publish_state_change([](MainWindowState& state) {
            state.draft.phase = DraftPhase::Dirty;
        });
        return ExecutionResult{
            CommandOutcome::Rejected,
            classify_editor_error(failure.code),
            {},
            failure.field,
            std::move(error),
            std::nullopt,
        };
    }
    publish_state_change([](MainWindowState& state) {
        state.draft.phase = DraftPhase::Applying;
    });
    const auto selected_before_commit = snapshot()->selected_profile_id;
    ConfigurationSnapshot committed;
    if (!editing_.commit(committed, error)) {
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
            ++state.draft.revision;
            state.draft.base_revision = repository_revision_token(committed.revision);
            state.profiles.clear();
            state.application_fields.clear();
            clear_profile_selection(state);
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
    const auto base_revision = repository_revision_token(editing_.draft().revision);
    publish_state_change([&](MainWindowState& state) {
        state.draft.phase = runtime_applied
            ? DraftPhase::Clean
            : DraftPhase::SavedPendingRuntimeApply;
        state.draft.runtime_apply_pending = !runtime_applied;
        ++state.draft.revision;
        state.draft.base_revision = base_revision;
    });
    rebuild_editor_state({}, selected_before_commit);
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
            ++state.draft.revision;
            state.draft.base_revision.clear();
            state.profiles.clear();
            state.application_fields.clear();
            clear_profile_selection(state);
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
    const auto base_revision = repository_revision_token(editing_.draft().revision);
    publish_state_change([&](MainWindowState& state) {
        state.draft.phase = state.draft.runtime_apply_pending
            ? DraftPhase::SavedPendingRuntimeApply
            : DraftPhase::Clean;
        ++state.draft.revision;
        state.draft.base_revision = base_revision;
    });
    rebuild_editor_state();
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

bool MainWindowViewModel::refresh_storage_status(std::string& error) {
    error.clear();
    MainWindowStorageStatus visible;
    visible.database_path = path_to_utf8(repository_.paths().profiles_database);
    visible.backup_directory = path_to_utf8(repository_.paths().migrations_directory);
    std::error_code exists_error;
    visible.database_exists = std::filesystem::exists(
        repository_.paths().profiles_database, exists_error);
    if (exists_error) {
        error = "failed to inspect profiles.db: " + exists_error.message();
        visible.state = MainWindowStorageState::Unknown;
        visible.detail = error;
        publish_state_change([&](MainWindowState& state) {
            state.storage = std::move(visible);
        });
        return false;
    }

    auto* repository = dynamic_cast<CompositeConfigRepository*>(&repository_);
    if (repository == nullptr) {
        error = "storage inspection is unavailable for this repository";
        visible.detail = error;
        publish_state_change([&](MainWindowState& state) {
            state.storage = std::move(visible);
        });
        return false;
    }

    StorageStatus inspected;
    const bool succeeded = repository->inspect_storage(inspected, error);
    visible.state = storage_state_from(inspected.state);
    visible.detail = inspected.detail.empty() ? error : inspected.detail;
    publish_state_change([&](MainWindowState& state) {
        state.storage = std::move(visible);
    });
    return succeeded;
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::inspect_storage() {
    std::string error;
    if (!refresh_storage_status(error)) {
        return ExecutionResult{
            CommandOutcome::Failed,
            MainWindowError::PersistenceFailed,
            {},
            "storage",
            std::move(error),
            std::nullopt,
        };
    }
    return {};
}

MainWindowViewModel::ExecutionResult MainWindowViewModel::migrate_storage(
    const MainWindowCommandRequest& request) {
    auto* repository = dynamic_cast<CompositeConfigRepository*>(&repository_);
    if (repository == nullptr) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::ServiceUnavailable,
            {},
            "storage",
            "storage migration is unavailable for this repository",
            std::nullopt,
        };
    }
    const auto current = snapshot();
    if (current->draft.dirty()) {
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::UnsavedChangesDecisionRequired,
            {},
            "storage",
            "apply or discard the current draft before migrating storage",
            std::nullopt,
        };
    }

    StorageStatus status;
    std::string error;
    if (!repository->inspect_storage(status, error)) {
        const auto inspection_error = error;
        std::string visible_error;
        (void)refresh_storage_status(visible_error);
        return ExecutionResult{
            CommandOutcome::Failed,
            MainWindowError::PersistenceFailed,
            {},
            "storage",
            inspection_error,
            std::nullopt,
        };
    }
    if (status.state == StorageState::Ready) {
        (void)refresh_storage_status(error);
        return {};
    }
    if (status.state != StorageState::MigrationRequired) {
        (void)refresh_storage_status(error);
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::PersistenceFailed,
            {},
            "storage",
            status.detail.empty() ? "storage is not ready for migration" : status.detail,
            std::nullopt,
        };
    }

    std::error_code exists_error;
    const bool database_exists = std::filesystem::exists(
        repository_.paths().profiles_database, exists_error);
    if (exists_error) {
        return ExecutionResult{
            CommandOutcome::Failed,
            MainWindowError::PersistenceFailed,
            {},
            "storage",
            "failed to inspect profiles.db: " + exists_error.message(),
            std::nullopt,
        };
    }
    if (database_exists
        && (!request.replace_existing_storage
            || request.replacement_confirmation != "REPLACE")) {
        (void)refresh_storage_status(error);
        return ExecutionResult{
            CommandOutcome::Rejected,
            MainWindowError::ReplacementConfirmationRequired,
            {},
            "storage",
            "profiles.db already exists and requires explicit replacement confirmation",
            std::nullopt,
        };
    }

    MigrationResult result;
    if (!repository->migrate_v2(
            MigrationOptions{request.replace_existing_storage}, result, error)) {
        const auto failure = repository->last_failure();
        const auto migration_error = error;
        std::string visible_error;
        (void)refresh_storage_status(visible_error);
        return ExecutionResult{
            CommandOutcome::Failed,
            failure == ConfigRepositoryFailure::Constraint && database_exists
                ? MainWindowError::ReplacementConfirmationRequired
                : classify_repository_error(failure),
            {},
            "storage",
            migration_error,
            std::nullopt,
        };
    }

    const auto loaded = load_draft(true, UnsavedChangesDecision::Discard);
    if (loaded.error != MainWindowError::None) return loaded;
    std::string visible_error;
    (void)refresh_storage_status(visible_error);
    ExecutionResult completed;
    if (result.replaced_database_backup) {
        completed.detail = "Replaced profiles.db backup: "
            + path_to_utf8(*result.replaced_database_backup);
    }
    return completed;
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

void MainWindowViewModel::rebuild_editor_state(
    std::optional<ProfileKey> preferred_key,
    std::optional<std::string> preferred_id,
    bool preserve_rules_text) {
    std::vector<ProfileListItem> profiles;
    std::vector<ConfigurationFieldState> application_fields;
    std::optional<ProfileEditorState> profile_editor;
    std::optional<RulesEditorState> rules_editor;
    const auto previous = snapshot();
    if (editing_.active()) {
        const auto& draft = editing_.draft();
        const auto protocols = builtin_protocol_registry();
        const auto rules = builtin_rule_registry();
        std::string error;
        (void)build_application_fields(draft.application, application_fields, error);
        profiles.reserve(draft.profiles.size());
        for (const auto& stored : draft.profiles) {
            ConfigurationSnapshot isolated;
            isolated.application = draft.application;
            isolated.profiles.push_back(stored);
            ConfigDocument document;
            error.clear();
            if (configuration_snapshot_to_config_document(isolated, document, error)) {
                profiles.push_back(make_profile_list_item(
                    stored, document.profiles.at(stored.profile_id), protocols, rules));
                continue;
            }
            ProfileListItem item;
            item.key = stored.key;
            item.id = stored.profile_id;
            item.enabled = stored.enabled;
            item.protocol = stored.protocol;
            item.rule_count = stored.rules.size();
            item.enabled_rule_count = static_cast<std::size_t>(std::count_if(
                stored.rules.begin(), stored.rules.end(), [](const auto& rule) {
                    return rule.enabled;
                }));
            item.readiness = ProfileReadiness::Invalid;
            item.status_detail = with_profile_context(stored.profile_id, error);
            profiles.push_back(std::move(item));
        }
    }
    const auto find_key = [&](ProfileKey key) {
        return std::find_if(profiles.begin(), profiles.end(), [&](const auto& profile) {
            return profile.key == key;
        });
    };
    const auto find_id = [&](std::string_view id) {
        return std::find_if(profiles.begin(), profiles.end(), [&](const auto& profile) {
            return profile.id == id;
        });
    };
    auto selected = profiles.end();
    if (preferred_key) selected = find_key(*preferred_key);
    if (selected == profiles.end() && preferred_id) selected = find_id(*preferred_id);
    if (selected == profiles.end() && previous->selected_profile_key) {
        selected = find_key(*previous->selected_profile_key);
    }
    if (selected == profiles.end() && previous->selected_profile_id) {
        selected = find_id(*previous->selected_profile_id);
    }
    if (selected == profiles.end() && !profiles.empty()) {
        selected = profiles.begin();
    }

    if (editing_.active() && selected != profiles.end()) {
        const auto& draft = editing_.draft();
        const auto stored = std::find_if(
            draft.profiles.begin(), draft.profiles.end(), [&](const auto& profile) {
                return profile.key == selected->key;
            });
        if (stored != draft.profiles.end()) {
            std::string error;
            ProfileEditorState built_profile;
            if (build_profile_editor(*stored, built_profile, error)) {
                profile_editor = std::move(built_profile);
            }
            if (preserve_rules_text
                && previous->rules_editor
                && previous->rules_editor->profile_key == stored->key) {
                rules_editor = previous->rules_editor;
            } else {
                std::string text;
                RulesTextError diagnostic;
                if (editing_.format_rules_text(stored->key, text, diagnostic, error)) {
                    rules_editor = RulesEditorState{
                        stored->key,
                        stored->profile_id,
                        std::move(text),
                        std::nullopt,
                    };
                } else {
                    rules_editor = RulesEditorState{
                        stored->key,
                        stored->profile_id,
                        {},
                        diagnostic,
                    };
                }
            }
        }
    }

    const auto selected_id = selected == profiles.end()
        ? std::optional<std::string>{}
        : std::optional<std::string>{selected->id};
    const auto selected_key = selected == profiles.end()
        ? std::optional<ProfileKey>{}
        : std::optional<ProfileKey>{selected->key};

    publish_state_change([&](MainWindowState& state) {
        state.profiles = std::move(profiles);
        state.application_fields = std::move(application_fields);
        state.profile_editor = std::move(profile_editor);
        state.rules_editor = std::move(rules_editor);
        if (!selected_id || !selected_key) {
            clear_profile_selection(state);
        } else {
            state.selected_profile_id = selected_id;
            state.selected_profile_key = selected_key;
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

MainWindowError MainWindowViewModel::classify_editor_error(
    ConfigurationEditError error) {
    switch (error) {
    case ConfigurationEditError::Inactive:
        return MainWindowError::ServiceUnavailable;
    case ConfigurationEditError::ProfileNotFound:
        return MainWindowError::ProfileNotFound;
    case ConfigurationEditError::ProfileAlreadyExists:
        return MainWindowError::ProfileAlreadyExists;
    case ConfigurationEditError::RouteCollision:
        return MainWindowError::RouteCollision;
    case ConfigurationEditError::None:
    case ConfigurationEditError::InvalidField:
    case ConfigurationEditError::ValidationFailed:
    case ConfigurationEditError::RulesInvalid:
        return MainWindowError::ValidationFailed;
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
        return MainWindowError::MigrationRequired;
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
