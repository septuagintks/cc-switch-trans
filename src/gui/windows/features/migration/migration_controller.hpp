#pragma once

#include <QObject>

namespace ccs_trans::gui {

class MigrationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state CONSTANT)
    Q_PROPERTY(bool actionAvailable READ actionAvailable CONSTANT)

public:
    explicit MigrationController(QObject* parent = nullptr);

    [[nodiscard]] QString state() const;
    [[nodiscard]] bool actionAvailable() const noexcept;
};

} // namespace ccs_trans::gui
