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
QString RulesController::error() const { return error_; }

void RulesController::setText(const QString& text) {
    QString canonical = text;
    canonical.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    canonical.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    canonical.replace(QStringLiteral("\u2028"), QStringLiteral("\n"));
    canonical.replace(QStringLiteral("\u2029"), QStringLiteral("\n"));
    if (text_ == canonical) return;
    text_ = std::move(canonical);
    ++edit_revision_;
    if (!error_.isEmpty()) {
        error_.clear();
        emit errorChanged();
    }
    dirty_ = text_ != server_text_;
    emit draftChanged();
    if (text_ != text) emit textReplaced(text_);
}

void RulesController::save() { submit(false); }
void RulesController::format() { submit(true); }

void RulesController::resetLocalDraft() {
    ++edit_revision_;
    text_ = server_text_;
    dirty_ = false;
    if (!error_.isEmpty()) {
        error_.clear();
        emit errorChanged();
    }
    emit draftChanged();
    emit textReplaced(text_);
}

void RulesController::syncFromState() {
    const auto selected_key = state_.selectedProfileKey();
    const bool selection_changed = selected_key != profile_key_;
    if (!selection_changed && awaiting_server_snapshot_
        && state_.draftRevision() <= submitted_draft_revision_) {
        return;
    }
    if (!selection_changed && dirty_ && !awaiting_server_snapshot_) return;
    const auto next_profile = state_.selectedProfileId();
    const auto next_text = state_.rulesText();
    const bool accept_server = awaiting_server_snapshot_ && !selection_changed;
    const bool preserve_local = accept_server
        && edit_revision_ > submitted_edit_revision_;
    if (selection_changed || accept_server) {
        awaiting_server_snapshot_ = false;
    }
    if (preserve_local) {
        server_text_ = next_text;
        dirty_ = text_ != server_text_;
        if (!error_.isEmpty()) {
            error_.clear();
            emit errorChanged();
        }
        emit draftChanged();
        return;
    }
    if (profile_key_ == selected_key && profile_id_ == next_profile
        && server_text_ == next_text && text_ == next_text && !dirty_) {
        return;
    }
    profile_key_ = selected_key;
    profile_id_ = next_profile;
    server_text_ = next_text;
    text_ = next_text;
    dirty_ = false;
    if (!error_.isEmpty()) {
        error_.clear();
        emit errorChanged();
    }
    emit draftChanged();
    emit textReplaced(text_);
}

void RulesController::handleCommandFinished(
    const QString& command,
    const QString& outcome,
    const QString& errorCode,
    const QString&,
    const QString& detail) {
    if (command != QStringLiteral("replace_rules_text")
        && command != QStringLiteral("format_rules_text")) {
        return;
    }
    if (errorCode == QStringLiteral("none")
        && (outcome == QStringLiteral("succeeded")
            || outcome == QStringLiteral("saved_pending_runtime_apply"))) {
        awaiting_server_snapshot_ = true;
        if (!error_.isEmpty()) {
            error_.clear();
            emit errorChanged();
        }
        syncFromState();
    } else if (!errorCode.isEmpty() && errorCode != QStringLiteral("none")) {
        const auto next = detail;
        if (error_ != next) {
            error_ = next;
            emit errorChanged();
        }
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
    const auto submitted_revision = state_.draftRevision();
    const auto submitted_edit_revision = edit_revision_;
    if (commands_.submit(std::move(request))) {
        submitted_draft_revision_ = submitted_revision;
        submitted_edit_revision_ = submitted_edit_revision;
    }
}

} // namespace ccs_trans::gui
