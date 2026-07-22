#include "config/app_paths.hpp"
#include "config/configuration_conversion.hpp"
#include "config/configuration_repository.hpp"
#include "presentation/main_window_view_model.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class MemoryConfigRepository final : public ccs::ConfigurationRepository {
public:
    explicit MemoryConfigRepository(ccs::AppPaths paths)
        : paths_(std::move(paths)) {}

    bool load(std::string& error) override {
        error.clear();
        if (fail_load_) {
            error = "injected config load failure";
            failure_ = ccs::ConfigRepositoryFailure::Io;
            return false;
        }
        std::vector<ccs::StoredProfile> profiles;
        profiles.reserve(document_.profiles.size());
        for (const auto& [profile_id, definition] : document_.profiles) {
            ccs::StoredProfile profile;
            profile.key = definition.storage_key;
            profile.profile_id = profile_id;
            profile.enabled = definition.enabled;
            if (definition.protocol) profile.protocol = definition.protocol->value;
            profile.local_request_path = definition.local.request_path;
            profile.local_usage_path = definition.local.usage_path;
            profile.upstream_base_url = definition.upstream.base_url;
            profile.upstream_request_path = definition.upstream.request_path;
            profile.upstream_usage_path = definition.upstream.usage_path;
            for (const auto& source_rule : definition.rules) {
                nlohmann::json options = nlohmann::json::object();
                for (const auto& [name, value] : source_rule.options) {
                    options[name] = value;
                }
                profile.rules.push_back({
                    source_rule.storage_key,
                    source_rule.id.value,
                    source_rule.enabled,
                    source_rule.type,
                    options.dump(),
                });
            }
            profiles.push_back(std::move(profile));
        }
        for (auto& profile : profiles) {
            if (profile.key == 0) {
                profile.key = next_profile_key_++;
            }
            for (auto& rule : profile.rules) {
                if (rule.key == 0) {
                    rule.key = next_rule_key_++;
                }
            }
        }
        snapshot_.application = document_.application;
        snapshot_.profiles = std::move(profiles);
        snapshot_.revision.profile_revision = static_cast<ccs::ProfileRevision>(load_count_ + 1);
        loaded_ = true;
        failure_ = ccs::ConfigRepositoryFailure::None;
        ++load_count_;
        return true;
    }

    bool save_snapshot(
        const ccs::ConfigurationSnapshot& desired,
        ccs::ConfigurationSnapshot& committed,
        std::string& error) override {
        error.clear();
        if (save_failure_ != ccs::ConfigRepositoryFailure::None) {
            failure_ = save_failure_;
            error = save_failure_ == ccs::ConfigRepositoryFailure::Stale
                ? "config file changed since it was loaded; reload before saving"
                : "injected config save failure";
            return false;
        }
        committed = desired;
        for (auto& profile : committed.profiles) {
            if (profile.key <= 0) {
                profile.key = next_profile_key_++;
            }
            for (auto& rule : profile.rules) {
                if (rule.key <= 0) {
                    rule.key = next_rule_key_++;
                }
            }
        }
        ++committed.revision.profile_revision;
        if (!ccs::configuration_snapshot_to_config_document(committed, document_, error)) {
            failure_ = ccs::ConfigRepositoryFailure::InvalidDocument;
            return false;
        }
        snapshot_ = committed;
        failure_ = ccs::ConfigRepositoryFailure::None;
        ++save_count_;
        return true;
    }

    bool loaded() const override {
        return loaded_;
    }

    const ccs::ConfigurationSnapshot& snapshot() const override {
        return snapshot_;
    }

    const ccs::AppPaths& paths() const override {
        return paths_;
    }

    ccs::ConfigRepositoryFailure last_failure() const noexcept override {
        return failure_;
    }

    ccs::ConfigDocument& mutable_document() {
        return document_;
    }

    const ccs::ConfigDocument& document() const {
        return document_;
    }

    void fail_save(ccs::ConfigRepositoryFailure failure) {
        save_failure_ = failure;
    }

    std::size_t save_count() const {
        return save_count_;
    }

private:
    ccs::AppPaths paths_;
    ccs::ConfigDocument document_ = ccs::make_default_config_document();
    ccs::ConfigurationSnapshot snapshot_;
    ccs::ConfigRepositoryFailure failure_ = ccs::ConfigRepositoryFailure::None;
    ccs::ConfigRepositoryFailure save_failure_ = ccs::ConfigRepositoryFailure::None;
    bool loaded_ = false;
    bool fail_load_ = false;
    std::size_t load_count_ = 0;
    std::size_t save_count_ = 0;
    ccs::ProfileKey next_profile_key_ = 1;
    ccs::RuleKey next_rule_key_ = 1;
};

class MemoryPreferences final : public ccs::UiPreferencesRepository {
public:
    bool load(ccs::UiPreferences& preferences, std::string& error) override {
        error.clear();
        if (fail_load_) {
            error = "injected preference load failure";
            return false;
        }
        loaded_ = true;
        preferences = preferences_;
        return true;
    }

