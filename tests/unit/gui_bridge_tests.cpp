#include "hosts/windows/gui_bridge/gui_command_router.hpp"
#include "hosts/windows/gui_bridge/gui_snapshot_builder.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

ccs::MainWindowState sample_state() {
    ccs::MainWindowState state;
    state.revision = 10;
    state.application = {
        ccs::ApplicationState::Running, "127.0.0.1", 15723, {}, 0};
    state.profiles.push_back({
        5, "findcg", true, std::string{"responses"},
        ccs::ProfileReadiness::Ready, {}, 2, 1});
    state.application_fields.push_back({
        "listener.port",
        ccs::ConfigurationFieldScope::Application,
        ccs::ConfigurationFieldInputKind::UnsignedInteger,
        true,
        1,
        65535,
        {},
        "field.listener.port",
        ccs::RuntimeApplyImpact::ServiceRestart,
        ccs::ConfigurationFieldValue{std::uint64_t{15723}},
    });
    state.selected_profile_id = "findcg";
    state.selected_profile_key = 5;
    state.profile_editor = ccs::ProfileEditorState{
        5,
        "findcg",
        {ccs::ConfigurationFieldState{
            "enabled",
            ccs::ConfigurationFieldScope::Profile,
            ccs::ConfigurationFieldInputKind::Boolean,
            true,
            {},
            {},
            {},
            "field.profile.enabled",
            ccs::RuntimeApplyImpact::RuntimeReload,
            ccs::ConfigurationFieldValue{true},
        }},
    };
    state.rules_editor = ccs::RulesEditorState{
        5, "findcg", "rules", ccs::RulesTextError{"bad", 2, 4, "r", "t", "o"}};
    state.draft = {ccs::DraftPhase::Dirty, false, 3, "opaque-revision"};
    state.last_command = ccs::CommandResult{
        8,
        ccs::MainWindowCommand::SaveProfile,
        ccs::CommandOutcome::Rejected,
        ccs::MainWindowError::ValidationFailed,
        "findcg",
        "upstream.base-url",
        "invalid URL",
        ccs::MainWindowCommand::ReloadDraft,
    };
    state.lightweight_mode = false;
    state.command_pending = true;
    return state;
}

void test_snapshot_and_delta_mapping() {
    const auto state = sample_state();
    const auto snapshot = ccs::build_gui_snapshot(state);
    require(snapshot.revision == 10
            && snapshot.application.state == "running"
            && snapshot.profiles.size() == 1
            && snapshot.profiles[0].key == 5
            && snapshot.selection.profile_key == 5
            && snapshot.profile_editor
            && snapshot.rules_editor
            && snapshot.rules_editor->diagnostic
            && snapshot.last_command
            && snapshot.last_command->sequence == 8
            && snapshot.last_command->error
                == ccs::gui_ipc::ErrorCode::ValidationFailed,
        "MainWindowState snapshot mapping lost typed fields");

    auto changed_state = state;
    changed_state.revision = 13;
    changed_state.application.state = ccs::ApplicationState::Reloading;
    changed_state.command_pending = false;
    changed_state.rules_editor.reset();
    const auto changed = ccs::build_gui_snapshot(changed_state);
    const auto delta = ccs::build_gui_state_delta(snapshot, changed);
    require(delta.from_revision == 10 && delta.revision == 13
            && delta.application
            && !delta.profiles
            && delta.rules_editor_changed
            && !delta.rules_editor
            && delta.command_pending == false,
        "state delta did not isolate changed sections");

    auto revision_only = changed;
    ++revision_only.revision;
    require(ccs::build_gui_state_delta(changed, revision_only).empty(),
        "revision-only status refresh produced a GUI rebuild delta");
}

void test_command_mapping() {
    ccs::gui_ipc::Command source;
    source.command = ccs::gui_ipc::GuiCommand::SaveProfile;
    source.profile_id = "findcg";
    source.replacement_profile_id = "renamed";
    source.profile_key = 5;
    source.enabled = true;
    source.position = 2;
    source.field_edits = {
        {"id", ccs::gui_ipc::FieldValue{std::string{"renamed"}}},
        {"enabled", ccs::gui_ipc::FieldValue{true}},
        {"upstream.usage-path", std::nullopt},
    };
    source.expected_draft_revision = 4;
    source.expected_base_revision = "opaque";
    source.unsaved_decision = ccs::gui_ipc::UnsavedDecision::Discard;

    ccs::MainWindowCommandRequest mapped;
    ccs::gui_ipc::ErrorCode error_code;
    std::string error;
    require(ccs::translate_gui_command(source, mapped, error_code, error), error);
    require(mapped.command == ccs::MainWindowCommand::SaveProfile
            && mapped.source == ccs::MainWindowCommandSource::GuiIpc
            && mapped.profile_key == 5
            && mapped.field_edits.size() == 3
            && !mapped.field_edits[2].value
            && mapped.expected_draft_revision == 4
            && mapped.expected_base_revision == "opaque"
            && mapped.unsaved_changes_decision
                == ccs::UnsavedChangesDecision::Discard,
        "GUI Save command mapping lost atomic edit fields");

    source.command = ccs::gui_ipc::GuiCommand::AddRule;
    require(!ccs::translate_gui_command(source, mapped, error_code, error)
            && error_code == ccs::gui_ipc::ErrorCode::InvalidArgument,
        "future visual Rule command was silently routed through the old editor");
}

} // namespace

int main() {
    try {
        test_snapshot_and_delta_mapping();
        test_command_mapping();
        std::cout << "GUI bridge tests ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "GUI bridge tests failed: " << exception.what() << '\n';
        return 1;
    }
}
