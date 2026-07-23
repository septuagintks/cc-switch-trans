#include "features/profiles/profiles_controller.hpp"

#include "controllers/command_dispatcher.hpp"
#include "models/editable_field_model.hpp"
#include "state/gui_state_store.hpp"

#include <utility>

namespace ccs_trans::gui {

ProfilesController::ProfilesController(
    GuiStateStore& state,
    CommandDispatcher& commands,
    QObject* parent)
    : QObject(parent), state_(state), commands_(commands) {
    connect(&state_, &GuiStateStore::snapshotApplied,
            this, &ProfilesController::syncFromState);
    connect(&fields_, &EditableFieldModel::dirtyChanged,
            this, &ProfilesController::draftChanged);
    connect(&fields_, &EditableFieldModel::validityChanged,
            this, &ProfilesController::draftChanged);
    connect(&fields_, &EditableFieldModel::fieldChanged,
            this, &ProfilesController::handleFieldChanged);
    connect(&commands_, &CommandDispatcher::commandFinished,
            this, &ProfilesController::handleCommandFinished);
}

QString ProfilesController::profileKey() const { return profile_key_; }
QString ProfilesController::profileId() const { return profile_id_; }
bool ProfilesController::enabled() const noexcept { return enabled_; }
bool ProfilesController::dirty() const noexcept { return fields_.dirty(); }
bool ProfilesController::valid() const noexcept { return fields_.valid(); }
QObject* ProfilesController::fieldsModel() noexcept { return &fields_; }

void ProfilesController::setProfileId(const QString& profile_id) {
    (void)setFieldValue(QStringLiteral("id"), profile_id);
}

void ProfilesController::setEnabled(bool enabled) {
    (void)setFieldValue(QStringLiteral("enabled"), enabled);
}

void ProfilesController::selectProfile(const QString& stable_key) {
    bool parsed = false;
    const auto key = stable_key.toLongLong(&parsed);
    if (!parsed || key <= 0 || stable_key == profile_key_) return;
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::SelectProfile;
    request.profile_key = key;
    (void)commands_.submit(std::move(request));
}

void ProfilesController::save() {
    bool parsed = false;
    const auto key = profile_key_.toLongLong(&parsed);
    if (!parsed || key <= 0 || profile_id_.trimmed().isEmpty()
        || !fields_.valid()) return;
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::SaveProfile;
    request.profile_key = key;
    request.profile_id = profile_id_.toUtf8().toStdString();
    request.field_edits = fields_.edits();
    if (request.field_edits.empty()) return;
    const auto submitted_revision = state_.draftRevision();
    const auto submitted_local_revision = fields_.localRevision();
    if (commands_.submit(std::move(request))) {
        submitted_draft_revision_ = submitted_revision;
        submitted_local_revision_ = submitted_local_revision;
    }
}

void ProfilesController::createProfile(const QString& profile_id) {
    if (profile_id.trimmed().isEmpty()) return;
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::CreateProfile;
    request.profile_id = profile_id.trimmed().toUtf8().toStdString();
    (void)commands_.submit(std::move(request));
}

void ProfilesController::removeSelected() {
    bool parsed = false;
    const auto key = profile_key_.toLongLong(&parsed);
    if (!parsed || key <= 0) return;
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::RemoveProfile;
    request.profile_key = key;
    request.profile_id = state_.selectedProfileId().toUtf8().toStdString();
    (void)commands_.submit(std::move(request));
}

void ProfilesController::moveSelected(int position) {
    bool parsed = false;
    const auto key = profile_key_.toLongLong(&parsed);
    if (!parsed || key <= 0 || position < 0) return;
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::MoveProfile;
    request.profile_key = key;
    request.profile_id = state_.selectedProfileId().toUtf8().toStdString();
    // The QML list uses a zero-based target index; the shared editor contract
    // intentionally remains one-based for CLI and presentation callers.
    request.position = static_cast<std::size_t>(position) + 1;
    (void)commands_.submit(std::move(request));
}

bool ProfilesController::setFieldValue(
    const QString& key,
    const QVariant& value) {
    return fields_.setValue(key, value);
}

bool ProfilesController::resetFieldValue(const QString& key) {
    return fields_.resetValue(key);
}

bool ProfilesController::ownsField(const QString& key) const {
    return fields_.contains(key);
}

void ProfilesController::resetLocalDraft() {
    fields_.discardLocal();
    syncFromState();
}

void ProfilesController::syncFromState() {
    const auto selected_key = state_.selectedProfileKey();
    const bool selection_changed = selected_key != profile_key_;
    if (!selection_changed && awaiting_server_snapshot_
        && state_.draftRevision() <= submitted_draft_revision_) {
        return;
    }
    if (!selection_changed && fields_.dirty() && !awaiting_server_snapshot_) return;
    const bool accept_server = awaiting_server_snapshot_ && !selection_changed;
    if (selection_changed || accept_server) {
        awaiting_server_snapshot_ = false;
    }
    const auto selected_id = state_.selectedProfileId();
    const bool selected_enabled = state_.selectedProfileEnabled();
    if (state_.snapshot() && state_.snapshot()->profile_editor) {
        if (accept_server) {
            fields_.apply(state_.snapshot()->profile_editor->fields,
                false, submitted_local_revision_);
        } else {
            fields_.apply(state_.snapshot()->profile_editor->fields,
                !selection_changed);
        }
    } else {
        fields_.clear();
    }
    if (profile_key_ == selected_key && profile_id_ == selected_id
        && enabled_ == selected_enabled && !fields_.dirty()) {
        return;
    }
    profile_key_ = selected_key;
    profile_id_ = selected_id;
    enabled_ = selected_enabled;
    if (const auto value = fields_.value(QStringLiteral("id")); value.isValid()) {
        profile_id_ = value.toString();
    }
    if (const auto value = fields_.value(QStringLiteral("enabled")); value.isValid()) {
        enabled_ = value.toBool();
    }
    emit draftChanged();
}

void ProfilesController::handleFieldChanged(const QString& key) {
    if (key == QStringLiteral("id")) {
        profile_id_ = fields_.textValue(key);
    } else if (key == QStringLiteral("enabled")) {
        enabled_ = fields_.value(key).toBool();
    }
    emit draftChanged();
}

void ProfilesController::handleCommandFinished(
    const QString& command,
    const QString& outcome,
    const QString& errorCode,
    const QString& field,
    const QString& detail) {
    if (command != QStringLiteral("save_profile")) return;
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
