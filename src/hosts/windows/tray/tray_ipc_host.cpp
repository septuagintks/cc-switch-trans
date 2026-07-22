#include "hosts/windows/tray_app.hpp"

#ifdef _WIN32

#include "core/version.hpp"
#include "hosts/windows/gui_bridge/gui_snapshot_builder.hpp"
#include "hosts/windows/tray/tray_messages.hpp"

#include <chrono>
#include <string_view>
#include <utility>

namespace ccs {

namespace {

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required,
            nullptr,
            nullptr) != required) {
        return {};
    }
    return result;
}

} // namespace

bool TrayApplication::initialize_gui_session(std::string& error) {
    GuiIpcServerCallbacks callbacks;
    callbacks.snapshot_provider = [this] {
        const auto state = view_model_.snapshot();
        return state ? build_gui_snapshot(*state) : gui_ipc::Snapshot{};
    };
    callbacks.command_handler = [this](
        const gui_ipc::Envelope& envelope,
        const gui_ipc::Command& command) {
        handle_gui_command(envelope, command);
    };
    callbacks.event_handler = [this](
        std::string_view event,
        std::string detail,
        std::uint64_t process_id) {
        handle_gui_ipc_event(event, std::move(detail), process_id);
    };
    auto instance_identity = wide_to_utf8(window_class_);
    if (instance_identity.empty()) {
        error = "failed to encode the tray instance identity";
        return false;
    }
    gui_session_ = std::make_unique<GuiSessionController>(
        GuiSessionControllerConfig{
            paths_.root,
            sibling_gui_executable(executable_path_),
            kVersion,
            kSourceCommit,
            std::move(instance_identity),
        },
        std::move(callbacks));
    if (!gui_session_->start(error)) {
        gui_session_.reset();
        return false;
    }

    MaintenanceIpcServerCallbacks maintenance_callbacks;
    maintenance_callbacks.request_handler = [this](
        const gui_ipc::MaintenanceRequest& request) {
        return handle_maintenance_request(request);
    };
    maintenance_callbacks.event_handler = [this](
        std::string_view event,
        std::string detail,
        std::uint64_t process_id) {
        handle_maintenance_ipc_event(event, std::move(detail), process_id);
    };
    maintenance_server_ = std::make_unique<MaintenanceIpcServer>(
        MaintenanceIpcServerConfig{
            gui_session_->identity().maintenance_pipe_name,
            gui_session_->identity().current_user_sid,
            kVersion,
            kSourceCommit,
        },
        std::move(maintenance_callbacks));
    if (!maintenance_server_->start(error)) {
        maintenance_server_.reset();
        std::string stop_error;
        (void)gui_session_->shutdown(
            std::chrono::milliseconds{250}, stop_error);
        gui_session_.reset();
        return false;
    }
    log_host("info", "gui_ipc_start", {
        field_string("identity_hash", gui_session_->identity().identity_hash),
    });
    log_host("info", "maintenance_ipc_start");
    return true;
}

void TrayApplication::handle_gui_command(
    const gui_ipc::Envelope& envelope,
    const gui_ipc::Command& command) {
    if (command.command == gui_ipc::GuiCommand::QuitApplication) {
        gui_ipc::CommandStatus status;
        status.sequence = envelope.sequence;
        status.command = gui_ipc::gui_command_name(command.command);
        status.outcome = gui_ipc::ResultCode::Accepted;
        if (gui_session_ && gui_session_->send_command_completion(
                envelope.request_id, status, envelope.base_revision)) {
            (void)PostMessageW(window_, tray_messages::gui_shutdown, 0, 0);
        } else {
            log_host("error", "gui_command_result_delivery_failed", {
                field_string("request_id", envelope.request_id),
            });
        }
        return;
    }
    const auto submission = gui_command_router_.submit(envelope, command);
    if (!submission.immediate) return;
    if (!gui_session_ || !gui_session_->send_command_completion(
            submission.immediate->request_id,
            submission.immediate->status,
            envelope.base_revision)) {
        log_host("error", "gui_command_result_delivery_failed", {
            field_string("request_id", envelope.request_id),
        });
    }
}

void TrayApplication::handle_gui_ipc_event(
    std::string_view event,
    std::string detail,
    std::uint64_t process_id) {
    if (event == "disconnected" || event == "hello_rejected") {
        gui_command_router_.disconnect();
    }
    const bool failure = event.find("failed") != std::string_view::npos
        || event.find("rejected") != std::string_view::npos;
    log_host(failure ? "error" : "info", "gui_ipc_event", {
        field_string("action", std::string(event)),
        field_string("detail", detail),
        field_number("process_id", static_cast<long long>(process_id)),
    });
}

gui_ipc::MaintenanceResult TrayApplication::handle_maintenance_request(
    const gui_ipc::MaintenanceRequest& request) {
    gui_ipc::MaintenanceResult result;
    result.version = kVersion;
    result.source_commit = kSourceCommit;
    switch (request.command) {
    case gui_ipc::MaintenanceCommand::QueryVersion:
        result.succeeded = true;
        result.state = application_state_name(controller_.status().state);
        return result;
    case gui_ipc::MaintenanceCommand::RequestShutdown:
        if (maintenance_shutdown_requested_.exchange(true)) {
            result.state = "shutdown_in_progress";
            result.detail = "the tray is already shutting down";
            return result;
        }
        result.succeeded = true;
        result.state = "shutdown_requested";
        result.detail = "shutdown will begin after this response is delivered";
        return result;
    case gui_ipc::MaintenanceCommand::WaitForRelease: {
        const auto application = controller_.status();
        result.succeeded = exiting_.load() && gui_shutdown_complete_.load()
            && (application.state == ApplicationState::Stopped
                || application.state == ApplicationState::Shutdown);
        result.state = result.succeeded ? "release_ready" : "draining";
        result.detail = result.succeeded
            ? "runtime and GUI handles are released"
            : "runtime shutdown is still in progress";
        return result;
    }
    }
    result.state = "unsupported";
    result.detail = "unknown maintenance command";
    return result;
}

void TrayApplication::handle_maintenance_ipc_event(
    std::string_view event,
    std::string detail,
    std::uint64_t process_id) {
    const bool failure = event.find("failed") != std::string_view::npos
        || event.find("rejected") != std::string_view::npos;
    log_host(failure ? "error" : "info", "maintenance_ipc_event", {
        field_string("action", std::string(event)),
        field_string("detail", detail),
        field_number("process_id", static_cast<long long>(process_id)),
    });
    if (event == "request_completed" && detail == "request_shutdown"
        && window_ != nullptr) {
        (void)PostMessageW(
            window_, tray_messages::maintenance_shutdown, 0, 0);
    }
}

} // namespace ccs

#endif
