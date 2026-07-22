#include "lifecycle/gui_window_controller.hpp"

#include "ipc/gui_ipc_client.hpp"
#include "state/gui_state_store.hpp"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QTimer>

namespace ccs_trans::gui {

GuiWindowController::GuiWindowController(
    QGuiApplication& application,
    GuiIpcClient& client,
    GuiStateStore& state,
    QObject* parent)
    : QObject(parent)
    , application_(application)
    , client_(client)
    , state_(state) {
    connect(&state_, &GuiStateStore::snapshotApplied,
            this, &GuiWindowController::handleSnapshot);
    connect(&client_, &GuiIpcClient::activateRequested,
            this, &GuiWindowController::activate);
    connect(&client_, &GuiIpcClient::shutdownRequested,
            this, &GuiWindowController::handleShutdown);
    connect(&client_, &GuiIpcClient::sessionLost,
            this, &GuiWindowController::handleSessionLost);
    connect(&application_, &QCoreApplication::aboutToQuit,
            &client_, &GuiIpcClient::stop);
}

bool GuiWindowController::windowVisible() const noexcept {
    return window_visible_;
}

void GuiWindowController::attachWindow(QQuickWindow* window) {
    if (window_ == window) return;
    if (window_) disconnect(window_, nullptr, this, nullptr);
    window_ = window;
    if (window_) {
        connect(window_, &QWindow::visibleChanged,
                this, &GuiWindowController::syncWindowVisibility);
        window_->hide();
    }
    setWindowVisible(false);
}

void GuiWindowController::requestClose() {
    if (state_.lightweightMode()) {
        destroyProcess();
        return;
    }
    hide();
}

void GuiWindowController::activate() {
    if (!window_ || !client_.ready() || state_.revision() == 0) return;
    setWindowVisible(true);
    window_->show();
    window_->raise();
    window_->requestActivate();
}

void GuiWindowController::hide() {
    if (window_) window_->hide();
    setWindowVisible(false);
}

void GuiWindowController::handleSnapshot() {
    if (!initial_activation_done_ && state_.revision() > 0) {
        initial_activation_done_ = true;
        activate();
        return;
    }
    if (initial_activation_done_ && state_.lightweightMode()
        && !window_visible_) {
        destroyProcess();
    }
}

void GuiWindowController::handleShutdown() {
    expected_shutdown_ = true;
    client_.stop();
    QTimer::singleShot(0, &application_, &QCoreApplication::quit);
}

void GuiWindowController::handleSessionLost(const QString&) {
    if (expected_shutdown_) return;
    QTimer::singleShot(0, &application_, [this] { application_.exit(2); });
}

void GuiWindowController::syncWindowVisibility() {
    setWindowVisible(window_ && window_->isVisible());
}

void GuiWindowController::destroyProcess() {
    expected_shutdown_ = true;
    client_.stop();
    application_.quit();
}

void GuiWindowController::setWindowVisible(bool visible) {
    if (window_visible_ == visible) return;
    window_visible_ = visible;
    emit windowVisibleChanged();
}

} // namespace ccs_trans::gui
