#include "hosts/windows/tray/gui_session_controller.hpp"

#ifdef _WIN32

#include <utility>

namespace ccs {

GuiSessionController::GuiSessionController(
    GuiSessionControllerConfig config,
    GuiIpcServerCallbacks callbacks)
    : config_(std::move(config))
    , callbacks_(std::move(callbacks))
    , launcher_(config_.gui_executable) {}

GuiSessionController::~GuiSessionController() {
    std::string error;
    (void)shutdown(std::chrono::milliseconds{250}, error);
}

bool GuiSessionController::start(std::string& error) {
    error.clear();
    if (server_) {
        error = "GUI session controller is already started";
        return false;
    }
    if (config_.version.empty() || config_.source_commit.empty()
        || config_.instance_identity.empty()) {
        error = "GUI session controller configuration is incomplete";
        return false;
    }
    if (!make_gui_pipe_identity(
            config_.config_root,
            config_.instance_identity,
            identity_,
            error)) {
        return false;
    }
    auto server = std::make_unique<GuiIpcServer>(
        GuiIpcServerConfig{
            identity_.gui_pipe_name,
            identity_.current_user_sid,
            config_.version,
            config_.source_commit,
            config_.instance_identity,
            config_.outbound_queue_capacity,
        },
        callbacks_);
    if (!server->start(error)) return false;
    server_ = std::move(server);
    return true;
}

bool GuiSessionController::launch_or_activate(std::string& error) {
    error.clear();
    if (!server_ || !server_->status().running) {
        error = "GUI IPC server is not running";
        return false;
    }
    const auto ipc_status = server_->status();
    if (ipc_status.authenticated) {
        if (server_->request_activate()) return true;
        error = "failed to queue GUI activation";
        return false;
    }
    if (launcher_.running()) return true;

    std::string token;
    std::string session_id;
    if (!generate_gui_secret(token, error)
        || !generate_gui_secret(session_id, error)) {
        return false;
    }
    gui_ipc::LaunchBootstrap bootstrap{
        pipe_name_utf8(identity_.gui_pipe_name),
        config_.version,
        config_.source_commit,
        config_.instance_identity,
        token,
        session_id,
    };
    if (!launcher_.launch_suspended(bootstrap, error)) return false;
    if (!server_->prepare_session(
            {token, session_id, launcher_.process_id()}, error)
        || !launcher_.resume(error)) {
        std::string terminate_error;
        (void)launcher_.terminate(terminate_error);
        return false;
    }
    bool exited = false;
    DWORD exit_code = STILL_ACTIVE;
    if (!launcher_.wait_for_exit(
            std::chrono::milliseconds{150}, exited, exit_code, error)) {
        return false;
    }
    if (exited) {
        const auto diagnostic = std::move(error);
        error = "ccs-trans GUI process exited during startup with code "
            + std::to_string(exit_code);
        if (!diagnostic.empty()) error += ": " + diagnostic;
        return false;
    }
    return true;
}

bool GuiSessionController::publish_state(gui_ipc::Snapshot snapshot) {
    return server_ && server_->publish_state(std::move(snapshot));
}

bool GuiSessionController::send_command_completion(
    const std::string& request_id,
    const gui_ipc::CommandStatus& status,
    std::string base_revision) {
    return server_ && server_->send_command_completion(
        request_id, status, std::move(base_revision));
}

bool GuiSessionController::request_activate() {
    return server_ && server_->request_activate();
}

bool GuiSessionController::shutdown(
    std::chrono::milliseconds timeout,
    std::string& error) {
    error.clear();
    bool succeeded = true;
    if (server_ && server_->status().authenticated) {
        (void)server_->request_shutdown();
    }
    if (launcher_.process_id() != 0) {
        bool exited = false;
        DWORD exit_code = STILL_ACTIVE;
        if (!launcher_.wait_for_exit(timeout, exited, exit_code, error)) {
            succeeded = false;
        } else if (!exited) {
            std::string terminate_error;
            (void)launcher_.terminate(terminate_error);
            error = "ccs-trans GUI process did not exit before the timeout";
            if (!terminate_error.empty()) error += ": " + terminate_error;
            succeeded = false;
        } else if (exit_code != 0) {
            const auto diagnostic = std::move(error);
            error = "ccs-trans GUI process exited with code "
                + std::to_string(exit_code);
            if (!diagnostic.empty()) error += ": " + diagnostic;
            succeeded = false;
        }
    }
    if (server_) {
        server_->stop();
        server_.reset();
    }
    return succeeded;
}

GuiSessionControllerStatus GuiSessionController::status() const {
    GuiSessionControllerStatus result;
    if (server_) result.ipc = server_->status();
    result.process_running = launcher_.running();
    result.process_suspended = launcher_.suspended();
    result.process_id = launcher_.process_id();
    return result;
}

const GuiPipeIdentity& GuiSessionController::identity() const noexcept {
    return identity_;
}

std::string GuiSessionController::pipe_name_utf8(
    const std::wstring& pipe_name) {
    return std::string(pipe_name.begin(), pipe_name.end());
}

} // namespace ccs

#endif
