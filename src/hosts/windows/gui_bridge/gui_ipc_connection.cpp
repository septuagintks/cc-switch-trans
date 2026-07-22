#include "hosts/windows/gui_bridge/gui_ipc_connection.hpp"

#ifdef _WIN32

#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"
#include "hosts/windows/windows_error.hpp"

#include <array>
#include <exception>
#include <utility>
#include <vector>

namespace ccs {

GuiIpcConnection::GuiIpcConnection(
    HANDLE pipe,
    std::uint64_t client_process_id,
    gui_ipc::ServerSessionPolicy policy,
    GuiIpcServerCallbacks callbacks,
    std::size_t outbound_capacity)
    : pipe_(pipe)
    , client_process_id_(client_process_id)
    , callbacks_(std::move(callbacks))
    , session_(policy)
    , outbound_(
          pipe,
          policy.session_id,
          policy.source_commit,
          [this] { return next_server_sequence(); },
          [this](std::string error) {
              event("writer_failed", std::move(error));
              (void)CancelIoEx(pipe_, nullptr);
          },
          outbound_capacity) {}

GuiIpcConnection::~GuiIpcConnection() {
    stop();
}

void GuiIpcConnection::run() {
    std::string error;
    if (!outbound_.start(error)) {
        event("writer_start_failed", std::move(error));
        return;
    }

    gui_ipc::FrameDecoder decoder;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    HANDLE read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (read_event == nullptr) {
        event("reader_start_failed", windows_error_message(
            "failed to create GUI named-pipe read event", GetLastError()));
        outbound_.stop();
        return;
    }
    bool drain_before_close = false;
    while (!stopping_.load()) {
        DWORD received = 0;
        OVERLAPPED operation{};
        operation.hEvent = read_event;
        (void)ResetEvent(read_event);
        bool read_succeeded = ReadFile(
            pipe_, buffer.data(), static_cast<DWORD>(buffer.size()),
            &received, &operation) != FALSE;
        auto code = read_succeeded ? ERROR_SUCCESS : GetLastError();
        if (!read_succeeded && code == ERROR_IO_PENDING) {
            if (WaitForSingleObject(read_event, INFINITE) == WAIT_OBJECT_0) {
                read_succeeded = GetOverlappedResult(
                    pipe_, &operation, &received, FALSE) != FALSE;
                code = read_succeeded ? ERROR_SUCCESS : GetLastError();
            } else {
                code = GetLastError();
            }
        }
        if (!read_succeeded) {
            if (code != ERROR_BROKEN_PIPE
                && code != ERROR_PIPE_NOT_CONNECTED
                && code != ERROR_OPERATION_ABORTED) {
                event("reader_failed", windows_error_message(
                    "failed to read GUI named pipe", code));
            }
            break;
        }
        if (received == 0) break;
        std::vector<std::string> frames;
        gui_ipc::FrameError frame_error;
        if (!decoder.consume(
                std::span<const std::uint8_t>(buffer.data(), received),
                frames,
                frame_error)) {
            event("frame_rejected", gui_ipc::frame_error_name(frame_error));
            break;
        }
        bool keep_reading = true;
        for (const auto& frame : frames) {
            if (!handle_frame(frame, drain_before_close)) {
                keep_reading = false;
                break;
            }
        }
        if (!keep_reading) break;
    }
    CloseHandle(read_event);
    gui_ipc::FrameError final_error;
    if (!stopping_.load() && !drain_before_close && !decoder.finish(final_error)) {
        event("incomplete_frame", gui_ipc::frame_error_name(final_error));
    }
    authenticated_.store(false);
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_.disconnect();
    }
    if (drain_before_close) {
        outbound_.close_after_drain();
        outbound_.join();
        if (!stopping_.load()) (void)FlushFileBuffers(pipe_);
    } else {
        outbound_.stop();
    }
    event("disconnected");
}

