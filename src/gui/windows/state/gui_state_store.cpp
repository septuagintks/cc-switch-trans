#include "state/gui_state_store.hpp"

#include "ipc/gui_ipc_client.hpp"

#include <algorithm>

namespace ccs_trans::gui {

GuiStateStore::GuiStateStore(GuiIpcClient& client, QObject* parent)
    : QObject(parent)
    , client_(client)
    , profiles_(this)
    , profile_fields_(this)
    , application_fields_(this) {
    connect(&client_, &GuiIpcClient::snapshotAvailable,
            this, &GuiStateStore::applyClientSnapshot);
    connect(&client_, &GuiIpcClient::commandStatusAvailable,
            this, &GuiStateStore::applyCommandStatus);
}

QObject* GuiStateStore::profilesModel() noexcept { return &profiles_; }
QObject* GuiStateStore::profileFieldsModel() noexcept { return &profile_fields_; }
QObject* GuiStateStore::applicationFieldsModel() noexcept { return &application_fields_; }
qulonglong GuiStateStore::revision() const noexcept {
    return snapshot_ ? snapshot_->revision : 0;
}

QString GuiStateStore::applicationState() const {
    return snapshot_ ? text(snapshot_->application.state) : QStringLiteral("connecting");
}

QString GuiStateStore::listenerAddress() const {
    if (!snapshot_) return {};
    return text(snapshot_->application.listener_host) + QStringLiteral(":")
        + QString::number(snapshot_->application.listener_port);
}

QString GuiStateStore::applicationError() const {
    return snapshot_ ? text(snapshot_->application.last_error) : QString{};
}

bool GuiStateStore::canStart() const {
    const auto state = applicationState();
    return !commandPending()
        && (state == QStringLiteral("stopped") || state == QStringLiteral("faulted"));
}

bool GuiStateStore::canStop() const {
    return !commandPending() && applicationState() == QStringLiteral("running");
}

bool GuiStateStore::canReload() const { return canStop(); }

QString GuiStateStore::selectedProfileKey() const {
    return snapshot_ && snapshot_->selection.profile_key
        ? QString::number(*snapshot_->selection.profile_key) : QString{};
}

QString GuiStateStore::selectedProfileId() const {
    return snapshot_ && snapshot_->selection.profile_id
        ? text(*snapshot_->selection.profile_id) : QString{};
}

bool GuiStateStore::selectedProfileEnabled() const {
    const auto* profile = selectedProfile();
    return profile != nullptr && profile->enabled;
}

QString GuiStateStore::rulesText() const {
    return snapshot_ && snapshot_->rules_editor
        ? text(snapshot_->rules_editor->text) : QString{};
}

QString GuiStateStore::rulesDiagnostic() const {
    if (!snapshot_ || !snapshot_->rules_editor
        || !snapshot_->rules_editor->diagnostic) {
        return {};
    }
    const auto& diagnostic = *snapshot_->rules_editor->diagnostic;
    return QStringLiteral("%1:%2  %3")
        .arg(diagnostic.line)
        .arg(diagnostic.column)
        .arg(text(diagnostic.message));
}

QString GuiStateStore::draftPhase() const {
    return snapshot_ ? text(snapshot_->draft.phase) : QStringLiteral("unloaded");
}

qulonglong GuiStateStore::draftRevision() const noexcept {
    return snapshot_ ? snapshot_->draft.revision : 0;
}

QString GuiStateStore::baseRevision() const {
    return snapshot_ ? text(snapshot_->draft.base_revision) : QString{};
}

bool GuiStateStore::draftDirty() const {
    return draftPhase() == QStringLiteral("dirty")
        || draftPhase() == QStringLiteral("saved_pending_runtime_apply");
}

bool GuiStateStore::commandPending() const noexcept {
    return snapshot_ && snapshot_->command_pending;
}

QString GuiStateStore::lastCommand() const {
    return last_command_ ? text(last_command_->command) : QString{};
}

QString GuiStateStore::lastCommandOutcome() const {
    return last_command_
        ? QString::fromLatin1(ccs::gui_ipc::result_code_name(last_command_->outcome))
        : QString{};
}

QString GuiStateStore::lastCommandError() const {
    return last_command_
        ? QString::fromLatin1(ccs::gui_ipc::error_code_name(last_command_->error))
        : QString{};
}

QString GuiStateStore::lastCommandField() const {
    return last_command_ ? text(last_command_->field) : QString{};
}

QString GuiStateStore::lastCommandDetail() const {
    return last_command_ ? text(last_command_->detail) : QString{};
}

bool GuiStateStore::lightweightMode() const noexcept {
    return !snapshot_ || snapshot_->lightweight_mode;
}

const std::optional<ccs::gui_ipc::Snapshot>&
GuiStateStore::snapshot() const noexcept {
    return snapshot_;
}

void GuiStateStore::applyClientSnapshot() {
    if (!client_.snapshot()) return;
    auto previous = snapshot_;
    snapshot_ = *client_.snapshot();
    profiles_.apply(snapshot_->profiles);
    application_fields_.apply(snapshot_->application_fields);
    if (snapshot_->profile_editor) {
        profile_fields_.apply(snapshot_->profile_editor->fields);
    } else {
        profile_fields_.clear();
    }
    if (snapshot_->last_command) last_command_ = snapshot_->last_command;
    emitSnapshotDifferences(previous);
    emit snapshotApplied();
}

void GuiStateStore::applyCommandStatus() {
    const auto& event = client_.lastCommandEvent();
    if (!event) return;
    last_command_ = event->status;
    emit commandStateChanged();
}

QString GuiStateStore::text(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

const ccs::gui_ipc::ProfileSummary* GuiStateStore::selectedProfile() const noexcept {
    if (!snapshot_ || !snapshot_->selection.profile_key) return nullptr;
    const auto key = *snapshot_->selection.profile_key;
    const auto found = std::find_if(
        snapshot_->profiles.cbegin(), snapshot_->profiles.cend(), [&](const auto& profile) {
            return profile.key == key;
        });
    return found == snapshot_->profiles.cend() ? nullptr : &*found;
}

void GuiStateStore::emitSnapshotDifferences(
    const std::optional<ccs::gui_ipc::Snapshot>& previous) {
    if (!previous || previous->revision != snapshot_->revision) emit revisionChanged();
    if (!previous || previous->application != snapshot_->application) {
        emit applicationChanged();
    }
    if (!previous || previous->selection != snapshot_->selection
        || previous->profiles != snapshot_->profiles) {
        emit selectionChanged();
    }
    if (!previous || previous->profile_editor != snapshot_->profile_editor
        || previous->rules_editor != snapshot_->rules_editor) {
        emit editorChanged();
    }
    if (!previous || previous->draft != snapshot_->draft) emit draftChanged();
    if (!previous || previous->command_pending != snapshot_->command_pending
        || previous->last_command != snapshot_->last_command) {
        emit commandStateChanged();
    }
    if (!previous || previous->lightweight_mode != snapshot_->lightweight_mode) {
        emit preferenceChanged();
    }
}

} // namespace ccs_trans::gui
