#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"
#include "hosts/windows/gui_bridge/gui_ipc_server.hpp"
#include "hosts/windows/platform/gui_pipe_security.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void write_all(HANDLE pipe, const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        require(WriteFile(
                    pipe,
                    bytes.data() + offset,
                    static_cast<DWORD>(bytes.size() - offset),
                    &written,
                    nullptr) != FALSE,
            "fake GUI failed to write named pipe");
        require(written != 0, "fake GUI named-pipe write returned zero");
        offset += written;
    }
}

class FakeGuiClient {
public:
    explicit FakeGuiClient(const std::wstring& pipe_name) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            pipe_ = CreateFileW(
                pipe_name.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_PIPE_BUSY) {
                std::this_thread::sleep_for(10ms);
            } else {
                (void)WaitNamedPipeW(pipe_name.c_str(), 100);
            }
        }
        require(pipe_ != INVALID_HANDLE_VALUE, "fake GUI failed to connect named pipe");
    }

    ~FakeGuiClient() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    }

    FakeGuiClient(const FakeGuiClient&) = delete;
    FakeGuiClient& operator=(const FakeGuiClient&) = delete;

    void send(const ccs::gui_ipc::Envelope& envelope) {
        std::string content;
        std::string error;
        require(ccs::gui_ipc::serialize_envelope(envelope, content, error), error);
        std::vector<std::uint8_t> frame;
        ccs::gui_ipc::FrameError frame_error;
        require(ccs::gui_ipc::encode_frame(content, frame, frame_error),
            "fake GUI failed to frame envelope");
        write_all(pipe_, frame);
    }

    ccs::gui_ipc::Envelope receive(std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!frames_.empty()) {
                auto content = std::move(frames_.front());
                frames_.erase(frames_.begin());
                ccs::gui_ipc::Envelope envelope;
                std::string error;
                require(ccs::gui_ipc::parse_envelope(content, envelope, error), error);
                return envelope;
            }
            DWORD available = 0;
            if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
                throw std::runtime_error("fake GUI pipe disconnected before response");
            }
            if (available == 0) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            std::vector<std::uint8_t> bytes(std::min<DWORD>(available, 64U * 1024U));
            DWORD received = 0;
            require(ReadFile(pipe_, bytes.data(), static_cast<DWORD>(bytes.size()),
                        &received, nullptr) != FALSE,
                "fake GUI failed to read named pipe");
            ccs::gui_ipc::FrameError frame_error;
            require(decoder_.consume(
                        std::span<const std::uint8_t>(bytes.data(), received),
                        frames_,
                        frame_error),
                "fake GUI received an invalid frame");
        }
        throw std::runtime_error("timed out waiting for GUI IPC response");
    }

    void send_partial_frame() {
        const std::vector<std::uint8_t> partial = {20, 0, 0, 0, '{', '"'};
        write_all(pipe_, partial);
    }

    void close() {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    ccs::gui_ipc::FrameDecoder decoder_;
    std::vector<std::string> frames_;
};

ccs::gui_ipc::Snapshot snapshot(std::uint64_t revision, std::string state) {
    ccs::gui_ipc::Snapshot value;
    value.revision = revision;
    value.application = {std::move(state), "127.0.0.1", 15723, {}, 0};
    value.draft = {"clean", false, revision, "base-" + std::to_string(revision)};
    value.lightweight_mode = true;
    return value;
}

ccs::gui_ipc::Envelope hello_envelope(
    const std::string& token,
    const std::string& session,
    std::string instance = "test") {
    ccs::gui_ipc::Hello hello{
        "0.8.0", "source", std::move(instance), token, GetCurrentProcessId()};
    std::string payload;
    std::string error;
    require(ccs::gui_ipc::serialize_hello(hello, payload, error), error);
    ccs::gui_ipc::Envelope envelope;
    envelope.kind = ccs::gui_ipc::MessageKind::Hello;
    envelope.request_id = "hello-" + session;
    envelope.source_commit = "source";
    envelope.payload_json = std::move(payload);
    return envelope;
}

ccs::gui_ipc::Envelope client_envelope(
    ccs::gui_ipc::MessageKind kind,
    const std::string& session,
    std::uint64_t sequence,
    std::string request_id,
    std::string payload = "{}") {
    ccs::gui_ipc::Envelope envelope;
    envelope.kind = kind;
    envelope.request_id = std::move(request_id);
    envelope.session_id = session;
    envelope.sequence = sequence;
    envelope.source_commit = "source";
    envelope.payload_json = std::move(payload);
    return envelope;
}

