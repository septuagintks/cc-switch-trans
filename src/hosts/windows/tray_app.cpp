#include "hosts/windows/tray_app.hpp"

#ifdef _WIN32

#include "hosts/windows/gui_bridge/gui_snapshot_builder.hpp"
#include "hosts/windows/instance_coordinator.hpp"
#include "hosts/windows/resource_ids.hpp"
#include "hosts/windows/tray/tray_menu.hpp"
#include "hosts/windows/tray/tray_messages.hpp"
#include "hosts/windows/windows_error.hpp"

#include <windowsx.h>

#include <chrono>
#include <memory>
#include <string_view>
#include <utility>

namespace ccs {

namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kViewCallbackMessage = WM_APP + 4;
constexpr UINT kStatusTimerIntervalMs = 1000;
constexpr UINT kTrayIconId = 1;

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return L"Unknown error";
    }
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

void debug_output(const std::string& message) {
    const auto wide = utf8_to_wide(message + "\n");
    OutputDebugStringW(wide.c_str());
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()),
        reinterpret_cast<const char*>(value.data() + value.size()));
}

} // namespace

TrayApplication::TrayApplication(
    HINSTANCE instance,
    AppPaths paths,
    std::filesystem::path executable_path,
    std::wstring window_class,
    std::wstring window_title)
    : instance_(instance)
    , paths_(std::move(paths))
    , executable_path_(std::move(executable_path))
    , window_class_(std::move(window_class))
    , window_title_(std::move(window_title))
    , controller_(paths_)
    , config_repository_(paths_)
    , config_editing_(config_repository_)
    , ui_preferences_(paths_)
    , view_model_(
          config_repository_,
          config_editing_,
          controller_,
          ui_preferences_,
          [this](std::function<void()> callback) {
              dispatch_view_callback(std::move(callback));
          },
          &executor_)
    , gui_command_router_(view_model_) {}

TrayApplication::~TrayApplication() {
    if (maintenance_server_) {
        maintenance_server_->stop();
        maintenance_server_.reset();
    }
    if (gui_session_) {
        std::string error;
        (void)gui_session_->shutdown(std::chrono::milliseconds{250}, error);
        gui_session_.reset();
    }
    view_model_.set_update_handler({});
    view_model_.stop();
    executor_.stop();
    if (!shutdown_complete_) {
        std::string error;
        (void)controller_.shutdown(error);
    }
    tray_icon_.remove();
    if (icon_ != nullptr) {
        DestroyIcon(icon_);
    }
    if (logger_) {
        std::string error;
        if (!logger_->drain(error)) {
            debug_output(error);
        }
    }
}

int TrayApplication::run() {
    std::string error;
    if (!initialize(error)) {
        MessageBoxW(
            nullptr,
            utf8_to_wide(error).c_str(),
            L"ccs-trans tray startup failed",
            MB_OK | MB_ICONERROR | MB_TASKMODAL);
        return 1;
    }

    MSG message{};
    int result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result < 0) {
        log_host("error", "host_message_loop_failed", {
            field_number("windows_error", static_cast<long long>(GetLastError())),
        });
        return 1;
    }
    return exit_code_;
}

bool TrayApplication::initialize(std::string& error) {
    if (!ensure_app_directories(paths_, error)) {
        return false;
    }
    std::string logger_error;
    if (!initialize_host_logger(logger_error)) {
        debug_output(logger_error);
        MessageBoxW(
            nullptr,
            utf8_to_wide(logger_error).c_str(),
            L"ccs-trans host logging unavailable",
            MB_OK | MB_ICONWARNING | MB_TASKMODAL);
    }
    wchar_t test_mode[2]{};
    tray_icon_enabled_ = GetEnvironmentVariableW(
        L"CCS_TRANS_TRAY_TEST_NO_ICON", test_mode, ARRAYSIZE(test_mode)) == 0
        || test_mode[0] != L'1';
    if (!register_window(error)
        || !tray_icon_.add(
            window_,
            icon_,
            kTrayCallbackMessage,
            kTrayIconId,
            tray_icon_enabled_,
            error)) {
        log_host("error", "host_start_failed", {field_string("error", error)});
        return false;
    }
    if (!tray_icon_enabled_) {
        log_host("info", "tray_icon_skipped_for_integration_test");
    }

    view_model_.set_update_handler([this](MainWindowStateSnapshot state) {
        handle_view_state(std::move(state));
    });
    if (!initialize_gui_session(error)) {
        log_host("error", "gui_ipc_start_failed", {field_string("error", error)});
        return false;
    }
    if (!view_model_.submit({MainWindowCommand::LoadDraft})) {
        log_host("error", "main_window_draft_load_rejected");
    }

    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (SetTimer(
            window_,
            tray_messages::status_timer,
            kStatusTimerIntervalMs,
            nullptr) == 0) {
        error = windows_error_message("failed to create tray status timer", GetLastError());
        return false;
    }
    log_host("info", "host_start", {
        field_string("executable", path_to_utf8(executable_path_)),
        field_string("config_root", path_to_utf8(paths_.root)),
    });
    post_command(Command::Status);
    post_command(Command::Start);
    return true;
}

