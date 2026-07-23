#include "features/migration/migration_controller.hpp"

#include "controllers/command_dispatcher.hpp"
#include "state/gui_state_store.hpp"

#include <utility>

namespace ccs_trans::gui {

MigrationController::MigrationController(
    GuiStateStore& state,
    CommandDispatcher& commands,
    QObject* parent)
    : QObject(parent), state_(state), commands_(commands) {
    connect(&state_, &GuiStateStore::storageChanged,
            this, &MigrationController::handleStorageChanged);
    connect(&commands_, &CommandDispatcher::commandFinished,
            this, &MigrationController::handleCommandFinished);
    connect(&commands_, &CommandDispatcher::busyChanged,
            this, &MigrationController::stateChanged);
}

QString MigrationController::state() const { return state_.storageState(); }
QString MigrationController::detail() const { return state_.storageDetail(); }
QString MigrationController::databasePath() const {
    return state_.storageDatabasePath();
}
QString MigrationController::backupDirectory() const {
    return state_.storageBackupDirectory();
}
bool MigrationController::databaseExists() const noexcept {
    return state_.storageDatabaseExists();
}

bool MigrationController::actionAvailable() const noexcept {
    return state_.storageState() == QStringLiteral("migration_required")
        && !commands_.busy();
}

bool MigrationController::migrationConfirmationRequired() const noexcept {
    return migration_confirmation_required_;
}

bool MigrationController::replacementConfirmationRequired() const noexcept {
    return replacement_confirmation_required_;
}

void MigrationController::inspect() {
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::StorageStatus;
    (void)commands_.submit(std::move(request));
}

void MigrationController::requestMigration() {
    if (!actionAvailable() || migration_confirmation_required_
        || replacement_confirmation_required_) return;
    migration_confirmation_required_ = true;
    emit confirmationChanged();
}

void MigrationController::confirmMigration() {
    if (!migration_confirmation_required_) return;
    migration_confirmation_required_ = false;
    const bool replace = databaseExists();
    replacement_confirmation_required_ = replace;
    emit confirmationChanged();
    if (replace) return;
    submitMigration(false);
}

void MigrationController::cancelMigration() {
    if (!migration_confirmation_required_) return;
    migration_confirmation_required_ = false;
    emit confirmationChanged();
}

void MigrationController::confirmReplacement() {
    if (!replacement_confirmation_required_) return;
    replacement_confirmation_required_ = false;
    emit confirmationChanged();
    submitMigration(true);
}

void MigrationController::cancelReplacement() {
    if (!replacement_confirmation_required_) return;
    replacement_confirmation_required_ = false;
    emit confirmationChanged();
}

void MigrationController::handleStorageChanged() {
    bool confirmation_changed = false;
    if (state_.storageState() != QStringLiteral("migration_required")) {
        confirmation_changed = migration_confirmation_required_
            || replacement_confirmation_required_;
        migration_confirmation_required_ = false;
        replacement_confirmation_required_ = false;
    }
    emit stateChanged();
    if (confirmation_changed) emit confirmationChanged();
}

void MigrationController::handleCommandFinished(
    const QString& command,
    const QString& outcome,
    const QString& errorCode,
    const QString&,
    const QString&) {
    if (command != QStringLiteral("migrate_storage")) return;
    const bool replacement = errorCode
        == QStringLiteral("replacement_confirmation_required");
    if (replacement) migration_confirmation_required_ = false;
    if (replacement != replacement_confirmation_required_) {
        replacement_confirmation_required_ = replacement;
        emit confirmationChanged();
    }
    if (errorCode == QStringLiteral("none")
        && outcome == QStringLiteral("succeeded")) {
        emit stateChanged();
    }
}

void MigrationController::submitMigration(bool replace) {
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::MigrateStorage;
    request.replace_existing_storage = replace;
    if (replace) request.replacement_confirmation = "REPLACE";
    (void)commands_.submit(std::move(request));
}

} // namespace ccs_trans::gui
