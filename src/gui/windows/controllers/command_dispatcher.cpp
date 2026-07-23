#include "controllers/command_dispatcher.hpp"

#include "ipc/gui_ipc_client.hpp"

#include <utility>

namespace ccs_trans::gui {

CommandDispatcher::CommandDispatcher(GuiIpcClient& client, QObject* parent)
    : QObject(parent), client_(client) {
    connect(&client_, &GuiIpcClient::commandStatusAvailable,
            this, &CommandDispatcher::handleCommandStatus);
    connect(&client_, &GuiIpcClient::sessionLost,
            this, &CommandDispatcher::handleSessionLost);
}

bool CommandDispatcher::busy() const noexcept { return busy_; }
QString CommandDispatcher::localError() const { return local_error_; }
QString CommandDispatcher::lastErrorCode() const { return last_error_code_; }
QString CommandDispatcher::lastErrorField() const { return last_error_field_; }
QString CommandDispatcher::lastErrorDetail() const { return last_error_detail_; }
QString CommandDispatcher::lastOutcome() const { return last_outcome_; }
bool CommandDispatcher::errorVisible() const noexcept { return error_visible_; }

bool CommandDispatcher::submit(ccs::gui_ipc::Command command) {
    if (busy_) {
        const auto detail = QStringLiteral("Wait for the current command to finish.");
        setLocalError(detail);
        setErrorState(
            QStringLiteral("busy"), {}, detail, QStringLiteral("rejected"));
        return false;
    }
    QString request_id;
    QString error;
    if (!client_.sendCommand(std::move(command), request_id, error)) {
        setLocalError(error);
        setErrorState(
            QStringLiteral("disconnected"), {}, std::move(error),
            QStringLiteral("failed"));
        return false;
    }
    pending_request_ = std::move(request_id);
    setLocalError({});
    setErrorState({}, {}, {}, {});
    setBusy(true);
    return true;
}

void CommandDispatcher::refresh() {
    submitSimple(ccs::gui_ipc::GuiCommand::Refresh);
}

void CommandDispatcher::applyDraft() {
    submitSimple(ccs::gui_ipc::GuiCommand::ApplyDraft);
}

void CommandDispatcher::discardDraft() {
    submitSimple(ccs::gui_ipc::GuiCommand::DiscardDraft);
}

void CommandDispatcher::reloadDraft() {
    submitSimple(ccs::gui_ipc::GuiCommand::ReloadDraft);
}

void CommandDispatcher::reloadDraftDiscardingChanges() {
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::ReloadDraft;
    request.unsaved_decision = ccs::gui_ipc::UnsavedDecision::Discard;
    (void)submit(std::move(request));
}

void CommandDispatcher::startService() {
    submitSimple(ccs::gui_ipc::GuiCommand::StartService);
}

void CommandDispatcher::stopService() {
    submitSimple(ccs::gui_ipc::GuiCommand::StopService);
}

void CommandDispatcher::reloadService() {
    submitSimple(ccs::gui_ipc::GuiCommand::ReloadService);
}

void CommandDispatcher::quitApplication() {
    submitSimple(ccs::gui_ipc::GuiCommand::QuitApplication);
}

void CommandDispatcher::handleCommandStatus() {
    const auto& event = client_.lastCommandEvent();
    if (!event) return;
    if (!pending_request_.isEmpty() && event->request_id != pending_request_) return;
    pending_request_.clear();
    setBusy(false);
    const auto& status = event->status;
    const auto error_code = QString::fromLatin1(
        ccs::gui_ipc::error_code_name(status.error));
    const auto detail = text(status.detail);
    setLocalError(status.error == ccs::gui_ipc::ErrorCode::None
        ? QString{} : detail);
    setErrorState(
        status.error == ccs::gui_ipc::ErrorCode::None ? QString{} : error_code,
        text(status.field), detail,
        QString::fromLatin1(ccs::gui_ipc::result_code_name(status.outcome)));
    emit commandFinished(
        text(status.command),
        QString::fromLatin1(ccs::gui_ipc::result_code_name(status.outcome)),
        error_code,
        text(status.field),
        text(status.detail));
}

void CommandDispatcher::handleSessionLost() {
    pending_request_.clear();
    setBusy(false);
    setLocalError(QStringLiteral("The tray connection was lost."));
    setErrorState(
        QStringLiteral("disconnected"), {},
        QStringLiteral("The tray connection was lost."),
        QStringLiteral("failed"));
}

QString CommandDispatcher::text(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

void CommandDispatcher::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void CommandDispatcher::setLocalError(QString error) {
    if (local_error_ == error) return;
    local_error_ = std::move(error);
    emit localErrorChanged();
}

void CommandDispatcher::setErrorState(
    QString code,
    QString field,
    QString detail,
    QString outcome) {
    const bool visible = !code.isEmpty();
    if (last_error_code_ == code && last_error_field_ == field
        && last_error_detail_ == detail && last_outcome_ == outcome
        && error_visible_ == visible) {
        return;
    }
    last_error_code_ = std::move(code);
    last_error_field_ = std::move(field);
    last_error_detail_ = std::move(detail);
    last_outcome_ = std::move(outcome);
    error_visible_ = visible;
    emit errorChanged();
}

void CommandDispatcher::clearError() {
    setLocalError({});
    setErrorState({}, {}, {}, {});
}

void CommandDispatcher::submitSimple(ccs::gui_ipc::GuiCommand command) {
    ccs::gui_ipc::Command request;
    request.command = command;
    (void)submit(std::move(request));
}

} // namespace ccs_trans::gui
