#include "features/migration/migration_controller.hpp"

namespace ccs_trans::gui {

MigrationController::MigrationController(QObject* parent) : QObject(parent) {}

QString MigrationController::state() const {
    return QStringLiteral("Unavailable");
}

bool MigrationController::actionAvailable() const noexcept { return false; }

} // namespace ccs_trans::gui
