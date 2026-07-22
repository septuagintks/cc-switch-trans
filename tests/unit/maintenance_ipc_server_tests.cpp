#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"
#include "hosts/windows/maintenance/maintenance_ipc_server.hpp"
#include "hosts/windows/platform/gui_pipe_security.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
void wait_until(Predicate predicate, const std::string& message) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(message);
        }
        std::this_thread::sleep_for(10ms);
    }
}

class MaintenanceClient {
public:
    explicit MaintenanceClient(const std::wstring& pipe_name) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            handle_ = CreateFileW(
                pipe_name.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) return;
            if (GetLastError() != ERROR_PIPE_BUSY
                || !WaitNamedPipeW(pipe_name.c_str(), 50)) {
                std::this_thread::sleep_for(10ms);
            }
        }
        throw std::runtime_error("failed to connect the maintenance test client");
    }

    ~MaintenanceClient() { close(); }

    MaintenanceClient(const MaintenanceClient&) = delete;
    MaintenanceClient& operator=(const MaintenanceClient&) = delete;

    void send(const ccs::gui_ipc::Envelope& envelope) {
        std::string content;
        std::string error;
        require(ccs::gui_ipc::serialize_envelope(envelope, content, error), error);
        std::vector<std::uint8_t> frame;
        ccs::gui_ipc::FrameError frame_error;
        require(ccs::gui_ipc::encode_frame(content, frame, frame_error),
            "failed to frame maintenance test request");
        DWORD written = 0;
        require(WriteFile(
                    handle_,
                    frame.data(),
                    static_cast<DWORD>(frame.size()),
                    &written,
                    nullptr)
                && written == frame.size(),
            "failed to send maintenance test request");
    }

    ccs::gui_ipc::Envelope receive() {
        ccs::gui_ipc::FrameDecoder decoder;
        std::vector<std::string> frames;
        std::vector<std::uint8_t> buffer(4096);
        while (frames.empty()) {
            DWORD received = 0;
            require(ReadFile(
                        handle_,
                        buffer.data(),
                        static_cast<DWORD>(buffer.size()),
                        &received,
                        nullptr)
                    && received != 0,
                "maintenance server closed without a response");
            ccs::gui_ipc::FrameError frame_error;
            require(decoder.consume(
                        std::span<const std::uint8_t>(buffer.data(), received),
                        frames,
                        frame_error),
                "maintenance response frame was invalid");
        }
        ccs::gui_ipc::Envelope response;
        std::string error;
        require(ccs::gui_ipc::parse_envelope(frames.front(), response, error), error);
        return response;
    }

    void close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

ccs::gui_ipc::Envelope maintenance_envelope(
    ccs::gui_ipc::MaintenanceCommand command,
    std::string request_id) {
    ccs::gui_ipc::MaintenanceRequest request{command, 1000};
    std::string payload;
    std::string error;
    require(ccs::gui_ipc::serialize_maintenance_request(request, payload, error), error);
    ccs::gui_ipc::Envelope envelope;
    envelope.protocol = std::string(ccs::gui_ipc::kMaintenanceProtocol);
    envelope.kind = ccs::gui_ipc::MessageKind::MaintenanceRequest;
    envelope.request_id = std::move(request_id);
    envelope.session_id = "installer-test-session";
    envelope.sequence = 1;
    envelope.source_commit = "different-installer-build";
    envelope.payload_json = std::move(payload);
    return envelope;
}

