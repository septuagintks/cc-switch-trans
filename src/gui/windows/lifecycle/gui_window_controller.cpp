#include "lifecycle/gui_window_controller.hpp"

#include "controllers/command_dispatcher.hpp"
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
    CommandDispatcher& commands,
    QObject* parent)
    : QObject(parent)
    , application_(application)
    , client_(client)
    , state_(state)
    , commands_(commands) {
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
    connect(&commands_, &CommandDispatcher::commandFinished,
            this, &GuiWindowController::handleCommandFinished);
}

bool GuiWindowController::windowVisible() const noexcept {
    return window_visible_;
}

bool GuiWindowController::closePromptVisible() const noexcept {
    return close_prompt_visible_;
}

bool GuiWindowController::closeHasLocalEdits() const noexcept {
    return close_has_local_edits_;
}

bool GuiWindowController::closeHasDraftEdits() const noexcept {
    return close_has_draft_edits_;
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

void GuiWindowController::requestClose(bool has_local_edits) {
    if (commands_.busy() || state_.commandPending()) {
        emit closeBlocked(QStringLiteral("Wait for the current command to finish."));
        return;
    }
    const bool prompt_changed = close_has_local_edits_ != has_local_edits
        || close_has_draft_edits_ != state_.draftDirty();
    close_has_local_edits_ = has_local_edits;
    close_has_draft_edits_ = state_.draftDirty();
    if (close_has_local_edits_ || close_has_draft_edits_) {
        if (close_prompt_visible_ && prompt_changed) emit closePromptChanged();
        else setClosePromptVisible(true);
        return;
    }
    performPendingClose();
}

void GuiWindowController::resolveClose(const QString& decision) {
    if (!close_prompt_visible_) return;
    if (decision == QStringLiteral("cancel")) {
        setClosePromptVisible(false);
        return;
    }
    if (close_has_local_edits_ && decision != QStringLiteral("discard")) {
        emit closeBlocked(QStringLiteral(
            "Save the current editor before applying the draft."));
        return;
    }
    setClosePromptVisible(false);
    if (close_has_draft_edits_) {
        close_after_command_ = true;
        if (decision == QStringLiteral("apply")) commands_.applyDraft();
        else commands_.discardDraft();
        return;
    }
    performPendingClose();
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

void GuiWindowController::handleCommandFinished(
    const QString& command,
    const QString& outcome,
    const QString& errorCode,
    const QString&,
    const QString&) {
    if (!close_after_command_
        || (command != QStringLiteral("apply_draft")
            && command != QStringLiteral("discard_draft"))) {
        return;
    }
    close_after_command_ = false;
    const bool completed = errorCode == QStringLiteral("none")
        && outcome == QStringLiteral("succeeded");
    const bool saved = command == QStringLiteral("apply_draft")
        && outcome == QStringLiteral("saved_pending_runtime_apply");
    if (completed || saved) performPendingClose();
}

void GuiWindowController::destroyProcess() {
    expected_shutdown_ = true;
    client_.stop();
    application_.quit();
}

void GuiWindowController::performPendingClose() {
    close_after_command_ = false;
    close_has_local_edits_ = false;
    close_has_draft_edits_ = false;
    setClosePromptVisible(false);
    if (state_.lightweightMode()) destroyProcess();
    else hide();
}

void GuiWindowController::setClosePromptVisible(bool visible) {
    if (close_prompt_visible_ == visible) return;
    close_prompt_visible_ = visible;
    emit closePromptChanged();
}

void GuiWindowController::setWindowVisible(bool visible) {
    if (window_visible_ == visible) return;
    window_visible_ = visible;
    emit windowVisibleChanged();
}

} // namespace ccs_trans::gui
