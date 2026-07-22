#pragma once

#ifdef _WIN32

#include "app/application_controller.hpp"
#include "app/control_executor.hpp"
#include "config/composite_config_repository.hpp"
#include "config/configuration_editor.hpp"
#include "hosts/windows/gui_bridge/gui_command_router.hpp"
#include "hosts/windows/maintenance/maintenance_ipc_server.hpp"
#include "hosts/windows/tray/gui_session_controller.hpp"
#include "hosts/windows/tray/tray_icon.hpp"
#include "hosts/windows/windows_host_platform.hpp"
#include "logging/logger.hpp"
#include "presentation/main_window_view_model.hpp"
#include "presentation/ui_preferences_store.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ccs {

class TrayApplication {
public:
    TrayApplication(
        HINSTANCE instance,
        AppPaths paths,
        std::filesystem::path executable_path,
        std::wstring window_class,
        std::wstring window_title);
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
    bool initialize_gui_session(std::string& error);
    bool register_window(std::string& error);
    void show_menu();
    void show_main_window();
    void set_lightweight_mode(bool enabled);
    void request_exit(const std::string& reason, bool force = false);
    void dispatch_view_callback(std::function<void()> callback);
    void handle_view_state(MainWindowStateSnapshot state);
    void handle_gui_command(
        const gui_ipc::Envelope& envelope,
        const gui_ipc::Command& command);
    void handle_gui_ipc_event(
        std::string_view event,
        std::string detail,
        std::uint64_t process_id);
    gui_ipc::MaintenanceResult handle_maintenance_request(
        const gui_ipc::MaintenanceRequest& request);
    void handle_maintenance_ipc_event(
        std::string_view event,
        std::string detail,
        std::uint64_t process_id);
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

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbar_created_message_ = 0;
    AppPaths paths_;
    std::filesystem::path executable_path_;
    std::wstring window_class_;
    std::wstring window_title_;
    ApplicationController controller_;
    WindowsHostPlatform platform_;
    ControlExecutor executor_;
    CompositeConfigRepository config_repository_;
    ConfigurationEditor config_editing_;
    UiPreferencesStore ui_preferences_;
    MainWindowViewModel view_model_;
    GuiCommandRouter gui_command_router_;
    std::unique_ptr<GuiSessionController> gui_session_;
    std::unique_ptr<MaintenanceIpcServer> maintenance_server_;
    MainWindowStateSnapshot view_state_;
    std::unique_ptr<Logger> logger_;
    ApplicationStatus cached_status_;
    TrayIcon tray_icon_;
    bool tray_icon_enabled_ = true;
    bool startup_known_ = false;
    bool startup_enabled_ = false;
    bool status_pending_ = false;
    bool service_command_pending_ = false;
    bool initial_draft_load_complete_ = false;
    bool gui_open_pending_ = false;
    std::atomic_bool exiting_{false};
    std::atomic_bool maintenance_shutdown_requested_{false};
    std::atomic_bool gui_shutdown_complete_{false};
    bool shutdown_complete_ = false;
    std::string last_status_error_;
    std::uint64_t last_view_command_sequence_ = 0;
    int exit_code_ = 0;
};

} // namespace ccs

#endif
