#pragma once

#include <QObject>
#include <QPointer>

class QGuiApplication;
class QQuickWindow;

namespace ccs_trans::gui {

class GuiIpcClient;
class GuiStateStore;

class GuiWindowController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool windowVisible READ windowVisible NOTIFY windowVisibleChanged)

public:
    GuiWindowController(
        QGuiApplication& application,
        GuiIpcClient& client,
        GuiStateStore& state,
        QObject* parent = nullptr);

    [[nodiscard]] bool windowVisible() const noexcept;
    void attachWindow(QQuickWindow* window);

    Q_INVOKABLE void requestClose();
    Q_INVOKABLE void activate();
    Q_INVOKABLE void hide();

signals:
    void windowVisibleChanged();

private slots:
    void handleSnapshot();
    void handleShutdown();
    void handleSessionLost(const QString& detail);
    void syncWindowVisibility();

private:
    void destroyProcess();
    void setWindowVisible(bool visible);

    QGuiApplication& application_;
    GuiIpcClient& client_;
    GuiStateStore& state_;
    QPointer<QQuickWindow> window_;
    bool window_visible_ = false;
    bool initial_activation_done_ = false;
    bool expected_shutdown_ = false;
};

} // namespace ccs_trans::gui