template <typename Predicate>
void wait_until(Predicate predicate, const std::string& message) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return;
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error(message);
}

void authenticate(
    FakeGuiClient& client,
    const std::string& token,
    const std::string& session) {
    client.send(hello_envelope(token, session));
    const auto hello_result = client.receive();
    require(hello_result.kind == ccs::gui_ipc::MessageKind::HelloResult
            && hello_result.result == ccs::gui_ipc::ResultCode::Accepted
            && hello_result.session_id == session,
        "fake GUI handshake was not accepted");
    ccs::gui_ipc::HelloResult result;
    std::string error;
    require(ccs::gui_ipc::parse_hello_result(
                hello_result.payload_json, result, error), error);
    require(result.accepted, "hello_result payload rejected fake GUI");
    const auto initial = client.receive();
    require(initial.kind == ccs::gui_ipc::MessageKind::Snapshot,
        "accepted handshake did not send initial snapshot");
}

void test_pipe_identity() {
    const auto nonce = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    ccs::GuiPipeIdentity first;
    ccs::GuiPipeIdentity second;
    std::string error;
    require(ccs::make_gui_pipe_identity(
                std::filesystem::temp_directory_path() / ("ccs-ipc-" + nonce),
                "test",
                first,
                error), error);
    require(ccs::make_gui_pipe_identity(
                std::filesystem::temp_directory_path() / ("ccs-ipc-other-" + nonce),
                "test",
                second,
                error), error);
    require(first.identity_hash.size() == 64
            && first.gui_pipe_name != second.gui_pipe_name
            && first.gui_pipe_name.find(first.current_user_sid) == std::wstring::npos
            && first.gui_pipe_name.find(L"ccs-ipc-") == std::wstring::npos,
        "public GUI pipe identity leaks source material or is not unique");
    std::string secret;
    require(ccs::generate_gui_secret(secret, error) && secret.size() == 64, error);
}

