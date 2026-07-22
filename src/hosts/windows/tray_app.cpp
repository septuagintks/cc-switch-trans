#include "hosts/windows/tray_app.hpp"

#ifdef _WIN32

#include "hosts/windows/instance_coordinator.hpp"
#include "hosts/windows/resource_ids.hpp"
#include "hosts/windows/windows_error.hpp"

#include <strsafe.h>
#include <windowsx.h>

#include <memory>
#include <utility>

namespace ccs {

namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kCommandResultMessage = WM_APP + 2;
constexpr UINT kShutdownCompleteMessage = WM_APP + 3;
constexpr UINT kViewCallbackMessage = WM_APP + 4;
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT kStatusTimerIntervalMs = 1000;
constexpr UINT kTrayIconId = 1;

constexpr UINT kMenuStatus = 1000;
constexpr UINT kMenuStart = 1001;
constexpr UINT kMenuStop = 1002;
constexpr UINT kMenuReload = 1003;
constexpr UINT kMenuOpenConfig = 1004;
constexpr UINT kMenuOpenLogs = 1005;
constexpr UINT kMenuStartup = 1006;
constexpr UINT kMenuExit = 1007;
constexpr UINT kMenuOpenMain = 1008;
constexpr UINT kMenuLightweight = 1009;

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
    std::wstring window_title,
    std::wstring main_window_class)
    : instance_(instance)
    , paths_(std::move(paths))
    , executable_path_(std::move(executable_path))
    , window_class_(std::move(window_class))
    , window_title_(std::move(window_title))
    , main_window_class_(std::move(main_window_class))
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
          &executor_) {}

