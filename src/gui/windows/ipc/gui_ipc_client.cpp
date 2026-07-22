#include "ipc/gui_ipc_client.hpp"

#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <QByteArray>

#include <span>
#include <utility>
#include <vector>

namespace ccs_trans::gui {

namespace {

constexpr qint64 kMaximumQueuedClientBytes = 1024 * 1024;
constexpr qint64 kConnectTimeoutMilliseconds = 5000;
constexpr int kReconnectDelayMilliseconds = 50;

QString fromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

GuiIpcClient::GuiIpcClient(
    ccs::gui_ipc::LaunchBootstrap bootstrap,
    QObject* parent)
    : QObject(parent)
    , bootstrap_(std::move(bootstrap)) {
    socket_.setReadBufferSize(
        static_cast<qint64>(ccs::gui_ipc::kMaximumFrameBytes) + 4);
    reconnect_timer_.setSingleShot(true);
    connect(&reconnect_timer_, &QTimer::timeout,
            this, &GuiIpcClient::connectToServer);
    connect(&socket_, &QLocalSocket::connected,
            this, &GuiIpcClient::handleConnected);
    connect(&socket_, &QLocalSocket::readyRead,
            this, &GuiIpcClient::handleReadyRead);
    connect(&socket_, &QLocalSocket::disconnected,
            this, &GuiIpcClient::handleDisconnected);
    connect(&socket_, &QLocalSocket::errorOccurred,
            this, &GuiIpcClient::handleSocketError);
}

QString GuiIpcClient::connectionState() const { return stateName(state_); }
QString GuiIpcClient::connectionDetail() const { return detail_; }
bool GuiIpcClient::ready() const noexcept { return state_ == State::Ready; }

const std::optional<ccs::gui_ipc::Snapshot>& GuiIpcClient::snapshot() const noexcept {
    return tracker_.snapshot();
}

const std::optional<GuiIpcClient::CommandEvent>&
GuiIpcClient::lastCommandEvent() const noexcept {
    return last_command_event_;
}

void GuiIpcClient::start() {
    if (state_ != State::Idle && state_ != State::Closed) return;
    stopping_ = false;
    reconnect_scheduled_ = false;
    authenticated_ = false;
    next_client_sequence_ = 1;
    next_server_sequence_ = 1;
    decoder_.reset();
    tracker_.disconnect();
    connect_elapsed_.start();
    connectToServer();
}

void GuiIpcClient::stop() {
    if (stopping_) return;
    stopping_ = true;
    reconnect_timer_.stop();
    reconnect_scheduled_ = false;
    authenticated_ = false;
    tracker_.disconnect();
    socket_.abort();
    setState(State::Closed);
}

bool GuiIpcClient::sendCommand(
    ccs::gui_ipc::Command command,
    QString& request_id,
    QString& error) {
    error.clear();
    request_id.clear();
    if (!ready()) {
        error = QStringLiteral("GUI IPC session is not ready");
        return false;
    }
    ccs::gui_ipc::Envelope envelope;
    envelope.kind = ccs::gui_ipc::MessageKind::Command;
    envelope.request_id = "command-" + std::to_string(next_request_id_++);
    if (tracker_.snapshot()) {
        envelope.base_revision = tracker_.snapshot()->draft.base_revision;
        if (!command.expected_draft_revision) {
            command.expected_draft_revision = tracker_.snapshot()->draft.revision;
        }
        if (!command.expected_base_revision
            && !tracker_.snapshot()->draft.base_revision.empty()) {
            command.expected_base_revision = tracker_.snapshot()->draft.base_revision;
        }
    }
    std::string codec_error;
    if (!ccs::gui_ipc::serialize_command(command, envelope.payload_json, codec_error)) {
        error = fromUtf8(codec_error);
        return false;
    }
    request_id = fromUtf8(envelope.request_id);
    return sendEnvelope(std::move(envelope), error);
}

void GuiIpcClient::connectToServer() {
    reconnect_scheduled_ = false;
    if (stopping_) return;
    socket_.abort();
    decoder_.reset();
    setState(State::Connecting);
    socket_.connectToServer(
        fromUtf8(bootstrap_.pipe_name_utf8),
        QIODevice::ReadWrite);
}

void GuiIpcClient::handleConnected() {
    if (stopping_) return;
    setState(State::Handshaking);
    QString error;
    if (!sendHello(error)) fail(std::move(error));
}

void GuiIpcClient::handleReadyRead() {
    const QByteArray bytes = socket_.readAll();
    if (bytes.isEmpty()) return;
    std::vector<std::string> frames;
    ccs::gui_ipc::FrameError frame_error;
    if (!decoder_.consume(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                static_cast<std::size_t>(bytes.size())),
            frames,
            frame_error)) {
        fail(QStringLiteral("GUI IPC frame rejected: %1")
                 .arg(QString::fromLatin1(
                     ccs::gui_ipc::frame_error_name(frame_error))));
        return;
    }
    for (const auto& frame : frames) {
        QString error;
        if (!handleFrame(frame, error)) {
            fail(std::move(error));
            return;
        }
    }
}

void GuiIpcClient::handleDisconnected() {
    if (stopping_) return;
    if (!authenticated_) {
        scheduleReconnect(socket_.errorString());
        return;
    }
    fail(QStringLiteral("GUI IPC session disconnected"));
}

void GuiIpcClient::handleSocketError(QLocalSocket::LocalSocketError) {
    if (stopping_) return;
    if (!authenticated_) {
        scheduleReconnect(socket_.errorString());
        return;
    }
    fail(socket_.errorString());
}

void GuiIpcClient::setState(State state, QString detail) {
    const bool was_ready = ready();
    if (state_ != state) {
        state_ = state;
        emit connectionStateChanged();
    }
    if (detail_ != detail) {
        detail_ = std::move(detail);
        emit connectionDetailChanged();
    }
    if (was_ready != ready()) emit readyChanged();
}

void GuiIpcClient::scheduleReconnect(QString detail) {
    if (stopping_ || reconnect_scheduled_) return;
    if (!connect_elapsed_.isValid()
        || connect_elapsed_.elapsed() >= kConnectTimeoutMilliseconds) {
        fail(detail.isEmpty()
                 ? QStringLiteral("timed out connecting to the tray GUI pipe")
                 : std::move(detail));
        return;
    }
    socket_.abort();
    setState(State::Connecting, std::move(detail));
    reconnect_scheduled_ = true;
    reconnect_timer_.start(kReconnectDelayMilliseconds);
}

void GuiIpcClient::fail(QString detail) {
    if (state_ == State::Failed || stopping_) return;
    stopping_ = true;
    reconnect_timer_.stop();
    reconnect_scheduled_ = false;
    authenticated_ = false;
    tracker_.disconnect();
    socket_.abort();
    if (detail.isEmpty()) detail = QStringLiteral("GUI IPC session failed");
    setState(State::Failed, detail);
    emit sessionLost(detail_);
}

bool GuiIpcClient::sendHello(QString& error) {
    ccs::gui_ipc::Hello hello{
        bootstrap_.version,
        bootstrap_.source_commit,
        bootstrap_.instance_identity,
        bootstrap_.handshake_token,
        static_cast<std::uint64_t>(GetCurrentProcessId()),
    };
    ccs::gui_ipc::Envelope envelope;
    envelope.kind = ccs::gui_ipc::MessageKind::Hello;
    envelope.request_id = "hello";
    envelope.source_commit = bootstrap_.source_commit;
    std::string codec_error;
    if (!ccs::gui_ipc::serialize_hello(hello, envelope.payload_json, codec_error)) {
        error = fromUtf8(codec_error);
        return false;
    }
    return sendEnvelope(std::move(envelope), error);
}

bool GuiIpcClient::sendSnapshotRequest(QString& error) {
    ccs::gui_ipc::Envelope envelope;
    envelope.kind = ccs::gui_ipc::MessageKind::SnapshotRequest;
    envelope.request_id = "snapshot-" + std::to_string(next_request_id_++);
    return sendEnvelope(std::move(envelope), error);
}

bool GuiIpcClient::sendEnvelope(
    ccs::gui_ipc::Envelope envelope,
    QString& error) {
    error.clear();
    if (socket_.state() != QLocalSocket::ConnectedState) {
        error = QStringLiteral("GUI IPC pipe is not connected");
        return false;
    }
    envelope.protocol = std::string(ccs::gui_ipc::kProtocol);
    envelope.source_commit = bootstrap_.source_commit;
    if (envelope.kind != ccs::gui_ipc::MessageKind::Hello) {
        envelope.session_id = bootstrap_.session_id;
        envelope.sequence = next_client_sequence_++;
    }
    std::string content;
    std::string codec_error;
    if (!ccs::gui_ipc::serialize_envelope(envelope, content, codec_error)) {
        error = fromUtf8(codec_error);
        return false;
    }
    std::vector<std::uint8_t> encoded;
    ccs::gui_ipc::FrameError frame_error;
    if (!ccs::gui_ipc::encode_frame(content, encoded, frame_error)) {
        error = QStringLiteral("failed to frame GUI IPC message: %1")
                    .arg(QString::fromLatin1(
                        ccs::gui_ipc::frame_error_name(frame_error)));
        return false;
    }
    if (socket_.bytesToWrite() + static_cast<qint64>(encoded.size())
        > kMaximumQueuedClientBytes) {
        error = QStringLiteral("GUI IPC client write queue is full");
        return false;
    }
    const auto written = socket_.write(
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<qint64>(encoded.size()));
    if (written != static_cast<qint64>(encoded.size())) {
        error = socket_.errorString().isEmpty()
            ? QStringLiteral("failed to queue GUI IPC message")
            : socket_.errorString();
        return false;
    }
    return true;
}

bool GuiIpcClient::handleFrame(std::string_view frame, QString& error) {
    ccs::gui_ipc::Envelope envelope;
    std::string codec_error;
    if (!ccs::gui_ipc::parse_envelope(frame, envelope, codec_error)) {
        error = fromUtf8(codec_error);
        return false;
    }
    if (!authenticated_) return handleHelloResult(envelope, error);
    if (!validateServerEnvelope(envelope, error)) return false;
    return handleAuthenticated(envelope, error);
}

bool GuiIpcClient::handleHelloResult(
    const ccs::gui_ipc::Envelope& envelope,
    QString& error) {
    if (envelope.kind != ccs::gui_ipc::MessageKind::HelloResult
        || envelope.request_id != "hello"
        || envelope.protocol != ccs::gui_ipc::kProtocol
        || envelope.source_commit != bootstrap_.source_commit
        || envelope.session_id != bootstrap_.session_id) {
        error = QStringLiteral("tray returned an invalid GUI hello response");
        return false;
    }
    ccs::gui_ipc::HelloResult result;
    std::string codec_error;
    if (!ccs::gui_ipc::parse_hello_result(
            envelope.payload_json, result, codec_error)) {
        error = fromUtf8(codec_error);
        return false;
    }
    if (!result.accepted || result.version != bootstrap_.version
        || result.source_commit != bootstrap_.source_commit
        || result.session_id != bootstrap_.session_id) {
        error = result.detail.empty()
            ? QStringLiteral("tray rejected the GUI session")
            : fromUtf8(result.detail);
        return false;
    }
    authenticated_ = true;
    bootstrap_.handshake_token.clear();
    next_server_sequence_ = 1;
    return true;
}

bool GuiIpcClient::validateServerEnvelope(
    const ccs::gui_ipc::Envelope& envelope,
    QString& error) {
    if (envelope.protocol != ccs::gui_ipc::kProtocol
        || envelope.source_commit != bootstrap_.source_commit
        || envelope.session_id != bootstrap_.session_id) {
        error = QStringLiteral("GUI IPC server identity changed during the session");
        return false;
    }
    if (envelope.sequence != next_server_sequence_) {
        error = QStringLiteral("GUI IPC server sequence is not contiguous");
        return false;
    }
    ++next_server_sequence_;
    return true;
}

bool GuiIpcClient::handleAuthenticated(
    const ccs::gui_ipc::Envelope& envelope,
    QString& error) {
    std::string codec_error;
    switch (envelope.kind) {
    case ccs::gui_ipc::MessageKind::Snapshot: {
        ccs::gui_ipc::Snapshot snapshot;
        if (!ccs::gui_ipc::parse_snapshot(
                envelope.payload_json, snapshot, codec_error)
            || !tracker_.accept_snapshot(snapshot, codec_error)) {
            error = fromUtf8(codec_error);
            return false;
        }
        setState(State::Ready);
        emit snapshotAvailable();
        return true;
    }
    case ccs::gui_ipc::MessageKind::StateChanged: {
        ccs::gui_ipc::StateDelta delta;
        if (!ccs::gui_ipc::parse_state_delta(
                envelope.payload_json, delta, codec_error)) {
            error = fromUtf8(codec_error);
            return false;
        }
        if (!tracker_.apply_delta(delta, codec_error)) {
            return sendSnapshotRequest(error);
        }
        emit snapshotAvailable();
        return true;
    }
    case ccs::gui_ipc::MessageKind::CommandStatus: {
        ccs::gui_ipc::CommandStatus status;
        if (!ccs::gui_ipc::parse_command_status(
                envelope.payload_json, status, codec_error)) {
            error = fromUtf8(codec_error);
            return false;
        }
        last_command_event_ = CommandEvent{fromUtf8(envelope.request_id), status};
        emit commandStatusAvailable();
        return true;
    }
    case ccs::gui_ipc::MessageKind::Activate:
        emit activateRequested();
        return true;
    case ccs::gui_ipc::MessageKind::Shutdown:
        emit shutdownRequested();
        return true;
    case ccs::gui_ipc::MessageKind::Pong:
        return true;
    case ccs::gui_ipc::MessageKind::Hello:
    case ccs::gui_ipc::MessageKind::HelloResult:
    case ccs::gui_ipc::MessageKind::Ping:
    case ccs::gui_ipc::MessageKind::SnapshotRequest:
    case ccs::gui_ipc::MessageKind::Command:
    case ccs::gui_ipc::MessageKind::MaintenanceRequest:
    case ccs::gui_ipc::MessageKind::MaintenanceResult:
        error = QStringLiteral("tray sent a message kind that is invalid for a GUI client");
        return false;
    }
    error = QStringLiteral("tray sent an unknown GUI IPC message kind");
    return false;
}

QString GuiIpcClient::stateName(State state) {
    switch (state) {
    case State::Idle: return QStringLiteral("idle");
    case State::Connecting: return QStringLiteral("connecting");
    case State::Handshaking: return QStringLiteral("handshaking");
    case State::Ready: return QStringLiteral("ready");
    case State::Failed: return QStringLiteral("failed");
    case State::Closed: return QStringLiteral("closed");
    }
    return QStringLiteral("unknown");
}

} // namespace ccs_trans::gui
