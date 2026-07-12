#pragma once

#ifdef _WIN32

#include "app/application_controller.hpp"
#include "hosts/control_executor.hpp"
#include "hosts/windows/windows_host_platform.hpp"
#include "logging/logger.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ccs {

class TrayApplication {
public:
    TrayApplication(
        HINSTANCE instance,
        AppPaths paths,
        std::filesystem::path executable_path);
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
    ApplicationController controller_;
    WindowsHostPlatform platform_;
    ControlExecutor executor_;
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
    int exit_code_ = 0;
};

} // namespace ccs

#endif