void test_server_protocol_and_reconnects() {
    const auto nonce = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    ccs::GuiPipeIdentity identity;
    std::string error;
    require(ccs::make_gui_pipe_identity(
                std::filesystem::temp_directory_path() / ("ccs-server-" + nonce),
                "test",
                identity,
                error), error);

    std::mutex state_mutex;
    auto current = snapshot(1, "running");
    std::mutex command_mutex;
    std::condition_variable command_cv;
    std::size_t command_count = 0;
    ccs::GuiIpcServer* server_pointer = nullptr;
    ccs::GuiIpcServerCallbacks callbacks;
    callbacks.snapshot_provider = [&] {
        std::lock_guard<std::mutex> lock(state_mutex);
        return current;
    };
    callbacks.command_handler = [&](const auto& envelope, const auto& command) {
        require(command.command == ccs::gui_ipc::GuiCommand::Refresh,
            "server command callback received wrong typed command");
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            ++command_count;
        }
        command_cv.notify_all();
        ccs::gui_ipc::CommandStatus status;
        status.sequence = envelope.sequence;
        status.command = "refresh";
        status.outcome = ccs::gui_ipc::ResultCode::Succeeded;
        require(server_pointer->send_command_completion(
                    envelope.request_id, status, envelope.base_revision),
            "server rejected command completion");
    };
    callbacks.event_handler = [](std::string_view, std::string, std::uint64_t) {
        throw std::runtime_error("test event observer failure");
    };

    ccs::GuiIpcServer server({
        identity.gui_pipe_name,
        identity.current_user_sid,
        "0.8.0",
        "source",
        "test",
        16,
    }, std::move(callbacks));
    server_pointer = &server;
    require(server.start(error), error);

    require(server.prepare_session(
                {"wrong-token-expected", "wrong-session", GetCurrentProcessId()},
                error), error);
    const auto rejected_disconnects = server.status().disconnects;
    {
        FakeGuiClient rejected(identity.gui_pipe_name);
        rejected.send(hello_envelope("wrong-token", "wrong-session"));
        const auto response = rejected.receive();
        require(response.kind == ccs::gui_ipc::MessageKind::HelloResult
                && response.result == ccs::gui_ipc::ResultCode::Rejected
                && response.error_code
                    == ccs::gui_ipc::ErrorCode::AuthenticationFailed,
            "invalid handshake did not receive typed rejection");
    }
    wait_until([&] {
        const auto status = server.status();
        return !status.connected && status.disconnects > rejected_disconnects;
    },
        "server retained rejected connection");

    require(server.prepare_session(
                {"token-main", "session-main", GetCurrentProcessId()}, error), error);
    const auto main_disconnects = server.status().disconnects;
    {
        FakeGuiClient client(identity.gui_pipe_name);
        authenticate(client, "token-main", "session-main");

        client.send(client_envelope(
            ccs::gui_ipc::MessageKind::Ping, "session-main", 1, "ping-1"));
        const auto pong = client.receive();
        require(pong.kind == ccs::gui_ipc::MessageKind::Pong
                && pong.request_id == "ping-1",
            "ping did not receive correlated pong");

        ccs::gui_ipc::Command command;
        command.command = ccs::gui_ipc::GuiCommand::Refresh;
        std::string command_payload;
        require(ccs::gui_ipc::serialize_command(command, command_payload, error), error);
        client.send(client_envelope(
            ccs::gui_ipc::MessageKind::Command,
            "session-main",
            2,
            "command-1",
            command_payload));
        const auto completion = client.receive();
        require(completion.kind == ccs::gui_ipc::MessageKind::CommandStatus
                && completion.request_id == "command-1"
                && completion.result == ccs::gui_ipc::ResultCode::Succeeded,
            "command did not receive reliable correlated result");

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            current = snapshot(4, "reloading");
        }
        require(server.publish_state(snapshot(4, "reloading")),
            "server rejected state publication");
        const auto changed = client.receive();
        require(changed.kind == ccs::gui_ipc::MessageKind::StateChanged,
            "state update sent a repeated full snapshot");
        ccs::gui_ipc::StateDelta delta;
        require(ccs::gui_ipc::parse_state_delta(changed.payload_json, delta, error), error);
        require(delta.from_revision == 1 && delta.revision == 4
                && delta.application && delta.application->state == "reloading",
            "state delta revision or content is incorrect");

        client.send(client_envelope(
            ccs::gui_ipc::MessageKind::Shutdown,
            "session-main",
            3,
            "client-shutdown"));
        const auto shutdown_rejection = client.receive();
        require(shutdown_rejection.kind == ccs::gui_ipc::MessageKind::CommandStatus
                && shutdown_rejection.request_id == "client-shutdown"
                && shutdown_rejection.result == ccs::gui_ipc::ResultCode::Rejected
                && shutdown_rejection.error_code
                    == ccs::gui_ipc::ErrorCode::MalformedMessage,
            "client-sent shutdown was not rejected as a GUI protocol violation");

        client.send(client_envelope(
            ccs::gui_ipc::MessageKind::Ping, "session-main", 4, "ping-2"));
        const auto second_pong = client.receive();
        require(second_pong.kind == ccs::gui_ipc::MessageKind::Pong
                && second_pong.request_id == "ping-2",
            "protocol rejection incorrectly disconnected the GUI session");
        client.close();
    }
    wait_until([&] {
        const auto status = server.status();
        return !status.connected && status.disconnects > main_disconnects;
    },
        "server retained main fake GUI connection");

    require(server.prepare_session(
                {"token-half", "session-half", GetCurrentProcessId()}, error), error);
    const auto half_disconnects = server.status().disconnects;
    {
        FakeGuiClient half(identity.gui_pipe_name);
        half.send_partial_frame();
        half.close();
    }
    wait_until([&] {
        const auto status = server.status();
        return !status.connected && status.disconnects > half_disconnects;
    },
        "server did not recover after a half frame");

    for (int cycle = 0; cycle < 100; ++cycle) {
        const auto suffix = std::to_string(cycle);
        const auto token = "token-" + suffix;
        const auto session = "session-" + suffix;
        require(server.prepare_session(
                    {token, session, GetCurrentProcessId()}, error), error);
        const auto cycle_disconnects = server.status().disconnects;
        {
            FakeGuiClient client(identity.gui_pipe_name);
            authenticate(client, token, session);
            client.close();
        }
        wait_until([&] {
            const auto status = server.status();
            return !status.connected && status.disconnects > cycle_disconnects;
        },
            "server retained reconnect cycle " + suffix);
    }
    const auto status = server.status();
    require(status.accepted_connections == 101
            && status.rejected_connections >= 1
            && status.disconnects >= 102,
        "server connection accounting missed reconnects");
    {
        std::unique_lock<std::mutex> lock(command_mutex);
        require(command_cv.wait_for(lock, 1s, [&] { return command_count == 1; }),
            "typed command callback did not complete");
    }
    server.stop();
    require(!server.status().running, "server did not stop cleanly");
}

} // namespace

int main() {
    try {
        test_pipe_identity();
        test_server_protocol_and_reconnects();
        std::cout << "GUI IPC server tests ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "GUI IPC server tests failed: " << exception.what() << '\n';
        return 1;
    }
}
