#pragma once

#ifdef _WIN32

#include "app/application_controller.hpp"
#include "app/control_executor.hpp"
#include "config/config_editing_service.hpp"
#include "config/config_store.hpp"
#include "hosts/windows/main_window.hpp"
#include "hosts/windows/windows_host_platform.hpp"
#include "logging/logger.hpp"
#include "presentation/main_window_view_model.hpp"
#include "presentation/ui_preferences_store.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ccs {

class TrayApplication {
public:
    TrayApplication(
        HINSTANCE instance,
        AppPaths paths,
        std::filesystem::path executable_path,
        std::wstring window_class,
        std::wstring window_title,
        std::wstring main_window_class);
    ~TrayApplication();

    TrayApplication(const TrayApplication&) = delete;
    TrayApplication& operator=(const TrayApplication&) = delete;

    int run();

private:
    enum class Command {
        Status,
        Start,
        Stop,
        Reload,
        OpenConfig,
        OpenLogs,
        SetStartup,
    };

    struct CommandResult {
        Command command = Command::Status;
        bool succeeded = false;
        bool startup_known = false;
        bool startup_enabled = false;
        std::string error;
        ApplicationStatus status;
    };

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    bool initialize(std::string& error);
    bool initialize_host_logger(std::string& error);
    bool register_window(std::string& error);
    bool add_tray_icon(std::string& error);
    void remove_tray_icon();
    void show_menu();
    void show_main_window();
    void set_lightweight_mode(bool enabled);
    void request_exit(const std::string& reason, bool force = false);
    void dispatch_view_callback(std::function<void()> callback);
    void handle_view_state(MainWindowStateSnapshot state);
    void show_notification(const std::wstring& title, const std::wstring& message, DWORD flags);
    void post_command(Command command, bool startup_enabled = false);
    void handle_command_result(std::unique_ptr<CommandResult> result);
    void begin_exit(const std::string& reason);
    void finish_exit(bool succeeded, std::string error);
    void log_host(
        const std::string& level,
        const std::string& event,
        const std::vector<LogField>& fields = {});

    static const char* command_name(Command command);
    static std::wstring status_text(const ApplicationStatus& status);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HICON icon_ = nullptr;
    NOTIFYICONDATAW notification_{};
    UINT taskbar_created_message_ = 0;
    AppPaths paths_;
    std::filesystem::path executable_path_;
    std::wstring window_class_;
    std::wstring window_title_;
    std::wstring main_window_class_;
    ApplicationController controller_;
    WindowsHostPlatform platform_;
    ControlExecutor executor_;
    ConfigStore config_repository_;
    ConfigEditingService config_editing_;
    UiPreferencesStore ui_preferences_;
    MainWindowViewModel view_model_;
    std::unique_ptr<WindowsMainWindow> main_window_;
    MainWindowStateSnapshot view_state_;
    std::unique_ptr<Logger> logger_;
    ApplicationStatus cached_status_;
    bool tray_icon_added_ = false;
    bool tray_icon_enabled_ = true;
    bool startup_known_ = false;
    bool startup_enabled_ = false;
    bool status_pending_ = false;
    bool service_command_pending_ = false;
    bool exiting_ = false;
    bool shutdown_complete_ = false;
    std::string last_status_error_;
    std::uint64_t last_view_command_sequence_ = 0;
    int exit_code_ = 0;
};

} // namespace ccs

#endif
