#include "hosts/windows/maintenance/maintenance_ipc_server.hpp"

#ifdef _WIN32

#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"
#include "hosts/windows/platform/gui_pipe_security.hpp"
#include "hosts/windows/windows_error.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ccs {

namespace {

bool read_operation(
    HANDLE pipe,
    HANDLE event,
    std::uint8_t* buffer,
    DWORD capacity,
    DWORD& received,
    std::string& error) {
    OVERLAPPED operation{};
    operation.hEvent = event;
    (void)ResetEvent(event);
    if (ReadFile(pipe, buffer, capacity, &received, &operation)) return true;
    auto code = GetLastError();
    if (code == ERROR_IO_PENDING
        && WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0
        && GetOverlappedResult(pipe, &operation, &received, FALSE)) {
        return true;
    }
    code = code == ERROR_IO_PENDING ? GetLastError() : code;
    error = windows_error_message(
        "failed to read the maintenance named pipe", code);
    return false;
}

bool read_single_frame(HANDLE pipe, std::string& frame, std::string& error) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        error = windows_error_message(
            "failed to create a maintenance read event", GetLastError());
        return false;
    }
    gui_ipc::FrameDecoder decoder;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    std::vector<std::string> frames;
    while (frames.empty()) {
        DWORD received = 0;
        if (!read_operation(
                pipe,
                event,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                received,
                error)) {
            CloseHandle(event);
            return false;
        }
        if (received == 0) {
            error = "maintenance client closed before sending a request";
            CloseHandle(event);
            return false;
        }
        gui_ipc::FrameError frame_error;
        if (!decoder.consume(
                std::span<const std::uint8_t>(buffer.data(), received),
                frames,
                frame_error)) {
            error = "maintenance frame rejected: "
                + std::string(gui_ipc::frame_error_name(frame_error));
            CloseHandle(event);
            return false;
        }
    }
    CloseHandle(event);
    if (frames.size() != 1 || decoder.buffered_bytes() != 0) {
        error = "maintenance connections accept exactly one request frame";
        return false;
    }
    frame = std::move(frames.front());
    return true;
}

bool write_all(
    HANDLE pipe,
    const std::vector<std::uint8_t>& frame,
    std::string& error) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        error = windows_error_message(
            "failed to create a maintenance write event", GetLastError());
        return false;
    }
    std::size_t offset = 0;
    while (offset < frame.size()) {
        const auto remaining = frame.size() - offset;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            remaining, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        OVERLAPPED operation{};
        operation.hEvent = event;
        (void)ResetEvent(event);
        bool succeeded = WriteFile(
            pipe,
            frame.data() + offset,
            chunk,
            &written,
            &operation) != FALSE;
        auto code = succeeded ? ERROR_SUCCESS : GetLastError();
        if (!succeeded && code == ERROR_IO_PENDING
            && WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0) {
            succeeded = GetOverlappedResult(
                pipe, &operation, &written, FALSE) != FALSE;
            code = succeeded ? ERROR_SUCCESS : GetLastError();
        }
        if (!succeeded || written == 0) {
            error = succeeded
                ? "maintenance named-pipe write returned zero bytes"
                : windows_error_message(
                    "failed to write the maintenance named pipe", code);
            CloseHandle(event);
            return false;
        }
        offset += written;
    }
    CloseHandle(event);
    return true;
}

} // namespace

class MaintenanceIpcServer::Impl {
public:
    Impl(
        MaintenanceIpcServerConfig config,
        MaintenanceIpcServerCallbacks callbacks)
        : config_(std::move(config)), callbacks_(std::move(callbacks)) {}

    ~Impl() { stop(); }

