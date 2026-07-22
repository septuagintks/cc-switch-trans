#pragma once

#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/protocol_types.hpp"
#include "gui_ipc/session.hpp"

#include <QElapsedTimer>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

#include <cstdint>
#include <optional>
#include <string>

namespace ccs_trans::gui {

class GuiIpcClient final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionStateChanged)
    Q_PROPERTY(QString connectionDetail READ connectionDetail NOTIFY connectionDetailChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    struct CommandEvent {
        QString request_id;
        ccs::gui_ipc::CommandStatus status;
    };

    explicit GuiIpcClient(
        ccs::gui_ipc::LaunchBootstrap bootstrap,
        QObject* parent = nullptr);

    [[nodiscard]] QString connectionState() const;
    [[nodiscard]] QString connectionDetail() const;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const std::optional<ccs::gui_ipc::Snapshot>& snapshot() const noexcept;
    [[nodiscard]] const std::optional<CommandEvent>& lastCommandEvent() const noexcept;

    void start();
    void stop();
    bool sendCommand(
        ccs::gui_ipc::Command command,
        QString& request_id,
        QString& error);

signals:
    void connectionStateChanged();
    void connectionDetailChanged();
    void readyChanged();
    void snapshotAvailable();
    void commandStatusAvailable();
    void activateRequested();
    void shutdownRequested();
    void sessionLost(const QString& detail);

private slots:
    void connectToServer();
    void handleConnected();
    void handleReadyRead();
    void handleDisconnected();
    void handleSocketError(QLocalSocket::LocalSocketError error);

private:
    enum class State {
        Idle,
        Connecting,
        Handshaking,
        Ready,
        Failed,
        Closed,
    };

    void setState(State state, QString detail = {});
    void scheduleReconnect(QString detail);
    void fail(QString detail);
    bool sendHello(QString& error);
    bool sendSnapshotRequest(QString& error);
    bool sendEnvelope(ccs::gui_ipc::Envelope envelope, QString& error);
    bool handleFrame(std::string_view frame, QString& error);
    bool handleHelloResult(
        const ccs::gui_ipc::Envelope& envelope,
        QString& error);
    bool handleAuthenticated(
        const ccs::gui_ipc::Envelope& envelope,
        QString& error);
    bool validateServerEnvelope(
        const ccs::gui_ipc::Envelope& envelope,
        QString& error);
    static QString stateName(State state);

    ccs::gui_ipc::LaunchBootstrap bootstrap_;
    QLocalSocket socket_;
    QTimer reconnect_timer_;
    QElapsedTimer connect_elapsed_;
    ccs::gui_ipc::FrameDecoder decoder_;
    ccs::gui_ipc::ClientStateTracker tracker_;
    std::optional<CommandEvent> last_command_event_;
    State state_ = State::Idle;
    QString detail_;
    std::uint64_t next_client_sequence_ = 1;
    std::uint64_t next_request_id_ = 1;
    std::uint64_t next_server_sequence_ = 1;
    bool authenticated_ = false;
    bool stopping_ = false;
    bool reconnect_scheduled_ = false;
};

} // namespace ccs_trans::gui
