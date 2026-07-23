#include "controllers/command_dispatcher.hpp"
#include "features/profiles/profiles_controller.hpp"
#include "features/rules/rules_controller.hpp"
#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"
#include "ipc/gui_ipc_client.hpp"
#include "models/profile_summary_model.hpp"
#include "state/gui_state_store.hpp"

#include <QSignalSpy>
#include <QTest>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string readFrame(HANDLE pipe) {
    ccs::gui_ipc::FrameDecoder decoder;
    std::array<std::uint8_t, 4096> buffer{};
    std::vector<std::string> frames;
    while (frames.empty()) {
        DWORD received = 0;
        require(ReadFile(
                    pipe,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &received,
                    nullptr)
                && received > 0,
            "failed to read fake tray pipe");
        ccs::gui_ipc::FrameError error;
        require(decoder.consume(
                    std::span<const std::uint8_t>(buffer.data(), received),
                    frames,
                    error),
            "fake tray rejected a client frame");
    }
    require(frames.size() == 1 && decoder.buffered_bytes() == 0,
        "fake tray expected exactly one client frame");
    return std::move(frames.front());
}

void writeEnvelope(HANDLE pipe, const ccs::gui_ipc::Envelope& envelope) {
    std::string content;
    std::string error;
    require(ccs::gui_ipc::serialize_envelope(envelope, content, error), error);
    std::vector<std::uint8_t> frame;
    ccs::gui_ipc::FrameError frame_error;
    require(ccs::gui_ipc::encode_frame(content, frame, frame_error),
        "failed to encode fake tray frame");
    DWORD written = 0;
    require(WriteFile(
                pipe,
                frame.data(),
                static_cast<DWORD>(frame.size()),
                &written,
                nullptr)
            && written == frame.size(),
        "failed to write fake tray frame");
}

ccs::gui_ipc::Envelope serverEnvelope(
    ccs::gui_ipc::MessageKind kind,
    std::uint64_t sequence,
    std::string request_id = {}) {
    ccs::gui_ipc::Envelope envelope;
    envelope.kind = kind;
    envelope.request_id = std::move(request_id);
    envelope.session_id = "qt-client-session";
    envelope.source_commit = "qt-client-source";
    envelope.sequence = sequence;
    return envelope;
}

ccs::gui_ipc::Snapshot initialSnapshot() {
    ccs::gui_ipc::Snapshot snapshot;
    snapshot.revision = 1;
    snapshot.application = {"starting", "127.0.0.1", 15723, {}, 0};
    snapshot.profiles = {
        {11, "alpha", true, std::string{"responses"}, "ready", {}, 2, 2},
        {22, "beta", false, std::string{"chat-completions"}, "ready", {}, 1, 0},
    };
    snapshot.selection = {std::string{"alpha"}, 11};
    snapshot.profile_editor = ccs::gui_ipc::ProfileEditor{11, "alpha", {
        {"id", "profile", "text", true, {}, {}, {},
            "field.profile.id", "runtime_reload", std::string{"alpha"}},
        {"enabled", "profile", "boolean", true, {}, {}, {},
            "field.profile.enabled", "runtime_reload", true},
    }};
    snapshot.rules_editor = ccs::gui_ipc::RulesEditor{11, "alpha", "[]\n", {}};
    snapshot.draft = {"clean", false, 3, "base-revision"};
    snapshot.lightweight_mode = false;
    return snapshot;
}

class FakeTray {
public:
    FakeTray() {
        const auto nonce = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        pipe_name_ = L"\\\\.\\pipe\\ccs-trans.qt-client-test." + nonce;
    }

    ~FakeTray() {
        if (thread_.joinable()) thread_.join();
    }

    std::string pipeNameUtf8() const {
        return std::string(pipe_name_.begin(), pipe_name_.end());
    }

    void start() {
        thread_ = std::thread([this] {
            try {
                run();
            } catch (const std::exception& exception) {
                error_ = exception.what();
            }
            finished_.store(true);
        });
    }

    void join() {
        if (thread_.joinable()) thread_.join();
        require(error_.empty(), error_);
    }

    bool finished() const noexcept { return finished_.load(); }

private:
    void run() {
        const HANDLE pipe = CreateNamedPipeW(
            pipe_name_.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            64U * 1024U,
            64U * 1024U,
            0,
            nullptr);
        require(pipe != INVALID_HANDLE_VALUE, "failed to create fake tray pipe");
        const bool connected = ConnectNamedPipe(pipe, nullptr) != FALSE
            || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) {
            CloseHandle(pipe);
            throw std::runtime_error("failed to accept Qt GUI pipe client");
        }

        std::string error;
        ccs::gui_ipc::Envelope hello_envelope;
        require(ccs::gui_ipc::parse_envelope(
                    readFrame(pipe), hello_envelope, error), error);
        ccs::gui_ipc::Hello hello;
        require(hello_envelope.kind == ccs::gui_ipc::MessageKind::Hello
                && ccs::gui_ipc::parse_hello(
                    hello_envelope.payload_json, hello, error)
                && hello.handshake_token == "qt-client-token"
                && hello.process_id == GetCurrentProcessId(),
            "Qt GUI hello did not preserve bootstrap identity");

        ccs::gui_ipc::HelloResult hello_result{
            true,
            "0.8-test",
            "qt-client-source",
            "qt-client-session",
            1,
            ccs::gui_ipc::ErrorCode::None,
            {},
        };
        auto response = serverEnvelope(
            ccs::gui_ipc::MessageKind::HelloResult, 0, "hello");
        response.result = ccs::gui_ipc::ResultCode::Accepted;
        response.error_code = ccs::gui_ipc::ErrorCode::None;
        require(ccs::gui_ipc::serialize_hello_result(
                    hello_result, response.payload_json, error), error);
        writeEnvelope(pipe, response);

