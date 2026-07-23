#include "features/settings/settings_controller.hpp"

#include "controllers/command_dispatcher.hpp"
#include "state/gui_state_store.hpp"

#include <utility>

namespace ccs_trans::gui {

SettingsController::SettingsController(
    GuiStateStore& state,
    CommandDispatcher& commands,
    QObject* parent)
    : QObject(parent), state_(state), commands_(commands) {
    connect(&state_, &GuiStateStore::preferenceChanged,
            this, &SettingsController::lightweightModeChanged);
    connect(&state_, &GuiStateStore::snapshotApplied,
            this, &SettingsController::syncFromState);
    connect(&fields_, &EditableFieldModel::dirtyChanged,
            this, &SettingsController::draftChanged);
    connect(&fields_, &EditableFieldModel::validityChanged,
            this, &SettingsController::draftChanged);
    connect(&commands_, &CommandDispatcher::commandFinished,
            this, &SettingsController::handleCommandFinished);
}

bool SettingsController::lightweightMode() const noexcept {
    return state_.lightweightMode();
}

bool SettingsController::dirty() const noexcept { return fields_.dirty(); }
bool SettingsController::valid() const noexcept { return fields_.valid(); }
QObject* SettingsController::fieldsModel() noexcept { return &fields_; }

void SettingsController::setLightweightMode(bool enabled) {
    if (enabled == state_.lightweightMode()) return;
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::SetLightweightMode;
    request.enabled = enabled;
    (void)commands_.submit(std::move(request));
}

bool SettingsController::setFieldValue(
    const QString& key,
    const QVariant& value) {
    return fields_.setValue(key, value);
}

bool SettingsController::resetFieldValue(const QString& key) {
    return fields_.resetValue(key);
}

bool SettingsController::ownsField(const QString& key) const {
    return fields_.contains(key);
}

void SettingsController::save() {
    auto edits = fields_.edits();
    if (edits.empty() || !fields_.valid()) return;
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::UpdateApplicationFields;
    request.field_edits = std::move(edits);
    const auto submitted_revision = state_.draftRevision();
    const auto submitted_local_revision = fields_.localRevision();
    if (commands_.submit(std::move(request))) {
        submitted_draft_revision_ = submitted_revision;
        submitted_local_revision_ = submitted_local_revision;
    }
}

void SettingsController::resetLocalDraft() {
    fields_.discardLocal();
    syncFromState();
}

void SettingsController::syncFromState() {
    if (awaiting_server_snapshot_
        && state_.draftRevision() <= submitted_draft_revision_) {
        return;
    }
    if (fields_.dirty() && !awaiting_server_snapshot_) return;
    const bool accept_server = awaiting_server_snapshot_;
    if (accept_server) {
        awaiting_server_snapshot_ = false;
    }
    const auto* snapshot = state_.snapshot() ? &*state_.snapshot() : nullptr;
    if (snapshot == nullptr) {
        fields_.clear();
        return;
    }
    if (accept_server) {
        fields_.apply(snapshot->application_fields,
            false, submitted_local_revision_);
    } else {
        fields_.apply(snapshot->application_fields);
    }
}

void SettingsController::handleCommandFinished(
    const QString& command,
    const QString& outcome,
    const QString& errorCode,
    const QString& field,
    const QString& detail) {
    if (command != QStringLiteral("update_application_fields")) return;
    if (errorCode == QStringLiteral("none")
        && (outcome == QStringLiteral("succeeded")
            || outcome == QStringLiteral("saved_pending_runtime_apply"))) {
        awaiting_server_snapshot_ = true;
        fields_.clearFieldErrors();
        syncFromState();
    } else if (!field.isEmpty()) {
        fields_.setFieldError(field, detail);
    }
}

} // namespace ccs_trans::gui
