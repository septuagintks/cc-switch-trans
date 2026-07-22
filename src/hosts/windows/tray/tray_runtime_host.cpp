#include "hosts/windows/tray_app.hpp"

#ifdef _WIN32

#include "hosts/windows/tray/tray_messages.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <utility>

namespace ccs {

namespace {

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) return L"Unknown error";
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    (void)MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required);
    return result;
}

} // namespace

void TrayApplication::post_command(Command command, bool startup_enabled) {
    if (exiting_) return;
    if (command == Command::Status) {
        if (status_pending_) return;
        status_pending_ = true;
    }
    if (command == Command::Start || command == Command::Stop
        || command == Command::Reload) {
        if (service_command_pending_) return;
        service_command_pending_ = true;
    }

    if (command != Command::Status) {
        log_host("info", "host_command_start", {
            field_string("command", command_name(command)),
        });
    }
    const bool posted = executor_.post([this, command, startup_enabled]() {
        auto result = std::make_unique<CommandResult>();
        result->command = command;
        try {
            switch (command) {
            case Command::Status:
                result->succeeded = true;
                break;
            case Command::Start:
                result->succeeded = controller_.start(result->error);
                break;
            case Command::Stop:
                result->succeeded = controller_.stop(result->error);
                break;
            case Command::Reload:
                result->succeeded = controller_.reload(result->error);
                break;
            case Command::OpenConfig:
                result->succeeded = platform_.open_config_file(
                    paths_, result->error);
                break;
            case Command::OpenLogs:
                result->succeeded = platform_.open_logs_directory(
                    paths_, result->error);
                break;
            case Command::SetStartup:
                result->succeeded = platform_.set_startup_registered(
                    executable_path_, startup_enabled, result->error);
                break;
            }
            result->status = controller_.status();
            std::string startup_error;
            result->startup_known = platform_.startup_registered(
                executable_path_, result->startup_enabled, startup_error);
            if (!result->startup_known
                && (command == Command::Status
                    || command == Command::SetStartup)) {
                if (result->error.empty()) {
                    result->error = std::move(startup_error);
                }
                result->succeeded = false;
            }
        } catch (const std::exception& exception) {
            result->succeeded = false;
            result->error = "host command failed: "
                + std::string(exception.what());
            result->status = controller_.status();
        } catch (...) {
            result->succeeded = false;
            result->error = "host command failed with an unknown exception";
            result->status = controller_.status();
        }
        auto* raw_result = result.release();
        if (!PostMessageW(
                window_,
                tray_messages::command_result,
                0,
                reinterpret_cast<LPARAM>(raw_result))) {
            delete raw_result;
        }
    });
    if (!posted) {
        if (command == Command::Status) status_pending_ = false;
        if (command == Command::Start || command == Command::Stop
            || command == Command::Reload) {
            service_command_pending_ = false;
        }
        log_host("error", "host_command_rejected", {
            field_string("command", command_name(command)),
        });
    }
}

void TrayApplication::handle_command_result(
    std::unique_ptr<CommandResult> result) {
    if (!result) return;
    if (result->command == Command::Status) status_pending_ = false;
    if (result->command == Command::Start
        || result->command == Command::Stop
        || result->command == Command::Reload) {
        service_command_pending_ = false;
    }

    const auto previous_state = cached_status_.state;
    cached_status_ = result->status;
    view_model_.refresh_application_status();
    startup_known_ = result->startup_known;
    startup_enabled_ = result->startup_enabled;

    if (result->command != Command::Status) {
        log_host(result->succeeded ? "info" : "error", "host_command_complete", {
            field_string("command", command_name(result->command)),
            field_bool("succeeded", result->succeeded),
            field_string("state", application_state_name(cached_status_.state)),
            field_string("error", result->error),
        });
    } else if (!result->succeeded && result->error != last_status_error_) {
        log_host("error", "host_status_failed", {
            field_string("error", result->error),
        });
        last_status_error_ = result->error;
    } else if (result->succeeded) {
        last_status_error_.clear();
    }
    if (previous_state != cached_status_.state) {
        log_host("info", "host_state_changed", {
            field_string(
                "previous_state", application_state_name(previous_state)),
            field_string(
                "state", application_state_name(cached_status_.state)),
            field_number("exit_code", cached_status_.last_exit_code),
            field_string("error", cached_status_.last_error),
        });
    }

    if (!result->succeeded && result->command != Command::Status) {
        const auto message = result->error.empty()
            ? L"The command failed. Open logs for details."
            : utf8_to_wide(result->error);
        show_notification(L"ccs-trans command failed", message, NIIF_ERROR);
    } else if (previous_state != ApplicationState::Faulted
        && cached_status_.state == ApplicationState::Faulted) {
        const auto message = cached_status_.last_error.empty()
            ? L"The service stopped unexpectedly. Open logs for details."
            : utf8_to_wide(cached_status_.last_error);
        show_notification(L"ccs-trans service stopped", message, NIIF_ERROR);
    }
}

void TrayApplication::begin_exit(const std::string& reason) {
    if (exiting_) return;
    exiting_ = true;
    maintenance_shutdown_requested_.store(true);
    if (gui_session_) {
        std::string gui_error;
        const bool gui_stopped = gui_session_->shutdown(
            std::chrono::seconds{2}, gui_error);
        log_host(gui_stopped ? "info" : "error", "gui_ipc_stop", {
            field_bool("succeeded", gui_stopped),
            field_string("error", gui_error),
        });
        gui_session_.reset();
        gui_command_router_.disconnect();
    }
    gui_shutdown_complete_.store(true);
    view_model_.set_update_handler({});
    view_model_.stop();
    KillTimer(window_, tray_messages::status_timer);
    tray_icon_.remove();
    log_host("info", "host_shutdown_start", {field_string("reason", reason)});
    if (!executor_.post([this]() {
            std::string error;
            const bool succeeded = controller_.shutdown(error);
            auto* result = new std::string(std::move(error));
            if (!PostMessageW(
                    window_,
                    tray_messages::shutdown_complete,
                    succeeded ? 1 : 0,
                    reinterpret_cast<LPARAM>(result))) {
                delete result;
            }
        })) {
        finish_exit(false, "control executor rejected shutdown");
    }
}

void TrayApplication::finish_exit(bool succeeded, std::string error) {
    shutdown_complete_ = true;
    exit_code_ = succeeded ? 0 : 1;
    if (maintenance_server_) {
        maintenance_server_->stop();
        maintenance_server_.reset();
        log_host("info", "maintenance_ipc_stop");
    }
    log_host(succeeded ? "info" : "error", "host_shutdown_complete", {
        field_bool("succeeded", succeeded),
        field_string("error", error),
    });
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

const char* TrayApplication::command_name(Command command) {
    switch (command) {
    case Command::Status: return "status";
    case Command::Start: return "start";
    case Command::Stop: return "stop";
    case Command::Reload: return "reload";
    case Command::OpenConfig: return "open_config";
    case Command::OpenLogs: return "open_logs";
    case Command::SetStartup: return "set_startup";
    }
    return "unknown";
}

} // namespace ccs

#endif
