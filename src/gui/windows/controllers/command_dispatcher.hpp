#pragma once

#include "gui_ipc/protocol_types.hpp"

#include <QObject>

namespace ccs_trans::gui {

class GuiIpcClient;

class CommandDispatcher final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString localError READ localError NOTIFY localErrorChanged)
    Q_PROPERTY(QString lastErrorCode READ lastErrorCode NOTIFY errorChanged)
    Q_PROPERTY(QString lastErrorField READ lastErrorField NOTIFY errorChanged)
    Q_PROPERTY(QString lastErrorDetail READ lastErrorDetail NOTIFY errorChanged)
    Q_PROPERTY(QString lastOutcome READ lastOutcome NOTIFY errorChanged)
    Q_PROPERTY(bool errorVisible READ errorVisible NOTIFY errorChanged)

public:
    explicit CommandDispatcher(GuiIpcClient& client, QObject* parent = nullptr);

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString localError() const;
    [[nodiscard]] QString lastErrorCode() const;
    [[nodiscard]] QString lastErrorField() const;
    [[nodiscard]] QString lastErrorDetail() const;
    [[nodiscard]] QString lastOutcome() const;
    [[nodiscard]] bool errorVisible() const noexcept;
    bool submit(ccs::gui_ipc::Command command);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void applyDraft();
    Q_INVOKABLE void discardDraft();
    Q_INVOKABLE void reloadDraft();
    Q_INVOKABLE void reloadDraftDiscardingChanges();
    Q_INVOKABLE void startService();
    Q_INVOKABLE void stopService();
    Q_INVOKABLE void reloadService();
    Q_INVOKABLE void quitApplication();
    Q_INVOKABLE void clearError();

signals:
    void busyChanged();
    void localErrorChanged();
    void errorChanged();
    void commandFinished(
        const QString& command,
        const QString& outcome,
        const QString& errorCode,
        const QString& field,
        const QString& detail);

private slots:
    void handleCommandStatus();
    void handleSessionLost();

private:
    static QString text(const std::string& value);
    void setBusy(bool busy);
    void setLocalError(QString error);
    void setErrorState(
        QString code,
        QString field,
        QString detail,
        QString outcome);
    void submitSimple(ccs::gui_ipc::GuiCommand command);

    GuiIpcClient& client_;
    QString pending_request_;
    QString local_error_;
    QString last_error_code_;
    QString last_error_field_;
    QString last_error_detail_;
    QString last_outcome_;
    bool busy_ = false;
    bool error_visible_ = false;
};

} // namespace ccs_trans::gui
