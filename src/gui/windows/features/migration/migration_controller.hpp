#pragma once

#include <QObject>

namespace ccs_trans::gui {

class CommandDispatcher;
class GuiStateStore;

class MigrationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString detail READ detail NOTIFY stateChanged)
    Q_PROPERTY(QString databasePath READ databasePath NOTIFY stateChanged)
    Q_PROPERTY(QString backupDirectory READ backupDirectory NOTIFY stateChanged)
    Q_PROPERTY(bool databaseExists READ databaseExists NOTIFY stateChanged)
    Q_PROPERTY(bool actionAvailable READ actionAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool migrationConfirmationRequired
        READ migrationConfirmationRequired NOTIFY confirmationChanged)
    Q_PROPERTY(bool replacementConfirmationRequired
        READ replacementConfirmationRequired NOTIFY confirmationChanged)

public:
    MigrationController(
        GuiStateStore& state,
        CommandDispatcher& commands,
        QObject* parent = nullptr);

    [[nodiscard]] QString state() const;
    [[nodiscard]] QString detail() const;
    [[nodiscard]] QString databasePath() const;
    [[nodiscard]] QString backupDirectory() const;
    [[nodiscard]] bool databaseExists() const noexcept;
    [[nodiscard]] bool actionAvailable() const noexcept;
    [[nodiscard]] bool migrationConfirmationRequired() const noexcept;
    [[nodiscard]] bool replacementConfirmationRequired() const noexcept;

    Q_INVOKABLE void inspect();
    Q_INVOKABLE void requestMigration();
    Q_INVOKABLE void confirmMigration();
    Q_INVOKABLE void cancelMigration();
    Q_INVOKABLE void confirmReplacement();
    Q_INVOKABLE void cancelReplacement();

signals:
    void stateChanged();
    void confirmationChanged();

private slots:
    void handleStorageChanged();
    void handleCommandFinished(
        const QString& command,
        const QString& outcome,
        const QString& errorCode,
        const QString& field,
        const QString& detail);

private:
    void submitMigration(bool replace);

    GuiStateStore& state_;
    CommandDispatcher& commands_;
    bool migration_confirmation_required_ = false;
    bool replacement_confirmation_required_ = false;
};

} // namespace ccs_trans::gui
