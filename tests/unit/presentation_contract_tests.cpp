#include "config/app_paths.hpp"
#include "presentation/main_window_contract.hpp"
#include "presentation/ui_preferences.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_profile_list_and_service_contract() {
    ccs::MainWindowState state;
    state.profiles = {
        ccs::ProfileListItem{
            3, "zeta", false, std::nullopt, ccs::ProfileReadiness::Incomplete, {}},
        ccs::ProfileListItem{
            1, "alpha", true, std::string{"responses"}, ccs::ProfileReadiness::Ready, {}},
        ccs::ProfileListItem{
            2, "middle", false, std::string{"chat"}, ccs::ProfileReadiness::Incomplete, {}},
    };
    const auto original_profiles = state.profiles;
    ccs::sort_profile_list_items(state.profiles);
    require(
        state.profiles[0].id == "alpha"
            && state.profiles[1].id == "middle"
            && state.profiles[2].id == "zeta",
        "Profile list is ordered by stable id");
    require(ccs::select_profile(state, "middle"), "known Profile can be selected");
    require(state.selected_profile_id == "middle", "selection is stored as UI state");
    require(state.selected_profile_key == 2, "selection keeps the stable Profile key");
    require(!ccs::select_profile(state, "missing"), "unknown Profile selection is rejected");
    require(state.selected_profile_id == "middle", "failed selection preserves current selection");
    require(state.profiles[1].enabled == original_profiles[2].enabled,
        "selection does not mutate Profile enabled state");
    ccs::clear_profile_selection(state);
    require(!state.selected_profile_id, "Profile selection can be cleared");

    require(
        ccs::service_actions_for(ccs::ApplicationState::Stopped)
            == ccs::ServiceActionState{true, false, false},
        "stopped service exposes only Start");
    require(
        ccs::service_actions_for(ccs::ApplicationState::Running)
            == ccs::ServiceActionState{false, true, true},
        "running service exposes Stop and Reload");
    require(
        ccs::service_actions_for(ccs::ApplicationState::Faulted)
            == ccs::ServiceActionState{true, false, false},
        "faulted service remains recoverable through Start");
    require(
        ccs::service_actions_for(ccs::ApplicationState::Reloading)
            == ccs::ServiceActionState{},
        "transitional service state disables mutating actions");
}

void test_draft_and_close_contract() {
    ccs::DraftState draft;
    require(!draft.loaded() && !draft.dirty() && !draft.busy(),
        "default draft is unloaded and inactive");
    require(
        ccs::resolve_main_window_close(draft, true) == ccs::MainWindowCloseAction::Destroy,
        "lightweight mode destroys a clean window");
    require(
        ccs::resolve_main_window_close(draft, false) == ccs::MainWindowCloseAction::Hide,
        "normal mode keeps a clean hidden window cached");

    draft.phase = ccs::DraftPhase::Dirty;
    require(draft.loaded() && draft.dirty() && !draft.busy(), "dirty draft state is explicit");
    require(
        ccs::resolve_main_window_close(draft, true)
            == ccs::MainWindowCloseAction::PromptForUnsavedChanges,
        "dirty close requires an explicit decision");
    require(
        ccs::resolve_main_window_close(
            draft, true, ccs::UnsavedChangesDecision::Apply)
            == ccs::MainWindowCloseAction::ApplyThenDestroy,
        "lightweight Apply closes only after applying and destroys the window");
    require(
        ccs::resolve_main_window_close(
            draft, false, ccs::UnsavedChangesDecision::Apply)
            == ccs::MainWindowCloseAction::ApplyThenHide,
        "normal Apply closes only after applying and caches the window");
    require(
        ccs::resolve_main_window_close(
            draft, true, ccs::UnsavedChangesDecision::Discard)
            == ccs::MainWindowCloseAction::DiscardThenDestroy,
        "lightweight Discard retires the draft before destroying the window");
    require(
        ccs::resolve_main_window_close(
            draft, false, ccs::UnsavedChangesDecision::Discard)
            == ccs::MainWindowCloseAction::DiscardThenHide,
        "normal Discard retires the draft before caching the window");
    require(
        ccs::resolve_main_window_close(
            draft, true, ccs::UnsavedChangesDecision::Cancel)
            == ccs::MainWindowCloseAction::KeepOpen,
        "Cancel leaves a dirty window open");

    draft.phase = ccs::DraftPhase::Applying;
    require(draft.loaded() && draft.dirty() && draft.busy(),
        "applying draft remains dirty and busy until its result arrives");
    require(
        ccs::resolve_main_window_close(draft, true) == ccs::MainWindowCloseAction::KeepOpen,
        "close cannot race an in-flight draft command");

    draft.phase = ccs::DraftPhase::SavedPendingRuntimeApply;
    require(draft.loaded() && !draft.dirty() && !draft.busy(),
        "saved-but-not-applied state does not claim unsaved edits");
    require(
        ccs::resolve_main_window_close(draft, true) == ccs::MainWindowCloseAction::Destroy,
        "saved-but-not-applied state may close while preserving its explicit status");

    require(
        ccs::resolve_cached_main_window(draft, true, false, true)
            == ccs::CachedWindowAction::Destroy,
        "enabling lightweight mode destroys a clean hidden cached window");
    require(
        ccs::resolve_cached_main_window(draft, true, true, true)
            == ccs::CachedWindowAction::NoChange,
        "enabling lightweight mode does not destroy a visible window");
    draft.phase = ccs::DraftPhase::Dirty;
    require(
        ccs::resolve_cached_main_window(draft, true, false, true)
            == ccs::CachedWindowAction::PromptForUnsavedChanges,
        "a dirty hidden cache requires the same explicit decision before destruction");
}