bool TrayApplication::initialize_host_logger(std::string& error) {
    LoggerConfig config;
    config.path = paths_.host_log_file;
    config.level = "info";
    config.queue_capacity = 1024 * 1024;
    config.max_total_size = 64ULL * 1024 * 1024;
    config.flush_interval_ms = 100;
    logger_ = std::make_unique<Logger>(
        std::move(config),
        nullptr,
        nullptr,
        [](const std::string& failure) { debug_output("host logger failed: " + failure); });
    if (!logger_->open(error)) {
        logger_.reset();
        return false;
    }
    return true;
}

bool TrayApplication::register_window(std::string& error) {
    icon_ = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_CCS_TRANS), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    if (icon_ == nullptr) {
        error = windows_error_message("failed to load tray icon resource", GetLastError());
        return false;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &TrayApplication::window_proc;
    window_class.hInstance = instance_;
    window_class.hIcon = icon_;
    window_class.hIconSm = icon_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = window_class_.c_str();
    if (RegisterClassExW(&window_class) == 0) {
        error = windows_error_message("failed to register tray window class", GetLastError());
        return false;
    }

    window_ = CreateWindowExW(
        0,
        window_class_.c_str(),
        window_title_.c_str(),
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr) {
        error = windows_error_message("failed to create tray window", GetLastError());
        return false;
    }
    return true;
}

void TrayApplication::show_menu() {
    if (exiting_) return;
    const TrayMenuState state{
        cached_status_,
        service_command_pending_,
        view_state_ && view_state_->revision > 0 && initial_draft_load_complete_,
        view_state_ && view_state_->command_pending,
        !view_state_ || view_state_->lightweight_mode,
        startup_known_,
        startup_enabled_,
    };
    UINT selected = 0;
    std::string error;
    if (!show_tray_menu(window_, state, selected, error)) {
        log_host("error", "tray_menu_failed", {field_string("error", error)});
        return;
    }
    if (selected != 0) {
        (void)PostMessageW(window_, WM_COMMAND, MAKEWPARAM(selected, 0), 0);
    }
}

void TrayApplication::show_main_window() {
    if (exiting_) return;
    if (!view_state_ || view_state_->revision == 0
        || !initial_draft_load_complete_) {
        if (!gui_open_pending_) log_host("info", "gui_open_deferred");
        gui_open_pending_ = true;
        return;
    }
    gui_open_pending_ = false;
    std::string error;
    if (!gui_session_ || !gui_session_->launch_or_activate(error)) {
        log_host("error", "gui_launch_failed", {field_string("error", error)});
        show_notification(
            L"ccs-trans window failed",
            utf8_to_wide(error),
            NIIF_ERROR);
    }
}

void TrayApplication::set_lightweight_mode(bool enabled) {
    if (exiting_) {
        return;
    }
    if (!view_model_.submit({
            MainWindowCommand::SetLightweightMode, {}, {}, enabled})) {
        show_notification(
            L"ccs-trans command busy",
            L"Wait for the current command to finish.",
            NIIF_WARNING);
    }
}

void TrayApplication::request_exit(const std::string& reason, bool force) {
    (void)force;
    if (exiting_) {
        return;
    }
    begin_exit(reason);
}

void TrayApplication::dispatch_view_callback(std::function<void()> callback) {
    auto* posted = new std::function<void()>(std::move(callback));
    if (window_ == nullptr
        || !PostMessageW(
            window_, kViewCallbackMessage, 0, reinterpret_cast<LPARAM>(posted))) {
        delete posted;
    }
}