void GuiIpcConnection::stop() noexcept {
    if (stopping_.exchange(true)) return;
    outbound_.stop();
    if (pipe_ != INVALID_HANDLE_VALUE) (void)CancelIoEx(pipe_, nullptr);
}

bool GuiIpcConnection::handle_frame(
    std::string_view frame,
    bool& drain_before_close) {
    gui_ipc::Envelope envelope;
    std::string error;
    if (!gui_ipc::parse_envelope(frame, envelope, error)) {
        event("message_rejected", std::move(error));
        return false;
    }
    if (!authenticated_.load()) {
        return handle_hello(envelope, drain_before_close);
    }
    gui_ipc::SessionValidation validation;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        validation = session_.accept_message(envelope);
    }
    if (!validation.accepted) {
        (void)send_protocol_rejection(
            envelope, validation.error, std::move(validation.detail));
        drain_before_close = true;
        return false;
    }
    return handle_authenticated(envelope);
}

bool GuiIpcConnection::handle_hello(
    const gui_ipc::Envelope& envelope,
    bool& drain_before_close) {
    gui_ipc::Hello hello;
    std::string error;
    if (envelope.kind != gui_ipc::MessageKind::Hello
        || !gui_ipc::parse_hello(envelope.payload_json, hello, error)) {
        event("hello_rejected", error.empty()
            ? "the first message is not hello" : std::move(error));
        return false;
    }
    gui_ipc::HelloResult result;
    gui_ipc::SessionValidation validation;
    const auto snapshot = current_snapshot();
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        validation = session_.accept_hello(
            envelope, hello, client_process_id_, snapshot.revision, result);
    }
    std::string payload;
    if (!gui_ipc::serialize_hello_result(result, payload, error)) {
        event("hello_result_failed", std::move(error));
        return false;
    }
    auto response = server_envelope(
        gui_ipc::MessageKind::HelloResult, envelope.request_id);
    response.result = validation.accepted
        ? gui_ipc::ResultCode::Accepted : gui_ipc::ResultCode::Rejected;
    response.error_code = result.error;
    response.payload_json = std::move(payload);
    if (!outbound_.enqueue(std::move(response))) {
        event("outbound_rejected", "hello_result");
        return false;
    }
    if (!validation.accepted) {
        event("hello_rejected", std::move(validation.detail));
        drain_before_close = true;
        return false;
    }
    authenticated_.store(true);
    if (!outbound_.enqueue_snapshot("initial-snapshot", snapshot)) {
        event("outbound_rejected", "initial_snapshot");
        return false;
    }
    event("authenticated");
    return true;
}

bool GuiIpcConnection::handle_authenticated(
    const gui_ipc::Envelope& envelope) {
    switch (envelope.kind) {
    case gui_ipc::MessageKind::Command: {
        gui_ipc::Command command;
        std::string error;
        if (!gui_ipc::parse_command(envelope.payload_json, command, error)) {
            return send_protocol_rejection(
                envelope, gui_ipc::ErrorCode::MalformedMessage, std::move(error));
        }
        if (!callbacks_.command_handler) {
            return send_protocol_rejection(
                envelope,
                gui_ipc::ErrorCode::ServiceUnavailable,
                "GUI command routing is unavailable");
        }
        try {
            callbacks_.command_handler(envelope, command);
        } catch (const std::exception& exception) {
            return send_protocol_rejection(
                envelope, gui_ipc::ErrorCode::Internal, exception.what());
        } catch (...) {
            return send_protocol_rejection(
                envelope, gui_ipc::ErrorCode::Internal,
                "GUI command callback failed");
        }
        return true;
    }
    case gui_ipc::MessageKind::SnapshotRequest:
        if (!outbound_.enqueue_snapshot(envelope.request_id, current_snapshot())) {
            event("outbound_rejected", "snapshot");
            return false;
        }
        return true;
    case gui_ipc::MessageKind::Ping: {
        auto pong = server_envelope(gui_ipc::MessageKind::Pong, envelope.request_id);
        if (!outbound_.enqueue(std::move(pong))) {
            event("outbound_rejected", "pong");
            return false;
        }
        return true;
    }
    case gui_ipc::MessageKind::Hello:
    case gui_ipc::MessageKind::HelloResult:
    case gui_ipc::MessageKind::Activate:
    case gui_ipc::MessageKind::Shutdown:
    case gui_ipc::MessageKind::Pong:
    case gui_ipc::MessageKind::Snapshot:
    case gui_ipc::MessageKind::StateChanged:
    case gui_ipc::MessageKind::CommandStatus:
    case gui_ipc::MessageKind::MaintenanceRequest:
    case gui_ipc::MessageKind::MaintenanceResult:
        return send_protocol_rejection(
            envelope,
            gui_ipc::ErrorCode::MalformedMessage,
            "message kind is not valid from a GUI client");
    }
    return false;
}

