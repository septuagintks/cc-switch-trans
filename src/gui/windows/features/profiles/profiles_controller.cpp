#include "features/profiles/profiles_controller.hpp"

#include "controllers/command_dispatcher.hpp"
#include "state/gui_state_store.hpp"

namespace ccs_trans::gui {

ProfilesController::ProfilesController(
    GuiStateStore& state,
    CommandDispatcher& commands,
    QObject* parent)
    : QObject(parent), state_(state), commands_(commands) {
    connect(&state_, &GuiStateStore::snapshotApplied,
            this, &ProfilesController::syncFromState);
    connect(&commands_, &CommandDispatcher::commandFinished,
            this, &ProfilesController::handleCommandFinished);
}

QString ProfilesController::profileKey() const { return profile_key_; }
QString ProfilesController::profileId() const { return profile_id_; }
bool ProfilesController::enabled() const noexcept { return enabled_; }
bool ProfilesController::dirty() const noexcept { return dirty_; }

void ProfilesController::setProfileId(const QString& profile_id) {
    if (profile_id_ == profile_id) return;
    profile_id_ = profile_id;
    dirty_ = true;
    emit draftChanged();
}

void ProfilesController::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    dirty_ = true;
    emit draftChanged();
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
    if (!parsed || key <= 0 || profile_id_.trimmed().isEmpty()) return;
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::SaveProfile;
    request.profile_key = key;
    request.profile_id = state_.selectedProfileId().toUtf8().toStdString();
    request.field_edits = {
        {"id", ccs::gui_ipc::FieldValue{
            profile_id_.trimmed().toUtf8().toStdString()}},
        {"enabled", ccs::gui_ipc::FieldValue{enabled_}},
    };
    (void)commands_.submit(std::move(request));
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

void ProfilesController::resetLocalDraft() {
    dirty_ = false;
    syncFromState();
}

void ProfilesController::syncFromState() {
    const auto selected_key = state_.selectedProfileKey();
    const bool selection_changed = selected_key != profile_key_;
    if (!selection_changed && dirty_) return;
    const auto selected_id = state_.selectedProfileId();
    const bool selected_enabled = state_.selectedProfileEnabled();
    if (profile_key_ == selected_key && profile_id_ == selected_id
        && enabled_ == selected_enabled && !dirty_) {
        return;
    }
    profile_key_ = selected_key;
    profile_id_ = selected_id;
    enabled_ = selected_enabled;
    dirty_ = false;
    emit draftChanged();
}

void ProfilesController::handleCommandFinished(
    const QString& command,
    const QString& outcome,
    const QString& errorCode,
    const QString&,
    const QString&) {
    if (command != QStringLiteral("save_profile")) return;
    if (errorCode == QStringLiteral("none")
        && (outcome == QStringLiteral("succeeded")
            || outcome == QStringLiteral("saved_pending_runtime_apply"))) {
        dirty_ = false;
        emit draftChanged();
    }
}

} // namespace ccs_trans::gui