    bool save(const ccs::UiPreferences& preferences, std::string& error) override {
        error.clear();
        if (!loaded_ || fail_save_) {
            error = "injected preference save failure";
            return false;
        }
        preferences_ = preferences;
        ++save_count_;
        return true;
    }

    ccs::UiPreferences preferences_;
    bool loaded_ = false;
    bool fail_load_ = false;
    bool fail_save_ = false;
    std::size_t save_count_ = 0;
};

class FakeApplicationControl final : public ccs::ApplicationControl {
public:
    bool start(std::string& error) override {
        return execute(ccs::ApplicationState::Running, fail_start_, start_count_, error);
    }

    bool reload(std::string& error) override {
        ++reload_count_;
        if (fail_reload_) {
            error = "injected runtime reload failure";
            if (fault_on_reload_failure_) {
                status_.state = ccs::ApplicationState::Faulted;
            }
            return false;
        }
        status_.state = ccs::ApplicationState::Running;
        error.clear();
        return true;
    }

    bool stop(std::string& error) override {
        return execute(ccs::ApplicationState::Stopped, false, stop_count_, error);
    }

    bool shutdown(std::string& error) override {
        return execute(ccs::ApplicationState::Shutdown, false, shutdown_count_, error);
    }

    ccs::ApplicationStatus status() override {
        return status_;
    }

    void set_state(ccs::ApplicationState state) {
        status_.state = state;
    }

    void block_start() {
        std::lock_guard<std::mutex> lock(block_mutex_);
        block_start_ = true;
        start_entered_ = false;
        release_start_ = false;
    }

    void wait_for_start() {
        std::unique_lock<std::mutex> lock(block_mutex_);
        require(block_cv_.wait_for(lock, 2s, [&]() { return start_entered_; }),
            "start command did not reach the fake controller");
    }

    void release_start() {
        {
            std::lock_guard<std::mutex> lock(block_mutex_);
            release_start_ = true;
        }
        block_cv_.notify_all();
    }

    bool fail_start_ = false;
    bool fail_reload_ = false;
    bool fault_on_reload_failure_ = false;
    std::size_t start_count_ = 0;
    std::size_t reload_count_ = 0;
    std::size_t stop_count_ = 0;
    std::size_t shutdown_count_ = 0;

private:
    bool execute(
        ccs::ApplicationState success_state,
        bool fail,
        std::size_t& count,
        std::string& error) {
        ++count;
        if (success_state == ccs::ApplicationState::Running) {
            std::unique_lock<std::mutex> lock(block_mutex_);
            if (block_start_) {
                start_entered_ = true;
                block_cv_.notify_all();
                block_cv_.wait(lock, [&]() { return release_start_; });
                block_start_ = false;
            }
        }
        if (fail) {
            error = "injected service command failure";
            return false;
        }
        status_.state = success_state;
        error.clear();
        return true;
    }

    ccs::ApplicationStatus status_;
    std::mutex block_mutex_;
    std::condition_variable block_cv_;
    bool block_start_ = false;
    bool start_entered_ = false;
    bool release_start_ = false;
};

ccs::ProfileDefinition complete_profile(const std::string& prefix) {
    ccs::ProfileDefinition profile;
    profile.enabled = true;
    profile.protocol = ccs::ProtocolId{"responses"};
    profile.local.request_path = prefix + "/v1/responses";
    profile.upstream.base_url = "https://example.com";
    profile.upstream.request_path = "/v1/responses";
    return profile;
}

ccs::RuleDefinition enabled_rule(
    std::string id,
    std::string type,
    std::map<std::string, nlohmann::json> options) {
    ccs::RuleDefinition rule;
    rule.id.value = std::move(id);
    rule.enabled = true;
    rule.type = std::move(type);
    rule.options = std::move(options);
    return rule;
}

ccs::MainWindowStateSnapshot wait_for_result(
    ccs::MainWindowViewModel& view_model,
    ccs::MainWindowCommand command) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto state = view_model.snapshot();
        if (!state->command_pending
            && state->last_command
            && state->last_command->command == command) {
            return state;
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("timed out waiting for view-model command result");
}

ccs::MainWindowStateSnapshot submit_and_wait(
    ccs::MainWindowViewModel& view_model,
    ccs::MainWindowCommandRequest request) {
    const auto command = request.command;
    require(view_model.submit(std::move(request)), "view model rejected command submission");
    return wait_for_result(view_model, command);
}

const ccs::ConfigurationFieldState* find_field(
    const std::vector<ccs::ConfigurationFieldState>& fields,
    std::string_view key) {
    const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& field) {
        return field.key == key;
    });
    return found == fields.end() ? nullptr : &*found;
}

