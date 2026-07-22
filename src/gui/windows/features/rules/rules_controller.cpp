#include "features/rules/rules_controller.hpp"

#include "controllers/command_dispatcher.hpp"
#include "state/gui_state_store.hpp"

#include <utility>

namespace ccs_trans::gui {

RulesController::RulesController(
    GuiStateStore& state,
    CommandDispatcher& commands,
    QObject* parent)
    : QObject(parent), state_(state), commands_(commands) {
    connect(&state_, &GuiStateStore::snapshotApplied,
            this, &RulesController::syncFromState);
    connect(&state_, &GuiStateStore::editorChanged,
            this, &RulesController::diagnosticChanged);
    connect(&commands_, &CommandDispatcher::commandFinished,
            this, &RulesController::handleCommandFinished);
}

QString RulesController::profileId() const { return profile_id_; }
QString RulesController::text() const { return text_; }
bool RulesController::dirty() const noexcept { return dirty_; }
QString RulesController::diagnostic() const { return state_.rulesDiagnostic(); }

void RulesController::setText(const QString& text) {
    QString canonical = text;
    canonical.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    canonical.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (text_ == canonical) return;
    text_ = std::move(canonical);
    dirty_ = true;
    emit draftChanged();
}

void RulesController::save() { submit(false); }
void RulesController::format() { submit(true); }

void RulesController::resetLocalDraft() {
    dirty_ = false;
    syncFromState();
}

void RulesController::syncFromState() {
    const auto selected_key = state_.selectedProfileKey();
    const bool selection_changed = selected_key != profile_key_;
    if (!selection_changed && dirty_) return;
    const auto next_profile = state_.selectedProfileId();
    const auto next_text = state_.rulesText();
    if (profile_key_ == selected_key && profile_id_ == next_profile
        && text_ == next_text && !dirty_) {
        return;
    }
    profile_key_ = selected_key;
    profile_id_ = next_profile;
    text_ = next_text;
    dirty_ = false;
    emit draftChanged();
}

void RulesController::handleCommandFinished(
    const QString& command,
    const QString& outcome,
    const QString& errorCode,
    const QString&,
    const QString&) {
    if (command != QStringLiteral("replace_rules_text")
        && command != QStringLiteral("format_rules_text")) {
        return;
    }
    if (errorCode == QStringLiteral("none")
        && (outcome == QStringLiteral("succeeded")
            || outcome == QStringLiteral("saved_pending_runtime_apply"))) {
        dirty_ = false;
        emit draftChanged();
    }
}

void RulesController::submit(bool format_request) {
    bool parsed = false;
    const auto key = profile_key_.toLongLong(&parsed);
    if (!parsed || key <= 0) return;
    ccs::gui_ipc::Command request;
    request.command = format_request
        ? ccs::gui_ipc::GuiCommand::FormatRulesText
        : ccs::gui_ipc::GuiCommand::ReplaceRulesText;
    request.profile_key = key;
    request.profile_id = profile_id_.toUtf8().toStdString();
    request.text = text_.toUtf8().toStdString();
    (void)commands_.submit(std::move(request));
}

} // namespace ccs_trans::gui
