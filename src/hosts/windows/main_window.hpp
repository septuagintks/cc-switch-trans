#pragma once

#ifdef _WIN32

#include "presentation/main_window_contract.hpp"
#include "presentation/main_window_view_model.hpp"
#include "hosts/windows/windows_theme.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef StartService
#undef StartService
#endif

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    enum class View {
        Profiles,
        Rules,
        Settings,
    };

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

    struct FieldControl {
        ConfigurationFieldState state;
        HWND label = nullptr;
        HWND input = nullptr;
        int id = 0;
    };

    static LRESULT CALLBACK window_proc(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam);
    static LRESULT CALLBACK input_subclass_proc(
        HWND control,
        UINT message,
        WPARAM wparam,
        LPARAM lparam,
        UINT_PTR subclass_id,
        DWORD_PTR reference_data);
    static LRESULT CALLBACK canvas_static_subclass_proc(
        HWND control,
        UINT message,
        WPARAM wparam,
        LPARAM lparam,
        UINT_PTR subclass_id,
        DWORD_PTR reference_data);
    static LRESULT CALLBACK combo_list_subclass_proc(
        HWND control,
        UINT message,
        WPARAM wparam,
        LPARAM lparam,
        UINT_PTR subclass_id,
        DWORD_PTR reference_data);
    static LRESULT CALLBACK settings_scrollbar_subclass_proc(
        HWND control,
        UINT message,
        WPARAM wparam,
        LPARAM lparam,
        UINT_PTR subclass_id,
        DWORD_PTR reference_data);
    static LRESULT CALLBACK settings_viewport_subclass_proc(
        HWND control,
        UINT message,
        WPARAM wparam,
        LPARAM lparam,
        UINT_PTR subclass_id,
        DWORD_PTR reference_data);
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
    bool ensure_field_controls(std::string& error);
    FieldControl create_field_control(
        const ConfigurationFieldState& state,
        int id);

    void create_fonts();
    void destroy_fonts() noexcept;
    void apply_fonts();
    void apply_theme();
    void install_input_subclass(HWND control);
    void install_canvas_static_subclass(HWND control);
    void draw_input_frame(HWND control, HDC dc);
    void configure_combo_box(HWND control);
    void update_combo_list_region(HWND list);
    void draw_combo_box(HWND control, HDC dc);
    void update_input_metrics(HWND control);
    void add_tooltip(HWND control, const wchar_t* text);

    void set_view(View view);
    void update_view_visibility();
    void layout_controls();
    RECT content_rectangle() const;
    void layout_profile_view(const RECT& content);
    void layout_rules_view(const RECT& content);
    void layout_settings_view(const RECT& content, bool redraw = true);
    void layout_settings_fields(bool redraw);
    void position_settings_content(bool redraw);
    void update_settings_scrollbar(const RECT& content);
    int settings_scroll_maximum() const noexcept;
    int settings_page_rows() const noexcept;
    void advance_settings_scroll_animation();

    void render();
    void render_profile_list();
    void render_profile_details();
    void render_rules_editor();
    void render_settings();
    void render_command_status();
    void update_enabled_states();
    void populate_field_controls(
        std::vector<FieldControl>& controls,
        const std::vector<ConfigurationFieldState>& fields);

    void paint(HDC dc);
    bool draw_item(const DRAWITEMSTRUCT& item);
    void draw_status(const DRAWITEMSTRUCT& item);
    void draw_button(const DRAWITEMSTRUCT& item);
    void draw_profile_item(const DRAWITEMSTRUCT& item);
    void draw_combo_item(const DRAWITEMSTRUCT& item);
    void draw_settings_scrollbar(const DRAWITEMSTRUCT& item);
    void draw_rules_scroll_indicator(HDC dc);
    void draw_toggle(HDC dc, const RECT& rectangle, bool checked, bool enabled);

    void handle_command(int id, int notification_code);
    void handle_profile_selection();
    void handle_rules_profile_selection();
    void handle_settings_scroll(int request, int thumb_position = 0);
    void handle_settings_scrollbar_pointer(
        HWND control,
        UINT message,
        WPARAM wparam,
        LPARAM lparam);
    void show_rules_scroll_indicator();
    void hide_rules_scroll_indicator();
    RECT settings_scrollbar_thumb() const;
    bool collect_field_edits(
        const std::vector<FieldControl>& controls,
        std::vector<ConfigurationFieldEdit>& edits,
        std::wstring& error) const;

    bool submit(MainWindowCommandRequest request);
    void request_window_close();
    bool request_close(
        CloseTarget target,
        std::function<void()> continuation = {});
    void finish_pending_close();
    void perform_close(CloseTarget target, std::function<void()> continuation = {});
    std::optional<UnsavedChangesDecision> prompt_unsaved_changes();
    void set_local_status(const std::wstring& message, bool error);

    std::wstring control_text(HWND control) const;
    void set_control_text_if_changed(HWND control, std::wstring_view text) const;
    bool combo_box_dropped() const noexcept;
    const ProfileListItem* selected_profile() const noexcept;
    int scale(int logical_pixels) const noexcept;
    void notify_lifecycle(std::string_view event) const;
    void reset_handles() noexcept;

    HINSTANCE instance_ = nullptr;
    HICON icon_ = nullptr;
    MainWindowViewModel& view_model_;
    std::wstring window_class_;
    std::wstring window_title_;
    LifecycleHandler lifecycle_handler_;
    MainWindowStateSnapshot state_;
    WindowsTheme theme_;
    View view_ = View::Profiles;

    HWND window_ = nullptr;
    HWND brand_text_ = nullptr;
    HWND status_text_ = nullptr;
    HWND listener_text_ = nullptr;
    HWND start_button_ = nullptr;
    HWND stop_button_ = nullptr;
    HWND reload_button_ = nullptr;
    HWND lightweight_checkbox_ = nullptr;
    HWND nav_profiles_ = nullptr;
    HWND nav_rules_ = nullptr;
    HWND nav_settings_ = nullptr;

    HWND profile_list_ = nullptr;
    HWND new_profile_edit_ = nullptr;
    HWND add_profile_button_ = nullptr;
    HWND remove_profile_button_ = nullptr;
    HWND details_title_ = nullptr;
    HWND profile_id_label_ = nullptr;
    HWND rename_profile_edit_ = nullptr;
    HWND rename_profile_button_ = nullptr;
    HWND enabled_checkbox_ = nullptr;
    HWND readiness_label_ = nullptr;
    HWND readiness_value_ = nullptr;
    HWND profile_status_ = nullptr;
    HWND update_profile_fields_button_ = nullptr;
    std::vector<FieldControl> profile_fields_;

    HWND rules_title_ = nullptr;
    HWND rules_profile_combo_ = nullptr;
    HWND rules_edit_ = nullptr;
    HWND rules_format_button_ = nullptr;
    HWND rules_update_button_ = nullptr;
    HWND rules_status_ = nullptr;

    HWND settings_title_ = nullptr;
    HWND settings_subtitle_ = nullptr;
    HWND update_settings_button_ = nullptr;
    HWND settings_viewport_ = nullptr;
    HWND settings_content_ = nullptr;
    HWND settings_scrollbar_ = nullptr;
    std::vector<FieldControl> settings_fields_;

    HWND command_status_ = nullptr;
    HWND reload_draft_button_ = nullptr;
    HWND apply_button_ = nullptr;
    HWND discard_button_ = nullptr;
    HWND tooltip_ = nullptr;

    HFONT normal_font_ = nullptr;
    HFONT semibold_font_ = nullptr;
    HFONT title_font_ = nullptr;
    HFONT mono_font_ = nullptr;
    std::vector<ProfileListItem> rendered_profiles_;
    std::optional<ProfileEditorState> rendered_profile_editor_;
    std::vector<ConfigurationFieldState> rendered_application_fields_;
    std::optional<RulesEditorState> rendered_rules_editor_;
    std::wstring local_status_;
    bool local_status_is_error_ = false;
    bool profile_local_dirty_ = false;
    bool settings_local_dirty_ = false;
    bool rules_local_dirty_ = false;
    bool updating_ = false;
    bool class_registered_ = false;
    bool settings_scrollbar_dragging_ = false;
    int settings_scrollbar_drag_offset_ = 0;
    int settings_scroll_ = 0;
    int settings_scroll_target_ = 0;
    int settings_content_height_ = 0;
    int rules_wheel_remainder_ = 0;
    bool rules_scroll_indicator_visible_ = false;
    std::optional<ProfileKey> pending_profile_selection_;
    bool pending_profile_selection_started_ = false;
    UINT dpi_ = 96;
    PendingClose pending_close_;
};

} // namespace ccs

#endif
