#pragma once

#include <QObject>
#include <QPointer>

class QGuiApplication;
class QQuickWindow;

namespace ccs_trans::gui {

class GuiIpcClient;
class GuiStateStore;
class CommandDispatcher;

class GuiWindowController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool windowVisible READ windowVisible NOTIFY windowVisibleChanged)
    Q_PROPERTY(bool closePromptVisible READ closePromptVisible NOTIFY closePromptChanged)
    Q_PROPERTY(bool closeHasLocalEdits READ closeHasLocalEdits NOTIFY closePromptChanged)
    Q_PROPERTY(bool closeHasDraftEdits READ closeHasDraftEdits NOTIFY closePromptChanged)

public:
    GuiWindowController(
        QGuiApplication& application,
        GuiIpcClient& client,
        GuiStateStore& state,
        CommandDispatcher& commands,
        QObject* parent = nullptr);

    [[nodiscard]] bool windowVisible() const noexcept;
    [[nodiscard]] bool closePromptVisible() const noexcept;
    [[nodiscard]] bool closeHasLocalEdits() const noexcept;
    [[nodiscard]] bool closeHasDraftEdits() const noexcept;
    void attachWindow(QQuickWindow* window);

    Q_INVOKABLE void requestClose(bool hasLocalEdits = false);
    Q_INVOKABLE void resolveClose(const QString& decision);
    Q_INVOKABLE void activate();
    Q_INVOKABLE void hide();

signals:
    void windowVisibleChanged();
    void closePromptChanged();
    void closeBlocked(const QString& reason);

private slots:
    void handleSnapshot();
    void handleShutdown();
    void handleSessionLost(const QString& detail);
    void syncWindowVisibility();
    void handleCommandFinished(
        const QString& command,
        const QString& outcome,
        const QString& errorCode,
        const QString& field,
        const QString& detail);

private:
    void destroyProcess();
    void performPendingClose();
    void setClosePromptVisible(bool visible);
    void setWindowVisible(bool visible);

    QGuiApplication& application_;
    GuiIpcClient& client_;
    GuiStateStore& state_;
    CommandDispatcher& commands_;
    QPointer<QQuickWindow> window_;
    bool window_visible_ = false;
    bool initial_activation_done_ = false;
    bool expected_shutdown_ = false;
    bool close_prompt_visible_ = false;
    bool close_has_local_edits_ = false;
    bool close_has_draft_edits_ = false;
    bool close_after_command_ = false;
};

} // namespace ccs_trans::gui
