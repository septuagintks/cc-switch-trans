#pragma once

#include "gui_ipc/protocol_types.hpp"

#include <QObject>

namespace ccs_trans::gui {

class GuiIpcClient;

class CommandDispatcher final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString localError READ localError NOTIFY localErrorChanged)

public:
    explicit CommandDispatcher(GuiIpcClient& client, QObject* parent = nullptr);

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString localError() const;
    bool submit(ccs::gui_ipc::Command command);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void applyDraft();
    Q_INVOKABLE void discardDraft();
    Q_INVOKABLE void reloadDraft();
    Q_INVOKABLE void startService();
    Q_INVOKABLE void stopService();
    Q_INVOKABLE void reloadService();
    Q_INVOKABLE void quitApplication();

signals:
    void busyChanged();
    void localErrorChanged();
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
    void submitSimple(ccs::gui_ipc::GuiCommand command);

    GuiIpcClient& client_;
    QString pending_request_;
    QString local_error_;
    bool busy_ = false;
};

} // namespace ccs_trans::gui
