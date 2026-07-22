#include "hosts/windows/gui_bridge/gui_ipc_server.hpp"

#ifdef _WIN32

#include "hosts/windows/gui_bridge/gui_ipc_connection.hpp"
#include "hosts/windows/platform/gui_pipe_security.hpp"
#include "hosts/windows/windows_error.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace ccs {

class GuiIpcServer::Impl {
public:
    Impl(GuiIpcServerConfig config, GuiIpcServerCallbacks callbacks)
        : config_(std::move(config)), callbacks_(std::move(callbacks)) {}

    ~Impl() { stop(); }

    bool start(std::string& error) {
        error.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            error = "GUI IPC server is already running";
            return false;
        }
        if (config_.pipe_name.empty() || config_.current_user_sid.empty()
            || config_.version.empty() || config_.source_commit.empty()
            || config_.instance_identity.empty()) {
            error = "GUI IPC server configuration is incomplete";
            return false;
        }
        if (!security_.initialize(config_.current_user_sid, error)) return false;
        HANDLE first_pipe = create_pipe(error);
        if (first_pipe == INVALID_HANDLE_VALUE) return false;
        stopping_.store(false);
        running_ = true;
        active_pipe_ = first_pipe;
        try {
            accept_thread_ = std::thread([this, first_pipe] {
                accept_loop(first_pipe);
            });
        } catch (const std::exception& exception) {
            CloseHandle(first_pipe);
            active_pipe_ = INVALID_HANDLE_VALUE;
            running_ = false;
            error = "failed to start GUI IPC accept thread: "
                + std::string(exception.what());
            return false;
        }
        return true;
    }

    bool prepare_session(GuiSessionCredentials credentials, std::string& error) {
        error.clear();
        if (credentials.handshake_token.empty() || credentials.session_id.empty()) {
            error = "GUI session token and session id must not be empty";
            return false;
        }
        if (credentials.expected_process_id && *credentials.expected_process_id == 0) {
            error = "expected GUI process id must be positive";
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || stopping_.load()) {
            error = "GUI IPC server is not running";
            return false;
        }
        if (physical_client_connected_ || active_connection_) {
            error = "cannot replace GUI session credentials while a client is connected";
            return false;
        }
        credentials_ = std::move(credentials);
        return true;
    }

    bool publish_state(gui_ipc::Snapshot snapshot) {
        auto connection = active_connection();
        return connection && connection->publish_state(std::move(snapshot));
    }

    bool send_command_completion(
        const std::string& request_id,
        const gui_ipc::CommandStatus& status,
        std::string base_revision) {
        auto connection = active_connection();
        if (!connection) return false;
        if (connection->send_command_completion(
                request_id, status, std::move(base_revision))) {
            return true;
        }
        emit_event(
            "outbound_rejected", "command_status", connection->client_process_id());
        connection->stop();
        return false;
    }

    bool request_activate() {
        auto connection = active_connection();
        if (!connection) return false;
        if (connection->send_activate()) return true;
        emit_event(
            "outbound_rejected", "activate", connection->client_process_id());
        connection->stop();
        return false;
    }

    bool request_shutdown() {
        auto connection = active_connection();
        if (!connection) return false;
        if (connection->send_shutdown()) return true;
        emit_event(
            "outbound_rejected", "shutdown", connection->client_process_id());
        connection->stop();
        return false;
    }

    void stop() noexcept {
        std::shared_ptr<GuiIpcConnection> connection;
        HANDLE pipe = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ && !accept_thread_.joinable()) return;
            stopping_.store(true);
            connection = active_connection_;
            pipe = active_pipe_;
        }
        if (connection) connection->stop();
        if (pipe != INVALID_HANDLE_VALUE) (void)CancelIoEx(pipe, nullptr);
        if (accept_thread_.joinable()) {
            (void)CancelSynchronousIo(
                reinterpret_cast<HANDLE>(accept_thread_.native_handle()));
            HANDLE wake = CreateFileW(
                config_.pipe_name.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                nullptr);
            if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
            accept_thread_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        active_pipe_ = INVALID_HANDLE_VALUE;
        active_connection_.reset();
        credentials_.reset();
    }

    GuiIpcServerStatus status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = status_;
        result.running = running_;
        result.connected = physical_client_connected_;
        result.authenticated = active_connection_
            && active_connection_->authenticated();
        result.client_process_id = active_connection_
            ? active_connection_->client_process_id() : 0;
        if (active_connection_) {
            const auto outbound = active_connection_->outbound_status();
            result.outbound_rejected += outbound.rejected;
            result.outbound_coalesced += outbound.coalesced;
        }
        return result;
    }

    const std::wstring& pipe_name() const noexcept { return config_.pipe_name; }