bool GuiIpcConnection::send_protocol_rejection(
    const gui_ipc::Envelope& envelope,
    gui_ipc::ErrorCode error,
    std::string detail) {
    gui_ipc::CommandStatus status;
    status.sequence = envelope.sequence;
    status.command = "ipc";
    status.outcome = gui_ipc::ResultCode::Rejected;
    status.error = error;
    status.detail = std::move(detail);
    return send_command_completion(
        envelope.request_id, status, envelope.base_revision);
}

gui_ipc::Snapshot GuiIpcConnection::current_snapshot() const {
    if (!callbacks_.snapshot_provider) return {};
    try {
        return callbacks_.snapshot_provider();
    } catch (...) {
        return {};
    }
}

gui_ipc::Envelope GuiIpcConnection::server_envelope(
    gui_ipc::MessageKind kind,
    std::string request_id) const {
    gui_ipc::Envelope envelope;
    envelope.kind = kind;
    envelope.request_id = std::move(request_id);
    return envelope;
}

bool GuiIpcConnection::publish_state(gui_ipc::Snapshot snapshot) {
    return authenticated_.load() && outbound_.publish_state(std::move(snapshot));
}

bool GuiIpcConnection::send_command_completion(
    const std::string& request_id,
    const gui_ipc::CommandStatus& status,
    std::string base_revision) {
    if (!authenticated_.load()) return false;
    std::string payload;
    std::string error;
    if (!gui_ipc::serialize_command_status(status, payload, error)) {
        event("command_status_failed", std::move(error));
        return false;
    }
    auto envelope = server_envelope(
        gui_ipc::MessageKind::CommandStatus, request_id);
    envelope.base_revision = std::move(base_revision);
    envelope.result = status.outcome;
    envelope.error_code = status.error;
    envelope.payload_json = std::move(payload);
    return outbound_.enqueue(std::move(envelope));
}

bool GuiIpcConnection::send_activate() {
    if (!authenticated_.load()) return false;
    return outbound_.enqueue(server_envelope(
        gui_ipc::MessageKind::Activate, "activate"));
}

bool GuiIpcConnection::send_shutdown() {
    if (!authenticated_.load()) return false;
    return outbound_.enqueue(server_envelope(
        gui_ipc::MessageKind::Shutdown, "shutdown"));
}

bool GuiIpcConnection::authenticated() const noexcept {
    return authenticated_.load();
}

std::uint64_t GuiIpcConnection::client_process_id() const noexcept {
    return client_process_id_;
}

GuiOutboundStatus GuiIpcConnection::outbound_status() const {
    return outbound_.status();
}

std::uint64_t GuiIpcConnection::next_server_sequence() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return session_.next_server_sequence();
}

void GuiIpcConnection::event(
    std::string_view name,
    std::string detail) const {
    try {
        if (!callbacks_.event_handler) return;
        callbacks_.event_handler(name, std::move(detail), client_process_id_);
    } catch (...) {}
}

} // namespace ccs

#endif