void test_maintenance_endpoint() {
    const auto nonce = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    ccs::GuiPipeIdentity identity;
    std::string error;
    require(ccs::make_gui_pipe_identity(
                std::filesystem::current_path() / ("maintenance-test-" + nonce),
                "maintenance-test-" + nonce,
                identity,
                error),
        error);

    std::atomic_int query_count = 0;
    std::atomic_int shutdown_count = 0;
    std::atomic_int release_wait_count = 0;
    ccs::MaintenanceIpcServerCallbacks callbacks;
    callbacks.request_handler = [&](const ccs::gui_ipc::MaintenanceRequest& request) {
        ccs::gui_ipc::MaintenanceResult result;
        result.succeeded = true;
        result.state = ccs::gui_ipc::maintenance_command_name(request.command);
        if (request.command == ccs::gui_ipc::MaintenanceCommand::QueryVersion) {
            ++query_count;
        } else if (request.command
            == ccs::gui_ipc::MaintenanceCommand::RequestShutdown) {
            ++shutdown_count;
        } else if (request.command
            == ccs::gui_ipc::MaintenanceCommand::WaitForRelease) {
            ++release_wait_count;
        }
        return result;
    };
    callbacks.event_handler = [](std::string_view, std::string, std::uint64_t) {
        throw std::runtime_error("test maintenance observer failure");
    };
    ccs::MaintenanceIpcServer server({
        identity.maintenance_pipe_name,
        identity.current_user_sid,
        "0.8-test",
        "maintenance-source",
    }, std::move(callbacks));
    require(server.start(error), error);

    {
        MaintenanceClient client(identity.maintenance_pipe_name);
        client.send(maintenance_envelope(
            ccs::gui_ipc::MaintenanceCommand::QueryVersion, "version"));
        const auto response = client.receive();
        require(response.protocol == ccs::gui_ipc::kMaintenanceProtocol
                && response.kind == ccs::gui_ipc::MessageKind::MaintenanceResult
                && response.request_id == "version"
                && response.result == ccs::gui_ipc::ResultCode::Succeeded,
            "maintenance version request did not preserve its response envelope");
        ccs::gui_ipc::MaintenanceResult payload;
        require(ccs::gui_ipc::parse_maintenance_result(
                    response.payload_json, payload, error)
                && payload.succeeded
                && payload.version == "0.8-test"
                && payload.source_commit == "maintenance-source"
                && payload.state == "query_version",
            "maintenance version response was not typed");
    }
    {
        MaintenanceClient client(identity.maintenance_pipe_name);
        client.send(maintenance_envelope(
            ccs::gui_ipc::MaintenanceCommand::RequestShutdown, "shutdown"));
        const auto response = client.receive();
        require(response.request_id == "shutdown"
                && response.result == ccs::gui_ipc::ResultCode::Succeeded,
            "maintenance shutdown request was not acknowledged");
    }
    {
        MaintenanceClient client(identity.maintenance_pipe_name);
        client.send(maintenance_envelope(
            ccs::gui_ipc::MaintenanceCommand::WaitForRelease, "release"));
        const auto response = client.receive();
        require(response.request_id == "release"
                && response.result == ccs::gui_ipc::ResultCode::Succeeded,
            "maintenance release wait request was not acknowledged");
    }
    wait_until([&] {
        return query_count == 1 && shutdown_count == 1 && release_wait_count == 1;
    },
        "maintenance request handler did not receive typed commands");

    {
        MaintenanceClient client(identity.maintenance_pipe_name);
        ccs::gui_ipc::Command command;
        std::string payload;
        require(ccs::gui_ipc::serialize_command(command, payload, error), error);
        ccs::gui_ipc::Envelope wrong;
        wrong.kind = ccs::gui_ipc::MessageKind::Command;
        wrong.request_id = "gui-command";
        wrong.session_id = "gui-session";
        wrong.sequence = 1;
        wrong.source_commit = "maintenance-source";
        wrong.payload_json = std::move(payload);
        client.send(wrong);
    }
    wait_until([&] { return server.status().rejected_requests >= 1; },
        "maintenance endpoint accepted a GUI command envelope");
    const auto status = server.status();
    require(status.accepted_requests == 3 && status.rejected_requests >= 1,
        "maintenance endpoint request accounting is incomplete");
    server.stop();
    require(!server.status().running, "maintenance endpoint did not stop cleanly");
}

} // namespace

int main() {
    try {
        test_maintenance_endpoint();
        std::cout << "Maintenance IPC server tests ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Maintenance IPC server tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
