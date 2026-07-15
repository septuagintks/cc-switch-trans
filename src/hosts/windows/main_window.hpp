#pragma once

#ifdef _WIN32

#include "presentation/main_window_contract.hpp"
#include "presentation/main_window_view_model.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#ifdef StartService
#undef StartService
#endif

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ccs {

class WindowsMainWindow {
public:
    using LifecycleHandler = std::function<void(std::string_view)>;

    WindowsMainWindow(
        HINSTANCE instance,
        HICON icon,
        MainWindowViewModel& view_model,
        std::wstring window_class,
        std::wstring window_title,
        LifecycleHandler lifecycle_handler = {});
    ~WindowsMainWindow();

    WindowsMainWindow(const WindowsMainWindow&) = delete;
    WindowsMainWindow& operator=(const WindowsMainWindow&) = delete;

    bool show(MainWindowStateSnapshot state, std::string& error);
    void update(MainWindowStateSnapshot state);
    bool prepare_for_application_exit(std::function<void()> continuation);
    void destroy();

    bool exists() const noexcept;
    bool visible() const noexcept;
    HWND handle() const noexcept;

private:
    enum class CloseTarget {
        None,
        Hide,
        Destroy,
        ExitApplication,
    };

    struct PendingClose {
        CloseTarget target = CloseTarget::None;
        MainWindowCommand command = MainWindowCommand::Refresh;
        std::uint64_t previous_sequence = 0;
        std::function<void()> continuation;
    };

    static LRESULT CALLBACK window_proc(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    bool register_window_class(std::string& error);
    bool create_window(std::string& error);
    bool create_controls(std::string& error);
    HWND create_control(
        DWORD extended_style,
        const wchar_t* class_name,
        const wchar_t* text,
        DWORD style,
        int id);
    void create_fonts();
    void destroy_fonts() noexcept;
    void apply_fonts();
    void add_tooltip(HWND control, const wchar_t* text);
    void layout_controls();
    void render();
    void render_profile_list();
    void render_profile_details();
    void render_command_status();
    void update_enabled_states();
    void handle_command(int id, int notification_code);
    void handle_list_notification(const NMLISTVIEW& notification);

    void submit(MainWindowCommandRequest request);
    void request_window_close();
    bool request_close(
        CloseTarget target,
        std::function<void()> continuation = {});
    void finish_pending_close();
    void perform_close(CloseTarget target, std::function<void()> continuation = {});
    std::optional<UnsavedChangesDecision> prompt_unsaved_changes();
    void set_local_status(const std::wstring& message, bool error);

    std::wstring control_text(HWND control) const;
    const ProfileListItem* selected_profile() const noexcept;
    int scale(int logical_pixels) const noexcept;
    void notify_lifecycle(std::string_view event) const;

    HINSTANCE instance_ = nullptr;
    HICON icon_ = nullptr;
    MainWindowViewModel& view_model_;
    std::wstring window_class_;
    std::wstring window_title_;
    LifecycleHandler lifecycle_handler_;
    MainWindowStateSnapshot state_;
    HWND window_ = nullptr;
    HWND status_text_ = nullptr;
    HWND listener_text_ = nullptr;
    HWND start_button_ = nullptr;
    HWND stop_button_ = nullptr;
    HWND reload_button_ = nullptr;
    HWND lightweight_checkbox_ = nullptr;
    HWND header_separator_ = nullptr;
    HWND profile_list_ = nullptr;
    HWND new_profile_edit_ = nullptr;
    HWND add_profile_button_ = nullptr;
    HWND remove_profile_button_ = nullptr;
    HWND details_title_ = nullptr;
    HWND profile_id_label_ = nullptr;
    HWND rename_profile_edit_ = nullptr;
    HWND rename_profile_button_ = nullptr;
    HWND enabled_checkbox_ = nullptr;
    HWND protocol_label_ = nullptr;
    HWND protocol_value_ = nullptr;
    HWND readiness_label_ = nullptr;
    HWND readiness_value_ = nullptr;
    HWND profile_status_ = nullptr;
    HWND footer_separator_ = nullptr;
    HWND command_status_ = nullptr;
    HWND reload_draft_button_ = nullptr;
    HWND apply_button_ = nullptr;
    HWND discard_button_ = nullptr;
    HWND tooltip_ = nullptr;
    HFONT normal_font_ = nullptr;
    HFONT title_font_ = nullptr;
    HBRUSH error_brush_ = nullptr;
    std::wstring rendered_profile_id_;
    std::wstring local_status_;
    bool local_status_is_error_ = false;
    bool updating_ = false;
    bool class_registered_ = false;
    UINT dpi_ = 96;
    PendingClose pending_close_;
};

} // namespace ccs

#endif