void test_command_result_contract() {
    ccs::CommandResult success;
    success.command = ccs::MainWindowCommand::ApplyDraft;
    require(success.succeeded() && success.configuration_saved(),
        "successful Apply reports a saved configuration");

    ccs::CommandResult partial;
    partial.command = ccs::MainWindowCommand::ApplyDraft;
    partial.outcome = ccs::CommandOutcome::SavedPendingRuntimeApply;
    partial.error = ccs::MainWindowError::RuntimeApplyFailed;
    require(!partial.succeeded() && partial.configuration_saved(),
        "runtime reload failure distinguishes persisted from applied state");
    ccs::CommandResult service_success;
    service_success.command = ccs::MainWindowCommand::StartService;
    require(service_success.succeeded() && !service_success.configuration_saved(),
        "successful non-Apply commands do not claim configuration persistence");
    require(std::string(ccs::main_window_command_name(partial.command)) == "apply_draft"
            && std::string(ccs::main_window_error_name(partial.error)) == "runtime_apply_failed"
            && std::string(ccs::command_outcome_name(partial.outcome))
                == "saved_pending_runtime_apply",
        "command result exposes stable cross-platform names");
    require(std::string(ccs::main_window_error_name(
                ccs::MainWindowError::UnsavedChangesDecisionRequired))
            == "unsaved_changes_decision_required",
        "dirty draft decision requirement has a stable cross-platform name");
}

void test_ui_preference_schema() {
    const auto defaults = ccs::make_default_ui_preferences();
    require(defaults.lightweight_mode, "lightweight mode defaults to enabled");

    std::string serialized;
    std::string error;
    require(ccs::serialize_ui_preferences(defaults, serialized, error), error);
    require(serialized.find("ccs-trans.ui/v1") != std::string::npos
            && serialized.find("\"lightweight_mode\": true") != std::string::npos,
        "serialized preferences carry schema and lightweight mode");

    ccs::UiPreferences parsed;
    parsed.lightweight_mode = false;
    require(ccs::parse_ui_preferences(serialized, parsed, error), error);
    require(parsed == defaults, "UI preferences have a canonical round trip");

    const std::string disabled = R"json({
  "schema_version": "ccs-trans.ui/v1",
  "main_window": {"lightweight_mode": false}
})json";
    require(ccs::parse_ui_preferences(disabled, parsed, error), error);
    require(!parsed.lightweight_mode, "lightweight preference can be disabled");

    const auto before_failure = parsed;
    const std::string unknown = R"json({
  "schema_version": "ccs-trans.ui/v1",
  "main_window": {"lightweight_mode": true, "surprise": 1}
})json";
    require(!ccs::parse_ui_preferences(unknown, parsed, error)
            && error.find("unknown field") != std::string::npos,
        "unknown UI preference fields are rejected");
    require(parsed == before_failure, "failed preference parse leaves the target unchanged");

    const std::string duplicate = R"json({
  "schema_version": "ccs-trans.ui/v1",
  "main_window": {"lightweight_mode": true, "lightweight_mode": false}
})json";
    require(!ccs::parse_ui_preferences(duplicate, parsed, error)
            && error.find("duplicate") != std::string::npos,
        "duplicate UI preference keys are rejected");

    const std::string wrong_schema = R"json({
  "schema_version": "ccs-trans.ui/v2",
  "main_window": {"lightweight_mode": true}
})json";
    require(!ccs::parse_ui_preferences(wrong_schema, parsed, error)
            && error.find("schema_version") != std::string::npos,
        "unsupported UI preference schema is rejected");

    const std::string wrong_type = R"json({
  "schema_version": "ccs-trans.ui/v1",
  "main_window": {"lightweight_mode": "true"}
})json";
    require(!ccs::parse_ui_preferences(wrong_type, parsed, error)
            && error.find("JSON boolean") != std::string::npos,
        "incorrect UI preference types are rejected");
    require(!ccs::parse_ui_preferences("{", parsed, error)
            && error.find("failed to parse") != std::string::npos,
        "malformed UI preference JSON is rejected");

    std::string oversized(ccs::kMaxUiPreferencesBytes + 1, 'x');
    require(!ccs::parse_ui_preferences(oversized, parsed, error)
            && error.find("64 KiB") != std::string::npos,
        "UI preference input is bounded");
}

void test_ui_preference_path() {
    const auto root = std::filesystem::path("test-root");
    const auto paths = ccs::make_app_paths(root);
    require(paths.ui_preferences_file == root / "state" / "ui.json",
        "UI preferences use the independent state/ui.json path");
}

} // namespace

int main() {
    try {
        test_profile_list_and_service_contract();
        test_draft_and_close_contract();
        test_command_result_contract();
        test_ui_preference_schema();
        test_ui_preference_path();
        std::cout << "presentation contract tests ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "presentation contract tests failed: " << ex.what() << "\n";
        return 1;
    }
}