void TrayApplication::handle_view_state(MainWindowStateSnapshot state) {
    if (!state) {
        return;
    }
    const auto previous_status = cached_status_;
    view_state_ = std::move(state);
    cached_status_ = view_state_->application;
    if (!view_state_->command_pending && view_state_->last_command
        && view_state_->last_command->command == MainWindowCommand::LoadDraft) {
        initial_draft_load_complete_ = true;
    }
    if (gui_session_) {
        if (const auto completion = gui_command_router_.observe(*view_state_)) {
            if (!gui_session_->send_command_completion(
                    completion->request_id,
                    completion->status,
                    view_state_->draft.base_revision)) {
                log_host("error", "gui_command_result_delivery_failed", {
                    field_string("request_id", completion->request_id),
                });
            }
        }
        if (gui_session_->status().ipc.authenticated
            && !gui_session_->publish_state(build_gui_snapshot(*view_state_))) {
            log_host("error", "gui_state_delivery_failed", {
                field_number(
                    "revision", static_cast<long long>(view_state_->revision)),
            });
        }
    }

    if (view_state_->last_command
        && !view_state_->command_pending
        && view_state_->last_command->sequence > last_view_command_sequence_) {
        const auto& result = *view_state_->last_command;
        last_view_command_sequence_ = result.sequence;
        log_host(result.succeeded() || result.configuration_saved() ? "info" : "error",
            "main_window_command_complete", {
                field_string("command", main_window_command_name(result.command)),
                field_string("outcome", command_outcome_name(result.outcome)),
                field_string("error", main_window_error_name(result.error)),
                field_string("detail", result.detail),
            });
        if (!result.succeeded()
            && !result.configuration_saved()
            && result.error != MainWindowError::Cancelled) {
            show_notification(
                L"ccs-trans command failed",
                result.detail.empty() ? L"Open logs for details." : utf8_to_wide(result.detail),
                NIIF_ERROR);
        }
    }

    if (previous_status.state != cached_status_.state) {
        log_host("info", "host_state_changed", {
            field_string("previous_state", application_state_name(previous_status.state)),
            field_string("state", application_state_name(cached_status_.state)),
            field_number("exit_code", cached_status_.last_exit_code),
            field_string("error", cached_status_.last_error),
        });
    }
    if (gui_open_pending_ && view_state_->revision > 0
        && initial_draft_load_complete_) show_main_window();
}

void TrayApplication::show_notification(
    const std::wstring& title,
    const std::wstring& message,
    DWORD flags) {
    tray_icon_.show_notification(title, message, flags);
}

void TrayApplication::log_host(
    const std::string& level,
    const std::string& event,
    const std::vector<LogField>& fields) {
    if (!logger_ || !logger_->log(level, event, fields)) {
        debug_output(level + " " + event);
    }
}

LRESULT CALLBACK TrayApplication::window_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    TrayApplication* app = reinterpret_cast<TrayApplication*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        app = static_cast<TrayApplication*>(create->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app != nullptr) {
        return app->handle_message(message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT TrayApplication::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == taskbar_created_message_ && taskbar_created_message_ != 0) {
        tray_icon_.taskbar_recreated();
        std::string error;
        if (!tray_icon_.add(
                window_,
                icon_,
                kTrayCallbackMessage,
                kTrayIconId,
                tray_icon_enabled_,
                error)) {
            log_host("error", "tray_icon_restore_failed", {field_string("error", error)});
        } else if (tray_icon_enabled_) {
            log_host("info", "tray_icon_restored");
        }
        return 0;
    }
    if (message == tray_show_message()) {
        log_host("info", "second_instance_notified");
        show_main_window();
        return 0;
    }

    switch (message) {
    case kTrayCallbackMessage: {
        const UINT event = LOWORD(lparam);
        if (event == WM_LBUTTONDBLCLK) {
            show_main_window();
        } else if (event == WM_CONTEXTMENU || event == WM_LBUTTONUP || event == WM_RBUTTONUP
            || event == NIN_SELECT || event == NIN_KEYSELECT) {
            show_menu();
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kTrayMenuOpenMain:
            show_main_window();
            break;
        case kTrayMenuStart:
            post_command(Command::Start);
            break;
        case kTrayMenuStop:
            post_command(Command::Stop);
            break;
        case kTrayMenuReload:
            post_command(Command::Reload);
            break;
        case kTrayMenuOpenConfig:
            post_command(Command::OpenConfig);
            break;
        case kTrayMenuOpenLogs:
            post_command(Command::OpenLogs);
            break;
        case kTrayMenuLightweight:
            set_lightweight_mode(!(view_state_ && view_state_->lightweight_mode));
            break;
        case kTrayMenuStartup:
            post_command(Command::SetStartup, !startup_enabled_);
            break;
        case kTrayMenuExit:
            request_exit("menu");
            break;
        default:
            break;
        }
        return 0;
    case WM_TIMER:
        if (wparam == tray_messages::status_timer) {
            post_command(Command::Status);
        }
        return 0;
    case tray_messages::command_result:
        handle_command_result(std::unique_ptr<CommandResult>(
            reinterpret_cast<CommandResult*>(lparam)));
        return 0;
    case kViewCallbackMessage: {
        std::unique_ptr<std::function<void()>> callback(
            reinterpret_cast<std::function<void()>*>(lparam));
        if (callback && *callback) {
            (*callback)();
        }
        return 0;
    }
    case tray_messages::shutdown_complete: {
        std::unique_ptr<std::string> error(reinterpret_cast<std::string*>(lparam));
        finish_exit(wparam != 0, error ? std::move(*error) : std::string{});
        return 0;
    }
    case tray_messages::maintenance_shutdown:
        request_exit("maintenance", true);
        return 0;
    case tray_messages::gui_shutdown:
        request_exit("gui");
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wparam != 0) {
            request_exit("session_end", true);
        }
        return 0;
    case WM_CLOSE:
        request_exit("window_close");
        return 0;
    case WM_DESTROY:
        PostQuitMessage(exit_code_);
        return 0;
    default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
}

} // namespace ccs

#endif