        auto snapshot = initialSnapshot();
        response = serverEnvelope(ccs::gui_ipc::MessageKind::Snapshot, 1, "initial");
        response.base_revision = snapshot.draft.base_revision;
        require(ccs::gui_ipc::serialize_snapshot(
                    snapshot, response.payload_json, error), error);
        writeEnvelope(pipe, response);

        ccs::gui_ipc::Envelope command_envelope;
        require(ccs::gui_ipc::parse_envelope(
                    readFrame(pipe), command_envelope, error), error);
        ccs::gui_ipc::Command command;
        require(command_envelope.kind == ccs::gui_ipc::MessageKind::Command
                && command_envelope.sequence == 1
                && ccs::gui_ipc::parse_command(
                    command_envelope.payload_json, command, error)
                && command.command == ccs::gui_ipc::GuiCommand::Refresh,
            "Qt GUI command was not typed or sequenced");

        ccs::gui_ipc::CommandStatus status;
        status.sequence = command_envelope.sequence;
        status.command = "refresh";
        status.outcome = ccs::gui_ipc::ResultCode::Succeeded;
        response = serverEnvelope(
            ccs::gui_ipc::MessageKind::CommandStatus,
            2,
            command_envelope.request_id);
        response.result = status.outcome;
        response.error_code = status.error;
        require(ccs::gui_ipc::serialize_command_status(
                    status, response.payload_json, error), error);
        writeEnvelope(pipe, response);

        ccs::gui_ipc::StateDelta delta;
        delta.from_revision = 1;
        delta.revision = 2;
        delta.application = ccs::gui_ipc::ApplicationStatus{
            "running", "127.0.0.1", 15723, {}, 0};
        delta.profiles = std::vector<ccs::gui_ipc::ProfileSummary>{
            snapshot.profiles[1], snapshot.profiles[0]};
        response = serverEnvelope(
            ccs::gui_ipc::MessageKind::StateChanged, 3, "state");
        require(ccs::gui_ipc::serialize_state_delta(
                    delta, response.payload_json, error), error);
        writeEnvelope(pipe, response);
        writeEnvelope(pipe, serverEnvelope(
            ccs::gui_ipc::MessageKind::Activate, 4, "activate"));
        writeEnvelope(pipe, serverEnvelope(
            ccs::gui_ipc::MessageKind::Shutdown, 5, "shutdown"));
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    std::wstring pipe_name_;
    std::thread thread_;
    std::string error_;
    std::atomic_bool finished_{false};
};

class GuiIpcClientTests final : public QObject {
    Q_OBJECT

private slots:
    void handshakeStateCommandAndLifecycle() {
        FakeTray tray;
        tray.start();
        ccs::gui_ipc::LaunchBootstrap bootstrap{
            tray.pipeNameUtf8(),
            "0.8-test",
            "qt-client-source",
            "qt-client-instance",
            "qt-client-token",
            "qt-client-session",
        };
        ccs_trans::gui::GuiIpcClient client(std::move(bootstrap));
        ccs_trans::gui::GuiStateStore state(client);
        ccs_trans::gui::CommandDispatcher commands(client);
        ccs_trans::gui::ProfilesController profile_controller(state, commands);
        ccs_trans::gui::RulesController rules_controller(state, commands);
        auto* profiles = qobject_cast<ccs_trans::gui::ProfileSummaryModel*>(
            state.profilesModel());
        QVERIFY(profiles != nullptr);
        QSignalSpy reset_spy(profiles, &QAbstractItemModel::modelReset);
        QSignalSpy move_spy(profiles, &QAbstractItemModel::rowsMoved);
        QSignalSpy command_spy(
            &commands, &ccs_trans::gui::CommandDispatcher::commandFinished);
        QSignalSpy activate_spy(
            &client, &ccs_trans::gui::GuiIpcClient::activateRequested);
        QSignalSpy shutdown_spy(
            &client, &ccs_trans::gui::GuiIpcClient::shutdownRequested);

        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(client.ready(), 3000);
        QCOMPARE(state.revision(), 1ULL);
        QCOMPARE(profiles->count(), 2);
        QCOMPARE(profile_controller.profileId(), QStringLiteral("alpha"));
        QCOMPARE(rules_controller.text(), QStringLiteral("[]\n"));
        profile_controller.setProfileId(QStringLiteral("local alpha"));
        rules_controller.setText(QStringLiteral("local\r\nrules"));
        commands.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(command_spy.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(state.revision(), 2ULL, 3000);
        QCOMPARE(state.applicationState(), QStringLiteral("running"));
        QCOMPARE(profiles->keyAt(0), 22);
        QCOMPARE(profile_controller.profileId(), QStringLiteral("local alpha"));
        QCOMPARE(rules_controller.text(), QStringLiteral("local\nrules"));
        QVERIFY(profile_controller.dirty());
        QVERIFY(rules_controller.dirty());
        QCOMPARE(reset_spy.count(), 0);
        QVERIFY(move_spy.count() >= 1);
        QTRY_COMPARE_WITH_TIMEOUT(activate_spy.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(shutdown_spy.count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(tray.finished(), 3000);
        client.stop();
        tray.join();
    }
};

} // namespace

QTEST_GUILESS_MAIN(GuiIpcClientTests)

#include "gui_ipc_client_tests.moc"