TrayApplication::~TrayApplication() {
    view_model_.set_update_handler({});
    if (main_window_) {
        main_window_->destroy();
    }
    view_model_.stop();
    executor_.stop();
    if (!shutdown_complete_) {
        std::string error;
        (void)controller_.shutdown(error);
    }
    remove_tray_icon();
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
    if (!register_window(error) || !add_tray_icon(error)) {
        log_host("error", "host_start_failed", {field_string("error", error)});
        return false;
    }

    main_window_ = std::make_unique<WindowsMainWindow>(
        instance_,
        icon_,
        view_model_,
        main_window_class_,
        window_title_,
        [this](std::string_view event) {
            log_host("info", "main_window_lifecycle", {
                field_string("action", std::string(event)),
            });
        });
    view_model_.set_update_handler([this](MainWindowStateSnapshot state) {
        handle_view_state(std::move(state));
    });
    if (!view_model_.submit({MainWindowCommand::LoadDraft})) {
        log_host("error", "main_window_draft_load_rejected");
    }

    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (SetTimer(window_, kStatusTimer, kStatusTimerIntervalMs, nullptr) == 0) {
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

bool TrayApplication::add_tray_icon(std::string& error) {
    if (!tray_icon_enabled_) {
        log_host("info", "tray_icon_skipped_for_integration_test");
        return true;
    }
    notification_ = {};
    notification_.cbSize = sizeof(notification_);
    notification_.hWnd = window_;
    notification_.uID = kTrayIconId;
    notification_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    notification_.uCallbackMessage = kTrayCallbackMessage;
    notification_.hIcon = icon_;
    (void)StringCchCopyW(notification_.szTip, ARRAYSIZE(notification_.szTip), L"ccs-trans");
    bool added = false;
    for (int attempt = 0; attempt < 20 && !added; ++attempt) {
        added = Shell_NotifyIconW(NIM_ADD, &notification_) != FALSE;
        if (!added) {
            Sleep(100);
        }
    }
    if (!added) {
        error = "failed to add the ccs-trans notification area icon after 2 seconds";
        return false;
    }
    notification_.uVersion = NOTIFYICON_VERSION_4;
    (void)Shell_NotifyIconW(NIM_SETVERSION, &notification_);
    auto tooltip = notification_;
    tooltip.uFlags = NIF_TIP | NIF_SHOWTIP;
    (void)Shell_NotifyIconW(NIM_MODIFY, &tooltip);
    tray_icon_added_ = true;
    return true;
}

void TrayApplication::remove_tray_icon() {
    if (tray_icon_added_) {
        (void)Shell_NotifyIconW(NIM_DELETE, &notification_);
        tray_icon_added_ = false;
    }
}

void TrayApplication::show_menu() {
    if (exiting_) {
        return;
    }
    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        log_host("error", "tray_menu_failed", {
            field_number("windows_error", static_cast<long long>(GetLastError())),
        });
        return;
    }

    const auto state = cached_status_.state;
    const bool transition = state == ApplicationState::Starting
        || state == ApplicationState::Reloading
        || state == ApplicationState::Stopping;
    const bool view_command_pending = view_state_ && view_state_->command_pending;
    const bool can_start = !service_command_pending_ && !view_command_pending && !transition
        && (state == ApplicationState::Stopped || state == ApplicationState::Faulted);
    const bool can_stop = !service_command_pending_ && !view_command_pending && !transition
        && state == ApplicationState::Running;
    const bool can_reload = can_stop;

    (void)AppendMenuW(menu, MF_STRING, kMenuOpenMain, L"Open ccs-trans");
    SetMenuDefaultItem(menu, kMenuOpenMain, FALSE);
    (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const auto status = status_text(cached_status_);
    (void)AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, kMenuStatus, status.c_str());
    (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    (void)AppendMenuW(menu, MF_STRING | (can_start ? MF_ENABLED : MF_GRAYED), kMenuStart, L"Start");
    (void)AppendMenuW(menu, MF_STRING | (can_stop ? MF_ENABLED : MF_GRAYED), kMenuStop, L"Stop");
    (void)AppendMenuW(
        menu, MF_STRING | (can_reload ? MF_ENABLED : MF_GRAYED), kMenuReload, L"Reload configuration");
    (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    (void)AppendMenuW(menu, MF_STRING, kMenuOpenConfig, L"Open configuration");
    (void)AppendMenuW(menu, MF_STRING, kMenuOpenLogs, L"Open logs");
    UINT lightweight_flags = MF_STRING;
    if (!view_state_ || view_command_pending) {
        lightweight_flags |= MF_GRAYED;
    } else if (view_state_->lightweight_mode) {
        lightweight_flags |= MF_CHECKED;
    }
    (void)AppendMenuW(menu, lightweight_flags, kMenuLightweight, L"Lightweight mode");
    UINT startup_flags = MF_STRING;
    if (!startup_known_) {
        startup_flags |= MF_GRAYED;
    } else if (startup_enabled_) {
        startup_flags |= MF_CHECKED;
    }
    (void)AppendMenuW(menu, startup_flags, kMenuStartup, L"Start at sign-in");
    (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    (void)AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    POINT cursor{};
    (void)GetCursorPos(&cursor);
    SetForegroundWindow(window_);
    const UINT selected = TrackPopupMenuEx(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        window_,
        nullptr);
    DestroyMenu(menu);
    if (selected != 0) {
        PostMessageW(window_, WM_COMMAND, MAKEWPARAM(selected, 0), 0);
    }
}

void TrayApplication::show_main_window() {
    if (exiting_ || !main_window_) {
        return;
    }
    std::string error;
    const auto state = view_state_ ? view_state_ : view_model_.snapshot();
    if (!main_window_->show(state, error)) {
        log_host("error", "main_window_show_failed", {field_string("error", error)});
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
    if (exiting_) {
        return;
    }
    if (!force && main_window_ && !main_window_->prepare_for_application_exit(
            [this, reason]() { begin_exit(reason); })) {
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
    const bool previous_lightweight = view_state_ && view_state_->lightweight_mode;
    view_state_ = std::move(state);
    cached_status_ = view_state_->application;
    if (main_window_) {
        main_window_->update(view_state_);
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
    if (!previous_lightweight && view_state_->lightweight_mode && main_window_
        && resolve_cached_main_window(
                view_state_->draft,
                main_window_->exists(),
                main_window_->visible(),
                true)
            == CachedWindowAction::Destroy) {
        main_window_->destroy();
    }
}

void TrayApplication::show_notification(
    const std::wstring& title,
    const std::wstring& message,
    DWORD flags) {
    if (!tray_icon_added_) {
        return;
    }
    auto notification = notification_;
    notification.uFlags = NIF_INFO;
    notification.dwInfoFlags = flags;
    (void)StringCchCopyW(notification.szInfoTitle, ARRAYSIZE(notification.szInfoTitle), title.c_str());
    (void)StringCchCopyW(notification.szInfo, ARRAYSIZE(notification.szInfo), message.c_str());
    (void)Shell_NotifyIconW(NIM_MODIFY, &notification);
}

void TrayApplication::post_command(Command command, bool startup_enabled) {
    if (exiting_) {
        return;
    }
    if (command == Command::Status) {
        if (status_pending_) {
            return;
        }
        status_pending_ = true;
    }
    if (command == Command::Start || command == Command::Stop || command == Command::Reload) {
        if (service_command_pending_) {
            return;
        }
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
                result->succeeded = platform_.open_config_file(paths_, result->error);
                break;
            case Command::OpenLogs:
                result->succeeded = platform_.open_logs_directory(paths_, result->error);
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
                && (command == Command::Status || command == Command::SetStartup)) {
                if (result->error.empty()) {
                    result->error = std::move(startup_error);
                }
                result->succeeded = false;
            }
        } catch (const std::exception& ex) {
            result->succeeded = false;
            result->error = "host command failed: " + std::string(ex.what());
            result->status = controller_.status();
        } catch (...) {
            result->succeeded = false;
            result->error = "host command failed with an unknown exception";
            result->status = controller_.status();
        }
        auto* raw_result = result.release();
        if (!PostMessageW(
                window_, kCommandResultMessage, 0, reinterpret_cast<LPARAM>(raw_result))) {
            delete raw_result;
        }
    });
    if (!posted) {
        if (command == Command::Status) {
            status_pending_ = false;
        }
        if (command == Command::Start || command == Command::Stop || command == Command::Reload) {
            service_command_pending_ = false;
        }
        log_host("error", "host_command_rejected", {
            field_string("command", command_name(command)),
        });
    }
}

void TrayApplication::handle_command_result(std::unique_ptr<CommandResult> result) {
    if (!result) {
        return;
    }
    if (result->command == Command::Status) {
        status_pending_ = false;
    }
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
        log_host("error", "host_status_failed", {field_string("error", result->error)});
        last_status_error_ = result->error;
    } else if (result->succeeded) {
        last_status_error_.clear();
    }
    if (previous_state != cached_status_.state) {
        log_host("info", "host_state_changed", {
            field_string("previous_state", application_state_name(previous_state)),
            field_string("state", application_state_name(cached_status_.state)),
            field_number("exit_code", cached_status_.last_exit_code),
            field_string("error", cached_status_.last_error),
        });
    }

    if (!result->succeeded && result->command != Command::Status) {
        const auto message = result->error.empty() ? L"The command failed. Open logs for details."
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
    if (exiting_) {
        return;
    }
    exiting_ = true;
    view_model_.set_update_handler({});
    if (main_window_) {
        main_window_->destroy();
    }
    view_model_.stop();
    KillTimer(window_, kStatusTimer);
    remove_tray_icon();
    log_host("info", "host_shutdown_start", {field_string("reason", reason)});
    if (!executor_.post([this]() {
            std::string error;
            const bool succeeded = controller_.shutdown(error);
            auto* result = new std::string(std::move(error));
            if (!PostMessageW(
                    window_,
                    kShutdownCompleteMessage,
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
    log_host(succeeded ? "info" : "error", "host_shutdown_complete", {
        field_bool("succeeded", succeeded),
        field_string("error", error),
    });
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

void TrayApplication::log_host(
    const std::string& level,
    const std::string& event,
    const std::vector<LogField>& fields) {
    if (!logger_ || !logger_->log(level, event, fields)) {
        debug_output(level + " " + event);
    }
}

const char* TrayApplication::command_name(Command command) {
    switch (command) {
    case Command::Status:
        return "status";
    case Command::Start:
        return "start";
    case Command::Stop:
        return "stop";
    case Command::Reload:
        return "reload";
    case Command::OpenConfig:
        return "open_config";
    case Command::OpenLogs:
        return "open_logs";
    case Command::SetStartup:
        return "set_startup";
    }
    return "unknown";
}

std::wstring TrayApplication::status_text(const ApplicationStatus& status) {
    std::wstring result = L"Status: ";
    result += utf8_to_wide(application_state_name(status.state));
    if (!status.listener_host.empty() && status.listener_port != 0) {
        result += L" (";
        result += utf8_to_wide(status.listener_host);
        result += L":";
        result += std::to_wstring(status.listener_port);
        result += L")";
    }
    return result;
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
        tray_icon_added_ = false;
        std::string error;
        if (!add_tray_icon(error)) {
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
        case kMenuOpenMain:
            show_main_window();
            break;
        case kMenuStart:
            post_command(Command::Start);
            break;
        case kMenuStop:
            post_command(Command::Stop);
            break;
        case kMenuReload:
            post_command(Command::Reload);
            break;
        case kMenuOpenConfig:
            post_command(Command::OpenConfig);
            break;
        case kMenuOpenLogs:
            post_command(Command::OpenLogs);
            break;
        case kMenuLightweight:
            set_lightweight_mode(!(view_state_ && view_state_->lightweight_mode));
            break;
        case kMenuStartup:
            post_command(Command::SetStartup, !startup_enabled_);
            break;
        case kMenuExit:
            request_exit("menu");
            break;
        default:
            break;
        }
        return 0;
    case WM_TIMER:
        if (wparam == kStatusTimer) {
            post_command(Command::Status);
        }
        return 0;
    case kCommandResultMessage:
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
    case kShutdownCompleteMessage: {
        std::unique_ptr<std::string> error(reinterpret_cast<std::string*>(lparam));
        finish_exit(wparam != 0, error ? std::move(*error) : std::string{});
        return 0;
    }
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