private:
    HANDLE create_pipe(std::string& error) {
        const HANDLE pipe = CreateNamedPipeW(
            config_.pipe_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            64U * 1024U,
            64U * 1024U,
            0,
            security_.attributes());
        if (pipe == INVALID_HANDLE_VALUE) {
            error = windows_error_message(
                "failed to create current-user GUI named pipe", GetLastError());
        }
        return pipe;
    }

    std::shared_ptr<GuiIpcConnection> active_connection() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_connection_;
    }

    void close_active_pipe(HANDLE pipe) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_pipe_ == pipe) active_pipe_ = INVALID_HANDLE_VALUE;
        }
        CloseHandle(pipe);
    }

    bool verify_client(HANDLE pipe, std::uint64_t& process_id, std::string& error) {
        ULONG client_process_id = 0;
        if (!GetNamedPipeClientProcessId(pipe, &client_process_id)) {
            error = windows_error_message(
                "failed to identify the GUI named-pipe client", GetLastError());
            return false;
        }
        std::wstring client_sid;
        if (!process_user_sid(client_process_id, client_sid, error)) return false;
        if (_wcsicmp(client_sid.c_str(), config_.current_user_sid.c_str()) != 0) {
            error = "GUI named-pipe client belongs to a different Windows user";
            return false;
        }
        process_id = client_process_id;
        return true;
    }

    void accept_loop(HANDLE first_pipe) noexcept {
        HANDLE pipe = first_pipe;
        while (!stopping_.load()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_pipe_ = pipe;
            }
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            bool connected = false;
            if (operation.hEvent != nullptr) {
                connected = ConnectNamedPipe(pipe, &operation) != FALSE;
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
            }
            if (stopping_.load()) {
                close_active_pipe(pipe);
                break;
            }
            if (!connected) {
                emit_event("accept_failed", windows_error_message(
                    "failed to accept GUI named-pipe client", GetLastError()), 0);
                close_active_pipe(pipe);
            } else {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    physical_client_connected_ = true;
                }
                handle_client(pipe);
                (void)DisconnectNamedPipe(pipe);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    physical_client_connected_ = false;
                }
                close_active_pipe(pipe);
            }
            if (stopping_.load()) break;
            std::string error;
            pipe = create_pipe(error);
            if (pipe == INVALID_HANDLE_VALUE) {
                emit_event("listen_failed", std::move(error), 0);
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
    }

    void handle_client(HANDLE pipe) {
        std::uint64_t process_id = 0;
        std::string error;
        if (!verify_client(pipe, process_id, error)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.rejected_connections;
            }
            emit_event("client_rejected", std::move(error), process_id);
            return;
        }
        GuiSessionCredentials credentials;
        bool session_available = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_available = credentials_.has_value();
            if (!session_available) {
                ++status_.rejected_connections;
            } else {
                credentials = *credentials_;
            }
        }
        if (!session_available) {
            emit_event(
                "client_rejected", "no GUI launch session is pending", process_id);
            return;
        }
        gui_ipc::ServerSessionPolicy policy{
            config_.version,
            config_.source_commit,
            config_.instance_identity,
            credentials.handshake_token,
            credentials.session_id,
            credentials.expected_process_id,
        };
        auto callbacks = callbacks_;
        const auto original_event = callbacks.event_handler;
        callbacks.event_handler = [this, original_event](
            std::string_view event, std::string detail, std::uint64_t pid) {
            record_connection_event(event);
            if (original_event) original_event(event, std::move(detail), pid);
        };
        auto connection = std::make_shared<GuiIpcConnection>(
            pipe, process_id, std::move(policy), std::move(callbacks),
            config_.outbound_queue_capacity);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_connection_ = connection;
        }
        connection->run();
        const auto outbound = connection->outbound_status();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.outbound_rejected += outbound.rejected;
            status_.outbound_coalesced += outbound.coalesced;
            active_connection_.reset();
            credentials_.reset();
        }
    }

    void record_connection_event(std::string_view event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (event == "authenticated") {
            ++status_.accepted_connections;
        } else if (event == "hello_rejected") {
            ++status_.rejected_connections;
        } else if (event == "disconnected") {
            ++status_.disconnects;
        }
    }

    void emit_event(
        std::string_view event,
        std::string detail,
        std::uint64_t process_id) const {
        try {
            if (!callbacks_.event_handler) return;
            callbacks_.event_handler(event, std::move(detail), process_id);
        } catch (...) {}
    }

    GuiIpcServerConfig config_;
    GuiIpcServerCallbacks callbacks_;
    CurrentUserPipeSecurity security_;
    mutable std::mutex mutex_;
    std::optional<GuiSessionCredentials> credentials_;
    std::shared_ptr<GuiIpcConnection> active_connection_;
    HANDLE active_pipe_ = INVALID_HANDLE_VALUE;
    std::thread accept_thread_;
    std::atomic_bool stopping_{false};
    bool running_ = false;
    bool physical_client_connected_ = false;
    GuiIpcServerStatus status_;
};

GuiIpcServer::GuiIpcServer(
    GuiIpcServerConfig config,
    GuiIpcServerCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(callbacks))) {}

GuiIpcServer::~GuiIpcServer() = default;

bool GuiIpcServer::start(std::string& error) { return impl_->start(error); }

bool GuiIpcServer::prepare_session(
    GuiSessionCredentials credentials,
    std::string& error) {
    return impl_->prepare_session(std::move(credentials), error);
}

bool GuiIpcServer::publish_state(gui_ipc::Snapshot snapshot) {
    return impl_->publish_state(std::move(snapshot));
}

bool GuiIpcServer::send_command_completion(
    const std::string& request_id,
    const gui_ipc::CommandStatus& status,
    std::string base_revision) {
    return impl_->send_command_completion(
        request_id, status, std::move(base_revision));
}

bool GuiIpcServer::request_activate() { return impl_->request_activate(); }
bool GuiIpcServer::request_shutdown() { return impl_->request_shutdown(); }
void GuiIpcServer::stop() noexcept { impl_->stop(); }
GuiIpcServerStatus GuiIpcServer::status() const { return impl_->status(); }
const std::wstring& GuiIpcServer::pipe_name() const noexcept { return impl_->pipe_name(); }

} // namespace ccs

#endif