    bool start(std::string& error) {
        error.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            error = "maintenance IPC server is already running";
            return false;
        }
        if (config_.pipe_name.empty() || config_.current_user_sid.empty()
            || config_.version.empty() || config_.source_commit.empty()) {
            error = "maintenance IPC server configuration is incomplete";
            return false;
        }
        if (!security_.initialize(config_.current_user_sid, error)) return false;
        HANDLE first_pipe = create_pipe(error);
        if (first_pipe == INVALID_HANDLE_VALUE) return false;
        stopping_.store(false);
        running_ = true;
        active_pipe_ = first_pipe;
        try {
            thread_ = std::thread([this, first_pipe] { run(first_pipe); });
        } catch (const std::exception& exception) {
            CloseHandle(first_pipe);
            active_pipe_ = INVALID_HANDLE_VALUE;
            running_ = false;
            error = "failed to start maintenance IPC thread: "
                + std::string(exception.what());
            return false;
        }
        return true;
    }

    void stop() noexcept {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ && !thread_.joinable()) return;
            stopping_.store(true);
            pipe = active_pipe_;
        }
        if (pipe != INVALID_HANDLE_VALUE) (void)CancelIoEx(pipe, nullptr);
        if (thread_.joinable()) {
            (void)CancelSynchronousIo(
                reinterpret_cast<HANDLE>(thread_.native_handle()));
            HANDLE wake = CreateFileW(
                config_.pipe_name.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                nullptr);
            if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
            thread_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        connected_ = false;
        active_pipe_ = INVALID_HANDLE_VALUE;
    }

    MaintenanceIpcServerStatus status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = status_;
        result.running = running_;
        result.connected = connected_;
        return result;
    }

    const std::wstring& pipe_name() const noexcept { return config_.pipe_name; }