struct Fixture {
    Fixture()
        : root(std::filesystem::temp_directory_path()
              / ("ccs-trans-view-model-"
                  + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
        , repository(ccs::make_app_paths(root))
        , editing(repository)
        , view_model(repository, editing, application, preferences) {}

    ~Fixture() {
        view_model.stop();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path root;
    MemoryConfigRepository repository;
    ccs::ConfigurationEditor editing;
    FakeApplicationControl application;
    MemoryPreferences preferences;
    ccs::MainWindowViewModel view_model;
};

void test_load_profile_commands_and_stopped_apply() {
    Fixture fixture;
    auto zeta = complete_profile("/zeta");
    zeta.rules.push_back(enabled_rule(
        "remove-image", "remove_tool", {{"tool", "image_gen"}}));
    auto disabled_rule = enabled_rule(
        "remove-field", "remove_field", {{"path", "/metadata"}});
    disabled_rule.enabled = false;
    zeta.rules.push_back(std::move(disabled_rule));
    fixture.repository.mutable_document().profiles.emplace("zeta", std::move(zeta));
    fixture.repository.mutable_document().profiles.emplace("alpha", complete_profile("/alpha"));
    fixture.preferences.preferences_.lightweight_mode = false;

    auto state = submit_and_wait(
        fixture.view_model, {ccs::MainWindowCommand::LoadDraft});
    require(state->last_command->succeeded() && state->draft.phase == ccs::DraftPhase::Clean,
        "LoadDraft publishes a clean draft");
    require(!state->lightweight_mode && state->profiles.size() == 2
            && state->profiles[0].id == "alpha"
            && state->profiles[1].id == "zeta",
        "LoadDraft applies preferences and stable Profile ordering");
    require(state->profiles[1].rule_count == 2
            && state->profiles[1].enabled_rule_count == 1,
        "Profile list exposes total and enabled Rule counts");

    state = submit_and_wait(
        fixture.view_model,
        {ccs::MainWindowCommand::CreateProfile, "draft"});
    require(state->last_command->succeeded()
            && state->draft.phase == ccs::DraftPhase::Dirty
            && state->selected_profile_id == "draft",
        "CreateProfile creates a selected disabled draft");
    const auto* created = ccs::find_profile_list_item(*state, "draft");
    require(created != nullptr
            && !created->enabled
            && created->readiness == ccs::ProfileReadiness::Incomplete,
        "new Profile is disabled and visibly incomplete");

    state = submit_and_wait(
        fixture.view_model,
        {ccs::MainWindowCommand::RenameProfile, "draft", "renamed"});
    require(state->last_command->succeeded()
            && state->selected_profile_id == "renamed"
            && ccs::find_profile_list_item(*state, "draft") == nullptr,
        "RenameProfile atomically moves the stable id and selection");

    state = submit_and_wait(
        fixture.view_model,
        {ccs::MainWindowCommand::ReloadDraft, {}, {}, false,
            ccs::UnsavedChangesDecision::Cancel});
    require(state->last_command->outcome == ccs::CommandOutcome::Cancelled
            && state->draft.phase == ccs::DraftPhase::Dirty
            && ccs::find_profile_list_item(*state, "renamed") != nullptr,
        "Cancel preserves a dirty draft during ReloadDraft");

    state = submit_and_wait(
        fixture.view_model,
        {ccs::MainWindowCommand::SetProfileEnabled, "renamed", {}, true});
    require(state->last_command->outcome == ccs::CommandOutcome::Rejected
            && state->last_command->error == ccs::MainWindowError::ValidationFailed
            && !ccs::find_profile_list_item(*state, "renamed")->enabled,
        "incomplete Profile cannot be enabled and failed mutation is atomic");

    state = submit_and_wait(
        fixture.view_model, {ccs::MainWindowCommand::ApplyDraft});
    require(state->last_command->succeeded()
            && state->last_command->configuration_saved()
            && state->draft.phase == ccs::DraftPhase::Clean,
        "stopped Apply persists and reloads a clean editor draft");
    require(fixture.repository.save_count() == 1
            && fixture.application.reload_count_ == 0
            && fixture.repository.document().profiles.contains("renamed"),
        "stopped Apply does not start or reload the runtime");
    state = submit_and_wait(
        fixture.view_model, {ccs::MainWindowCommand::ApplyDraft});
    require(state->last_command->outcome == ccs::CommandOutcome::Rejected
            && state->last_command->error == ccs::MainWindowError::InvalidArgument
            && fixture.repository.save_count() == 1,
        "clean Apply is rejected without another save or runtime command");
}

void test_stale_save_and_runtime_reload_recovery() {
    Fixture stale;
    stale.repository.mutable_document().profiles.emplace("first", complete_profile("/first"));
    (void)submit_and_wait(stale.view_model, {ccs::MainWindowCommand::LoadDraft});
    (void)submit_and_wait(
        stale.view_model,
        {ccs::MainWindowCommand::RenameProfile, "first", "renamed"});
    stale.repository.fail_save(ccs::ConfigRepositoryFailure::Stale);
    auto state = submit_and_wait(stale.view_model, {ccs::MainWindowCommand::ApplyDraft});
    require(state->last_command->error == ccs::MainWindowError::RepositoryStale
            && state->last_command->recovery_command == ccs::MainWindowCommand::ReloadDraft
            && state->draft.phase == ccs::DraftPhase::Dirty,
        "stale Apply preserves dirty draft and offers ReloadDraft recovery");
    require(stale.repository.document().profiles.contains("first")
            && !stale.repository.document().profiles.contains("renamed"),
        "stale Apply cannot overwrite repository state");

    stale.repository.mutable_document().profiles.clear();
    stale.repository.mutable_document().profiles.emplace(
        "external-alpha", complete_profile("/external-alpha"));
    stale.repository.mutable_document().profiles.emplace(
        "external-zeta", complete_profile("/external-zeta"));
    state = submit_and_wait(stale.view_model, {ccs::MainWindowCommand::ReloadDraft});
    require(state->last_command->error
                == ccs::MainWindowError::UnsavedChangesDecisionRequired
            && state->draft.phase == ccs::DraftPhase::Dirty
            && ccs::find_profile_list_item(*state, "renamed") != nullptr
            && ccs::find_profile_list_item(*state, "external-alpha") == nullptr,
        "Reload Draft never overwrites a dirty GUI draft without an explicit decision");
    state = submit_and_wait(
        stale.view_model,
        {ccs::MainWindowCommand::ReloadDraft, {}, {}, false,
            ccs::UnsavedChangesDecision::Cancel});
    require(state->last_command->outcome == ccs::CommandOutcome::Cancelled
            && ccs::find_profile_list_item(*state, "renamed") != nullptr,
        "Reload Draft Cancel preserves the stale GUI draft");
    state = submit_and_wait(
        stale.view_model,
        {ccs::MainWindowCommand::ReloadDraft, {}, {}, false,
            ccs::UnsavedChangesDecision::Discard});
    require(state->last_command->succeeded()
            && state->draft.phase == ccs::DraftPhase::Clean
            && ccs::find_profile_list_item(*state, "renamed") == nullptr
            && ccs::find_profile_list_item(*state, "external-alpha") != nullptr
            && state->selected_profile_id == "external-alpha",
        "explicit Reload Draft Discard loads external state and selects the first stable Profile");

    Fixture running;
    running.repository.mutable_document().profiles.emplace("first", complete_profile("/first"));
    running.application.set_state(ccs::ApplicationState::Running);
    running.application.fail_reload_ = true;
    (void)submit_and_wait(running.view_model, {ccs::MainWindowCommand::LoadDraft});
    (void)submit_and_wait(
        running.view_model,
        {ccs::MainWindowCommand::RenameProfile, "first", "renamed"});
    state = submit_and_wait(running.view_model, {ccs::MainWindowCommand::ApplyDraft});
    require(state->last_command->outcome == ccs::CommandOutcome::SavedPendingRuntimeApply
            && state->last_command->error == ccs::MainWindowError::RuntimeApplyFailed
            && state->last_command->recovery_command == ccs::MainWindowCommand::ReloadService
            && state->draft.runtime_apply_pending
            && state->draft.phase == ccs::DraftPhase::SavedPendingRuntimeApply,
        "runtime reload failure distinguishes persisted configuration from active generation");
    require(running.repository.document().profiles.contains("renamed"),
        "runtime reload failure does not pretend persistence rolled back");

    running.application.fail_reload_ = false;
    state = submit_and_wait(running.view_model, {ccs::MainWindowCommand::ReloadService});
    require(state->last_command->succeeded()
            && !state->draft.runtime_apply_pending
            && state->draft.phase == ccs::DraftPhase::Clean,
        "successful recovery reload clears pending runtime state");

    Fixture faulted;
    faulted.repository.mutable_document().profiles.emplace("first", complete_profile("/first"));
    faulted.application.set_state(ccs::ApplicationState::Running);
    faulted.application.fail_reload_ = true;
    faulted.application.fault_on_reload_failure_ = true;
    (void)submit_and_wait(faulted.view_model, {ccs::MainWindowCommand::LoadDraft});
    (void)submit_and_wait(
        faulted.view_model,
        {ccs::MainWindowCommand::RenameProfile, "first", "renamed"});
    state = submit_and_wait(faulted.view_model, {ccs::MainWindowCommand::ApplyDraft});
    require(state->last_command->recovery_command == ccs::MainWindowCommand::StartService
            && state->application.state == ccs::ApplicationState::Faulted,
        "faulted runtime Apply offers Start recovery instead of an impossible Reload");
    faulted.application.fail_reload_ = false;
    state = submit_and_wait(faulted.view_model, {ccs::MainWindowCommand::StartService});
    require(state->last_command->succeeded()
            && !state->draft.runtime_apply_pending
            && state->application.state == ccs::ApplicationState::Running,
        "Start recovery applies the saved configuration after a fault");
}

void test_reload_selection_and_profile_validation_details() {
    Fixture selection;
    selection.repository.mutable_document().profiles.emplace(
        "alpha", complete_profile("/alpha"));
    selection.repository.mutable_document().profiles.emplace(
        "zeta", complete_profile("/zeta"));
    (void)submit_and_wait(selection.view_model, {ccs::MainWindowCommand::LoadDraft});
    auto state = submit_and_wait(
        selection.view_model,
        {ccs::MainWindowCommand::SelectProfile, "zeta"});
    require(state->selected_profile_id == "zeta",
        "test selected the Profile that must survive reload");
    selection.repository.mutable_document().profiles.emplace(
        "middle", complete_profile("/middle"));
    state = submit_and_wait(selection.view_model, {ccs::MainWindowCommand::ReloadDraft});
    require(state->last_command->succeeded()
            && state->selected_profile_id == "zeta"
            && state->profiles.size() == 3,
        "Reload Draft preserves a selected stable Profile that still exists");

    Fixture validation;
    auto unknown_protocol = complete_profile("/unknown");
    unknown_protocol.enabled = false;
    unknown_protocol.protocol = ccs::ProtocolId{"future_protocol"};
    validation.repository.mutable_document().profiles.emplace(
        "unknown-protocol", std::move(unknown_protocol));

    auto invalid_path = complete_profile("/invalid");
    invalid_path.enabled = false;
    invalid_path.local.request_path = "relative/path";
    validation.repository.mutable_document().profiles.emplace(
        "invalid-path", std::move(invalid_path));

    auto invalid_rule = complete_profile("/rule");
    invalid_rule.enabled = false;
    invalid_rule.rules.push_back(enabled_rule(
        "future", "future_rule", {}));
    validation.repository.mutable_document().profiles.emplace(
        "invalid-rule", std::move(invalid_rule));

    state = submit_and_wait(validation.view_model, {ccs::MainWindowCommand::LoadDraft});
    for (const auto& profile_id : {"unknown-protocol", "invalid-path", "invalid-rule"}) {
        const auto* item = ccs::find_profile_list_item(*state, profile_id);
        require(item != nullptr
                && item->readiness == ccs::ProfileReadiness::Invalid
                && item->status_detail.find("profile " + std::string(profile_id))
                    != std::string::npos,
            "Profile validation detail identifies its owning Profile: "
                + std::string(profile_id));
    }
    require(ccs::find_profile_list_item(*state, "unknown-protocol")
                    ->status_detail.find("unknown protocol")
                != std::string::npos
            && ccs::find_profile_list_item(*state, "invalid-path")
                    ->status_detail.find("local.request_path")
                != std::string::npos
            && ccs::find_profile_list_item(*state, "invalid-rule")
                    ->status_detail.find("unknown type")
                != std::string::npos,
        "Protocol, path, and Rule failures retain actionable field detail");
}

void test_duplicate_command_and_callback_invalidation() {
    Fixture fixture;
    fixture.application.block_start();
    require(fixture.view_model.submit({ccs::MainWindowCommand::StartService}),
        "first service command accepted");
    fixture.application.wait_for_start();
    require(!fixture.view_model.submit({ccs::MainWindowCommand::StopService}),
        "second control command is rejected while the first is running");
    auto state = fixture.view_model.snapshot();
    require(state->command_pending
            && state->last_command
            && state->last_command->command == ccs::MainWindowCommand::StopService
            && state->last_command->error == ccs::MainWindowError::Busy,
        "duplicate command publishes a stable Busy result without entering the queue");
    fixture.application.release_start();
    state = wait_for_result(fixture.view_model, ccs::MainWindowCommand::StartService);
    require(state->last_command->succeeded() && fixture.application.stop_count_ == 0,
        "only the accepted command reaches ApplicationControl");

    std::mutex queue_mutex;
    std::vector<std::function<void()>> queued;
    MemoryConfigRepository repository(ccs::make_app_paths(fixture.root / "callbacks"));
    ccs::ConfigurationEditor editing(repository);
    FakeApplicationControl application;
    MemoryPreferences preferences;
    ccs::MainWindowViewModel dispatched(
        repository, editing, application, preferences,
        [&](std::function<void()> callback) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queued.push_back(std::move(callback));
        });
    std::size_t stale_calls = 0;
    dispatched.set_update_handler([&](ccs::MainWindowStateSnapshot) { ++stale_calls; });
    dispatched.stop();
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        callbacks = std::move(queued);
    }
    for (auto& callback : callbacks) {
        callback();
    }
    require(stale_calls == 0,
        "queued UI callbacks are invalidated when the ViewModel/window subscription stops");
}

void test_preference_failure_preserves_published_value() {
    Fixture fixture;
    auto state = submit_and_wait(fixture.view_model, {ccs::MainWindowCommand::LoadDraft});
    require(state->lightweight_mode, "fixture starts in lightweight mode");
    fixture.preferences.fail_save_ = true;
    state = submit_and_wait(
        fixture.view_model,
        {ccs::MainWindowCommand::SetLightweightMode, {}, {}, false});
    require(state->last_command->error == ccs::MainWindowError::PersistenceFailed
            && state->lightweight_mode,
        "failed preference save leaves the published mode unchanged");
    fixture.preferences.fail_save_ = false;
    state = submit_and_wait(
        fixture.view_model,
        {ccs::MainWindowCommand::SetLightweightMode, {}, {}, false});
    require(state->last_command->succeeded()
            && !state->lightweight_mode
            && fixture.preferences.save_count_ == 1,
        "successful preference save publishes the new lightweight mode");
}

void test_route_collision_is_classified_without_mutating_draft() {
    Fixture fixture;
    fixture.repository.mutable_document().profiles.emplace("first", complete_profile("/shared"));
    auto second = complete_profile("/second");
    second.enabled = false;
    second.local.request_path = "/shared/v1/responses";
    fixture.repository.mutable_document().profiles.emplace("second", std::move(second));
    (void)submit_and_wait(fixture.view_model, {ccs::MainWindowCommand::LoadDraft});
    auto state = submit_and_wait(
        fixture.view_model,
        {ccs::MainWindowCommand::SetProfileEnabled, "second", {}, true});
    require(state->last_command->outcome == ccs::CommandOutcome::Rejected
            && state->last_command->error == ccs::MainWindowError::RouteCollision
            && !ccs::find_profile_list_item(*state, "second")->enabled
            && state->draft.phase == ccs::DraftPhase::Clean,
        "route collision is classified and rejected before mutating the draft");
}

void test_noop_profile_mutations_preserve_clean_draft() {
    Fixture fixture;
    fixture.repository.mutable_document().profiles.emplace("first", complete_profile("/first"));
    (void)submit_and_wait(fixture.view_model, {ccs::MainWindowCommand::LoadDraft});
    auto state = submit_and_wait(
        fixture.view_model,
        {ccs::MainWindowCommand::RenameProfile, "first", "first"});
    require(state->last_command->succeeded() && state->draft.phase == ccs::DraftPhase::Clean,
        "rename to the existing stable id is a clean no-op");
    state = submit_and_wait(
        fixture.view_model,
        {ccs::MainWindowCommand::SetProfileEnabled, "first", {}, true});
    require(state->last_command->succeeded() && state->draft.phase == ccs::DraftPhase::Clean,
        "setting the existing enabled value is a clean no-op");
}

void test_typed_fields_and_rules_text_workflow() {
    Fixture fixture;
    auto profile = complete_profile("/typed");
    profile.rules.push_back(enabled_rule(
        "remove-image", "remove_tool", {{"tool", "image_gen"}}));
    fixture.repository.mutable_document().profiles.emplace("typed", std::move(profile));
    auto state = submit_and_wait(fixture.view_model, {ccs::MainWindowCommand::LoadDraft});
    require(state->selected_profile_key
            && state->profile_editor
            && state->profile_editor->fields.size() == ccs::profile_field_descriptors().size()
            && state->application_fields.size() == ccs::application_field_descriptors().size(),
        "0.7 ViewModel publishes descriptor-driven Profile and application fields");
    require(state->rules_editor
            && state->rules_editor->text.find("ccs-trans.rules/v1") != std::string::npos
            && state->rules_editor->text.find("remove-image") != std::string::npos,
        "selected Profile publishes canonical Rule text");

    ccs::MainWindowCommandRequest profile_update;
    profile_update.command = ccs::MainWindowCommand::UpdateProfileFields;
    profile_update.profile_id = "typed";
    profile_update.profile_key = state->selected_profile_key;
    profile_update.field_edits.push_back({
        "upstream.base-url",
        ccs::ConfigurationFieldValue{std::string{"https://updated.example.com"}},
    });
    state = submit_and_wait(fixture.view_model, std::move(profile_update));
    const auto* upstream = find_field(
        state->profile_editor->fields, "upstream.base-url");
    require(state->last_command->succeeded()
            && state->draft.phase == ccs::DraftPhase::Dirty
            && upstream != nullptr
            && upstream->value
            && std::get<std::string>(*upstream->value) == "https://updated.example.com",
        "typed Profile field update is reflected in the shared editor state");

    ccs::MainWindowCommandRequest invalid_settings;
    invalid_settings.command = ccs::MainWindowCommand::UpdateApplicationFields;
    invalid_settings.field_edits.push_back({
        "listener.port",
        ccs::ConfigurationFieldValue{static_cast<std::uint64_t>(17070)},
    });
    invalid_settings.field_edits.push_back({
        "runtime.max-connections",
        ccs::ConfigurationFieldValue{static_cast<std::uint64_t>(0)},
    });
    state = submit_and_wait(fixture.view_model, std::move(invalid_settings));
    const auto* unchanged_port = find_field(state->application_fields, "listener.port");
    require(state->last_command->error == ccs::MainWindowError::ValidationFailed
            && unchanged_port != nullptr
            && std::get<std::uint64_t>(*unchanged_port->value) == 15723,
        "invalid typed Settings batch is atomic");

    ccs::MainWindowCommandRequest settings_update;
    settings_update.command = ccs::MainWindowCommand::UpdateApplicationFields;
    settings_update.field_edits.push_back({
        "listener.port",
        ccs::ConfigurationFieldValue{static_cast<std::uint64_t>(17070)},
    });
    settings_update.field_edits.push_back({
        "logging.body",
        ccs::ConfigurationFieldValue{false},
    });
    state = submit_and_wait(fixture.view_model, std::move(settings_update));
    const auto* updated_port = find_field(state->application_fields, "listener.port");
    require(state->last_command->succeeded()
            && updated_port != nullptr
            && std::get<std::uint64_t>(*updated_port->value) == 17070,
        "typed Settings update publishes the new draft values");

    ccs::MainWindowCommandRequest invalid_rules;
    invalid_rules.command = ccs::MainWindowCommand::ReplaceRulesText;
    invalid_rules.profile_id = "typed";
    invalid_rules.profile_key = state->selected_profile_key;
    invalid_rules.text = "{\n  invalid";
    state = submit_and_wait(fixture.view_model, std::move(invalid_rules));
    require(state->last_command->error == ccs::MainWindowError::ValidationFailed
            && state->rules_editor
            && state->rules_editor->diagnostic
            && state->rules_editor->text == "{\n  invalid",
        "invalid Rule text preserves editor content and publishes a diagnostic");

    ccs::MainWindowCommandRequest format_rules;
    format_rules.command = ccs::MainWindowCommand::FormatRulesText;
    format_rules.profile_id = "typed";
    format_rules.profile_key = state->selected_profile_key;
    format_rules.text = R"({"schema_version":"ccs-trans.rules/v1","rules":[{"options":{"tool":"web_search"},"type":"remove_tool","enabled":true,"id":"remove-web"}]})";
    state = submit_and_wait(fixture.view_model, std::move(format_rules));
    require(state->last_command->succeeded()
            && state->rules_editor
            && !state->rules_editor->diagnostic
            && state->rules_editor->text.find("\n  \"rules\": [") != std::string::npos
            && state->rules_editor->text.find("remove-web") != std::string::npos,
        "Format validates and publishes canonical Rule text");

    state = submit_and_wait(fixture.view_model, {ccs::MainWindowCommand::ApplyDraft});
    require(state->last_command->succeeded()
            && fixture.repository.document().application.listener.port == 17070
            && fixture.repository.document().profiles.at("typed").rules.front().id.value
                == "remove-web",
        "Apply persists Profile fields, Settings, and Rule text together");
}

void test_atomic_profile_save_and_revision_contract() {
    Fixture fixture;
    fixture.repository.mutable_document().profiles.emplace(
        "first", complete_profile("/first"));
    fixture.repository.mutable_document().profiles.emplace(
        "second", complete_profile("/second"));
    auto state = submit_and_wait(
        fixture.view_model, {ccs::MainWindowCommand::LoadDraft});
    require(state->draft.revision > 0
            && state->draft.base_revision.size() == 64
            && state->selected_profile_key,
        "loaded draft publishes opaque base and monotonic draft revisions");
    const auto first_key = *state->selected_profile_key;
    const auto original_revision = state->draft.revision;
    const auto original_base = state->draft.base_revision;

    ccs::MainWindowCommandRequest save;
    save.command = ccs::MainWindowCommand::SaveProfile;
    save.profile_id = "first";
    save.profile_key = first_key;
    save.expected_draft_revision = original_revision;
    save.expected_base_revision = original_base;
    save.field_edits = {
        {"id", ccs::ConfigurationFieldValue{std::string{"renamed"}}},
        {"upstream.base-url",
            ccs::ConfigurationFieldValue{std::string{"https://saved.example.test/v1"}}},
    };
    state = submit_and_wait(fixture.view_model, std::move(save));
    const auto* saved_url = find_field(
        state->profile_editor->fields, "upstream.base-url");
    require(state->last_command->succeeded()
            && state->last_command->profile_id == "renamed"
            && state->selected_profile_key == first_key
            && state->selected_profile_id == "renamed"
            && state->draft.revision == original_revision + 1
            && state->draft.base_revision == original_base
            && saved_url && saved_url->value
            && std::get<std::string>(*saved_url->value)
                == "https://saved.example.test/v1",
        "Save atomically updates Profile id and fields while preserving stable identity");

    ccs::MainWindowCommandRequest stale_save;
    stale_save.command = ccs::MainWindowCommand::SaveProfile;
    stale_save.profile_id = "renamed";
    stale_save.profile_key = first_key;
    stale_save.expected_draft_revision = original_revision;
    stale_save.expected_base_revision = original_base;
    stale_save.field_edits = {{
        "id", ccs::ConfigurationFieldValue{std::string{"stale-overwrite"}},
    }};
    state = submit_and_wait(fixture.view_model, std::move(stale_save));
    require(state->last_command->error == ccs::MainWindowError::DraftStale
            && state->last_command->recovery_command
                == ccs::MainWindowCommand::ReloadDraft
            && state->selected_profile_id == "renamed"
            && state->draft.revision == original_revision + 1,
        "stale GUI Save cannot overwrite a newer tray draft");

    const auto before_failed_save = *state;
    ccs::MainWindowCommandRequest duplicate_save;
    duplicate_save.command = ccs::MainWindowCommand::SaveProfile;
    duplicate_save.profile_id = "renamed";
    duplicate_save.profile_key = first_key;
    duplicate_save.expected_draft_revision = state->draft.revision;
    duplicate_save.expected_base_revision = state->draft.base_revision;
    duplicate_save.field_edits = {
        {"id", ccs::ConfigurationFieldValue{std::string{"second"}}},
        {"upstream.base-url",
            ccs::ConfigurationFieldValue{std::string{"https://must-not-stick.test/v1"}}},
    };
    state = submit_and_wait(fixture.view_model, std::move(duplicate_save));
    saved_url = find_field(state->profile_editor->fields, "upstream.base-url");
    require(state->last_command->error == ccs::MainWindowError::ProfileAlreadyExists
            && state->last_command->field == "id"
            && state->selected_profile_id == "renamed"
            && state->draft.revision == before_failed_save.draft.revision
            && saved_url && saved_url->value
            && std::get<std::string>(*saved_url->value)
                == "https://saved.example.test/v1",
        "failed Save rolls back every field and preserves selection and revision");

    ccs::MainWindowCommandRequest wrong_base;
    wrong_base.command = ccs::MainWindowCommand::SetProfileEnabled;
    wrong_base.profile_id = "renamed";
    wrong_base.profile_key = first_key;
    wrong_base.enabled = false;
    wrong_base.expected_draft_revision = state->draft.revision;
    wrong_base.expected_base_revision = std::string(64, '0');
    state = submit_and_wait(fixture.view_model, std::move(wrong_base));
    require(state->last_command->error == ccs::MainWindowError::RepositoryStale
            && ccs::find_profile_list_item(*state, first_key)->enabled,
        "base revision mismatch rejects mutation without changing the draft");
}

void test_field_specific_save_validation_and_rollback() {
    Fixture fixture;
    fixture.repository.mutable_document().profiles.emplace(
        "primary", complete_profile("/primary"));
    auto state = submit_and_wait(
        fixture.view_model, {ccs::MainWindowCommand::LoadDraft});
    require(state->selected_profile_key && state->profile_editor,
        "validation fixture has a selected Profile editor");
    const auto profile_key = *state->selected_profile_key;
    const auto baseline_draft_revision = state->draft.revision;
    const auto baseline_base_revision = state->draft.base_revision;

    const auto failed_profile_save = [&](
                                         std::string field,
                                         ccs::ConfigurationFieldValue value,
                                         std::string expected_field) {
        const auto edited_field = field;
        const auto* before_field = find_field(state->profile_editor->fields, field);
        require(before_field != nullptr, "Profile field exists before failed Save");
        const auto before_value = before_field->value;

        ccs::MainWindowCommandRequest request;
        request.command = ccs::MainWindowCommand::SaveProfile;
        request.profile_id = "primary";
        request.profile_key = profile_key;
        request.expected_draft_revision = baseline_draft_revision;
        request.expected_base_revision = baseline_base_revision;
        request.field_edits = {{std::move(field), std::move(value)}};
        state = submit_and_wait(fixture.view_model, std::move(request));

        require(state->last_command->outcome == ccs::CommandOutcome::Rejected
                && state->last_command->error == ccs::MainWindowError::ValidationFailed
                && state->last_command->field == expected_field
                && state->draft.revision == baseline_draft_revision
                && state->draft.base_revision == baseline_base_revision
                && state->selected_profile_key == profile_key
                && state->selected_profile_id == "primary",
            "failed Profile Save reports its exact field without changing draft identity");
        const auto* rolled_back = find_field(
            state->profile_editor->fields, edited_field);
        require(rolled_back != nullptr && rolled_back->value == before_value,
            "failed Profile Save preserves the previously published field value");
    };

    failed_profile_save(
        "local.request-path",
        std::string{"relative/responses"},
        "local.request-path");
    failed_profile_save(
        "upstream.base-url",
        std::string{"ftp://example.test/v1"},
        "upstream.base-url");
    failed_profile_save(
        "upstream.request-path",
        std::string{"relative/responses"},
        "upstream.request-path");
    failed_profile_save(
        "upstream.usage-path",
        std::string{"relative/usage"},
        "upstream.usage-path");
    failed_profile_save(
        "local.usage-path",
        std::string{"/primary/v1/usage"},
        "upstream.usage-path");

    ccs::MainWindowCommandRequest settings;
    settings.command = ccs::MainWindowCommand::UpdateApplicationFields;
    settings.expected_draft_revision = baseline_draft_revision;
    settings.expected_base_revision = baseline_base_revision;
    settings.field_edits = {{
        "runtime.max-connections",
        ccs::ConfigurationFieldValue{static_cast<std::uint64_t>(16)},
    }};
    state = submit_and_wait(fixture.view_model, std::move(settings));
    const auto* max_connections = find_field(
        state->application_fields, "runtime.max-connections");
    require(state->last_command->outcome == ccs::CommandOutcome::Rejected
            && state->last_command->error == ccs::MainWindowError::ValidationFailed
            && state->last_command->field == "runtime.max-connections"
            && state->draft.revision == baseline_draft_revision
            && max_connections && max_connections->value
            && std::get<std::uint64_t>(*max_connections->value) == 64,
        "cross-field Settings validation reports max-connections and rolls back");
}

} // namespace

int main() {
    try {
        test_load_profile_commands_and_stopped_apply();
        test_stale_save_and_runtime_reload_recovery();
        test_reload_selection_and_profile_validation_details();
        test_duplicate_command_and_callback_invalidation();
        test_preference_failure_preserves_published_value();
        test_route_collision_is_classified_without_mutating_draft();
        test_noop_profile_mutations_preserve_clean_draft();
        test_typed_fields_and_rules_text_workflow();
        test_atomic_profile_save_and_revision_contract();
        test_field_specific_save_validation_and_rollback();
        std::cout << "main window view model tests ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "main window view model tests failed: " << ex.what() << "\n";
        return 1;
    }
}
