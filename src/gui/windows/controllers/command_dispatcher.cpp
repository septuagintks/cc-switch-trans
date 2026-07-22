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

bool CommandDispatcher::submit(ccs::gui_ipc::Command command) {
    if (busy_) {
        setLocalError(QStringLiteral("Wait for the current command to finish."));
        return false;
    }
    QString request_id;
    QString error;
    if (!client_.sendCommand(std::move(command), request_id, error)) {
        setLocalError(std::move(error));
        return false;
    }
    pending_request_ = std::move(request_id);
    setLocalError({});
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
    setLocalError(status.error == ccs::gui_ipc::ErrorCode::None
        ? QString{} : text(status.detail));
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

void CommandDispatcher::submitSimple(ccs::gui_ipc::GuiCommand command) {
    ccs::gui_ipc::Command request;
    request.command = command;
    (void)submit(std::move(request));
}

} // namespace ccs_trans::gui