private:
    HANDLE create_pipe(std::string& error) {
        const HANDLE pipe = CreateNamedPipeW(
            config_.pipe_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE
                | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            64U * 1024U,
            64U * 1024U,
            0,
            security_.attributes());
        if (pipe == INVALID_HANDLE_VALUE) {
            error = windows_error_message(
                "failed to create the maintenance named pipe", GetLastError());
        }
        return pipe;
    }

    bool connect(HANDLE pipe) {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (operation.hEvent == nullptr) return false;
        bool connected = ConnectNamedPipe(pipe, &operation) != FALSE;
        const auto code = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && code == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (!connected && code == ERROR_IO_PENDING
            && WaitForSingleObject(operation.hEvent, INFINITE) == WAIT_OBJECT_0) {
            DWORD transferred = 0;
            connected = GetOverlappedResult(
                pipe, &operation, &transferred, FALSE) != FALSE;
        }
        CloseHandle(operation.hEvent);
        return connected;
    }

    bool verify_client(
        HANDLE pipe,
        std::uint64_t& process_id,
        std::string& error) {
        ULONG native_process_id = 0;
        if (!GetNamedPipeClientProcessId(pipe, &native_process_id)) {
            error = windows_error_message(
                "failed to identify the maintenance client", GetLastError());
            return false;
        }
        std::wstring sid;
        if (!process_user_sid(native_process_id, sid, error)) return false;
        if (_wcsicmp(sid.c_str(), config_.current_user_sid.c_str()) != 0) {
            error = "maintenance client belongs to a different Windows user";
            return false;
        }
        process_id = native_process_id;
        return true;
    }

    void close_active_pipe(HANDLE pipe) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_pipe_ == pipe) active_pipe_ = INVALID_HANDLE_VALUE;
        }
        CloseHandle(pipe);
    }

    bool handle_request(
        HANDLE pipe,
        std::uint64_t process_id,
        std::string& error) {
        std::string frame;
        if (!read_single_frame(pipe, frame, error)) return false;
        gui_ipc::Envelope request_envelope;
        if (!gui_ipc::parse_envelope(frame, request_envelope, error)
            || request_envelope.kind
                != gui_ipc::MessageKind::MaintenanceRequest) {
            if (error.empty()) {
                error = "maintenance endpoint rejected a non-maintenance request";
            }
            return false;
        }
        gui_ipc::MaintenanceRequest request;
        if (!gui_ipc::parse_maintenance_request(
                request_envelope.payload_json, request, error)) {
            return false;
        }

        gui_ipc::MaintenanceResult result;
        try {
            if (callbacks_.request_handler) {
                result = callbacks_.request_handler(request);
            } else {
                result.detail = "maintenance request handler is unavailable";
            }
        } catch (const std::exception& exception) {
            result.detail = exception.what();
        } catch (...) {
            result.detail = "maintenance request handler failed";
        }
        if (result.version.empty()) result.version = config_.version;
        if (result.source_commit.empty()) {
            result.source_commit = config_.source_commit;
        }
        std::string payload;
        if (!gui_ipc::serialize_maintenance_result(result, payload, error)) {
            return false;
        }
        gui_ipc::Envelope response;
        response.protocol = std::string(gui_ipc::kMaintenanceProtocol);
        response.kind = gui_ipc::MessageKind::MaintenanceResult;
        response.request_id = request_envelope.request_id;
        response.session_id = request_envelope.session_id;
        response.sequence = 1;
        response.source_commit = config_.source_commit;
        response.result = result.succeeded
            ? gui_ipc::ResultCode::Succeeded : gui_ipc::ResultCode::Failed;
        response.error_code = result.succeeded
            ? gui_ipc::ErrorCode::None : gui_ipc::ErrorCode::ServiceUnavailable;
        response.payload_json = std::move(payload);
        std::string content;
        if (!gui_ipc::serialize_envelope(response, content, error)) return false;
        std::vector<std::uint8_t> encoded;
        gui_ipc::FrameError frame_error;
        if (!gui_ipc::encode_frame(content, encoded, frame_error)) {
            error = "failed to frame the maintenance response: "
                + std::string(gui_ipc::frame_error_name(frame_error));
            return false;
        }
        if (!write_all(pipe, encoded, error)) return false;
        emit("request_completed", gui_ipc::maintenance_command_name(
            request.command), process_id);
        return true;
    }

    void run(HANDLE first_pipe) noexcept {
        HANDLE pipe = first_pipe;
        while (!stopping_.load()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_pipe_ = pipe;
            }
            const bool connected = connect(pipe);
            if (stopping_.load()) {
                close_active_pipe(pipe);
                break;
            }
            if (!connected) {
                emit("accept_failed", windows_error_message(
                    "failed to accept a maintenance client", GetLastError()), 0);
            } else {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    connected_ = true;
                }
                std::uint64_t process_id = 0;
                std::string error;
                const bool accepted = verify_client(pipe, process_id, error)
                    && handle_request(pipe, process_id, error);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    connected_ = false;
                    if (accepted) ++status_.accepted_requests;
                    else ++status_.rejected_requests;
                }
                if (!accepted) emit("request_rejected", std::move(error), process_id);
                (void)DisconnectNamedPipe(pipe);
            }
            close_active_pipe(pipe);
            if (stopping_.load()) break;
            std::string error;
            pipe = create_pipe(error);
            if (pipe == INVALID_HANDLE_VALUE) {
                emit("listen_failed", std::move(error), 0);
                break;
            }
            if (stopping_.load()) {
                CloseHandle(pipe);
                break;
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        active_pipe_ = INVALID_HANDLE_VALUE;
        running_ = false;
        connected_ = false;
    }

    void emit(
        std::string_view event,
        std::string detail,
        std::uint64_t process_id) const {
        try {
            if (!callbacks_.event_handler) return;
            callbacks_.event_handler(event, std::move(detail), process_id);
        } catch (...) {}
    }

    MaintenanceIpcServerConfig config_;
    MaintenanceIpcServerCallbacks callbacks_;
    CurrentUserPipeSecurity security_;
    mutable std::mutex mutex_;
    HANDLE active_pipe_ = INVALID_HANDLE_VALUE;
    std::thread thread_;
    std::atomic_bool stopping_{false};
    bool running_ = false;
    bool connected_ = false;
    MaintenanceIpcServerStatus status_;
};

MaintenanceIpcServer::MaintenanceIpcServer(
    MaintenanceIpcServerConfig config,
    MaintenanceIpcServerCallbacks callbacks)
    : impl_(std::make_unique<Impl>(
        std::move(config), std::move(callbacks))) {}

MaintenanceIpcServer::~MaintenanceIpcServer() = default;
bool MaintenanceIpcServer::start(std::string& error) {
    return impl_->start(error);
}
void MaintenanceIpcServer::stop() noexcept { impl_->stop(); }
MaintenanceIpcServerStatus MaintenanceIpcServer::status() const {
    return impl_->status();
}
const std::wstring& MaintenanceIpcServer::pipe_name() const noexcept {
    return impl_->pipe_name();
}

} // namespace ccs

#endif
