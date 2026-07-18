#include "hosts/windows/main_window.hpp"

#ifdef _WIN32

#include "hosts/windows/resource_ids.hpp"
#include "hosts/windows/windows_error.hpp"

#include <commctrl.h>
#include <windowsx.h>

#ifdef StartService
#undef StartService
#endif

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

namespace ccs {

namespace {

constexpr int kDefaultWidth = 1120;
constexpr int kDefaultHeight = 760;
constexpr int kMinimumWidth = 920;
constexpr int kMinimumHeight = 640;
constexpr int kHeaderHeight = 88;
constexpr int kFooterHeight = 68;
constexpr int kNavigationWidth = 178;
constexpr int kInputHeight = 35;
constexpr int kToggleButtonHeight = 32;
constexpr int kToggleFieldHeight = 30;
constexpr int kToggleTrackHeight = 20;
constexpr int kContentSeparatorGap = 12;

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) {
        return L"Invalid UTF-8 text";
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

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    (void)WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    return result;
}

std::wstring to_windows_edit_newlines(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == L'\n' && (index == 0 || value[index - 1] != L'\r')) {
            result.push_back(L'\r');
        }
        result.push_back(value[index]);
    }
    return result;
}

std::wstring to_canonical_newlines(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == L'\r') {
            if (index + 1 < value.size() && value[index + 1] == L'\n') {
                continue;
            }
            result.push_back(L'\n');
        } else {
            result.push_back(value[index]);
        }
    }
    return result;
}

const wchar_t* readiness_text(ProfileReadiness readiness) {
    switch (readiness) {
    case ProfileReadiness::Incomplete:
        return L"Incomplete";
    case ProfileReadiness::Ready:
        return L"Ready";
    case ProfileReadiness::Invalid:
        return L"Invalid";
    }
    return L"Unknown";
}

const wchar_t* field_display_name(std::string_view key) {
    if (key == "field.listener.host") return L"Listener address";
    if (key == "field.listener.port") return L"Listener port";
    if (key == "field.runtime.worker_threads") return L"Worker threads";
    if (key == "field.runtime.max_connections") return L"Maximum connections";
    if (key == "field.runtime.max_request_body_size") return L"Request body limit (bytes)";
    if (key == "field.runtime.max_response_body_size") return L"Response body limit (bytes)";
    if (key == "field.runtime.max_inflight_bytes") return L"Inflight memory budget (bytes)";
    if (key == "field.runtime.metrics_interval_ms") return L"Metrics interval (ms)";
    if (key == "field.timeouts.resolve_ms") return L"Resolve timeout (ms)";
    if (key == "field.timeouts.connect_ms") return L"Connect timeout (ms)";
    if (key == "field.timeouts.send_ms") return L"Send timeout (ms)";
    if (key == "field.timeouts.response_header_ms") return L"Response header timeout (ms)";
    if (key == "field.timeouts.stream_idle_ms") return L"Stream idle timeout (ms)";
    if (key == "field.timeouts.total_ms") return L"Total timeout (ms)";
    if (key == "field.logging.path") return L"Log path";
    if (key == "field.logging.level") return L"Log level";
    if (key == "field.logging.body") return L"Record bodies";
    if (key == "field.logging.redact_sensitive") return L"Redact sensitive headers";
    if (key == "field.logging.body_limit") return L"Logged body limit (bytes)";
    if (key == "field.logging.queue_capacity") return L"Log queue capacity (bytes)";
    if (key == "field.logging.max_total_size") return L"Total log limit (bytes)";
    if (key == "field.logging.flush_interval_ms") return L"Log flush interval (ms)";
    if (key == "field.profile.id") return L"Profile ID";
    if (key == "field.profile.enabled") return L"Enabled";
    if (key == "field.profile.protocol") return L"Protocol";
    if (key == "field.profile.local_request_path") return L"Local request path";
    if (key == "field.profile.local_usage_path") return L"Local usage path";
    if (key == "field.profile.upstream_base_url") return L"Upstream base URL";
    if (key == "field.profile.upstream_request_path") return L"Upstream request path";
    if (key == "field.profile.upstream_usage_path") return L"Upstream usage path";
    return L"Configuration field";
}

std::wstring field_value_text(const ConfigurationFieldValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return utf8_to_wide(*text);
    }
    if (const auto* number = std::get_if<std::uint64_t>(&value)) {
        return std::to_wstring(*number);
    }
    return std::get<bool>(value) ? L"true" : L"false";
}

std::wstring listener_status_text(const ApplicationStatus& status) {
    if (status.listener_host.empty() || status.listener_port == 0) {
        return L"Listener inactive";
    }
    auto text = utf8_to_wide(status.listener_host);
    text += L":";
    text += std::to_wstring(status.listener_port);
    return text;
}

bool is_toggle_id(int id) {
    return id == kMainLightweightId
        || id == kMainProfileEnabledId
        || (id >= kMainProfileFieldBaseId && id < kMainSettingsFieldBaseId)
        || (id >= kMainSettingsFieldBaseId && id < kMainSettingsFieldBaseId + 100);
}

bool is_navigation_id(int id) {
    return id == kMainNavProfilesId
        || id == kMainNavRulesId
        || id == kMainNavSettingsId;
}

bool is_primary_id(int id) {
    return id == kMainApplyId
        || id == kMainUpdateProfileFieldsId
        || id == kMainRulesUpdateId
        || id == kMainUpdateSettingsId;
}

constexpr UINT_PTR kToggleSubclassId = 1;
constexpr UINT_PTR kOwnerDrawSubclassId = 2;
constexpr UINT_PTR kInputSubclassId = 3;
constexpr UINT_PTR kCanvasStaticSubclassId = 4;
constexpr UINT_PTR kComboListSubclassId = 5;
constexpr UINT_PTR kSettingsScrollbarSubclassId = 6;
constexpr UINT_PTR kSettingsViewportSubclassId = 7;
constexpr UINT_PTR kSettingsScrollTimerId = 1;
constexpr UINT_PTR kRulesScrollIndicatorTimerId = 2;
constexpr UINT kSettingsScrollIntervalMs = 16;
constexpr UINT kRulesScrollIndicatorDurationMs = 800;
constexpr wchar_t kToggleStateProperty[] = L"ccs-trans.ToggleState";

LRESULT CALLBACK owner_draw_subclass_proc(
    HWND control,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR) {
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, owner_draw_subclass_proc, kOwnerDrawSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK toggle_subclass_proc(
    HWND control,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR reference_data) {
    if (message == WM_MOUSEWHEEL && reference_data != 0) {
        (void)SendMessageW(
            reinterpret_cast<HWND>(reference_data), message, wparam, lparam);
        return 0;
    }
    const auto checked = [&]() {
        return GetPropW(control, kToggleStateProperty) != nullptr;
    };
    const auto set_checked = [&](bool value) {
        if (checked() == value) {
            return;
        }
        if (value) {
            (void)SetPropW(control, kToggleStateProperty, reinterpret_cast<HANDLE>(1));
        } else {
            (void)RemovePropW(control, kToggleStateProperty);
        }
        InvalidateRect(control, nullptr, TRUE);
    };
    if (message == BM_SETCHECK) {
        set_checked(wparam == BST_CHECKED);
        return 0;
    }
    if (message == BM_GETCHECK) {
        return checked() ? BST_CHECKED : BST_UNCHECKED;
    }
    if (message == WM_LBUTTONUP
        || (message == WM_KEYUP && wparam == VK_SPACE)) {
        set_checked(!checked());
    }
    if (message == WM_NCDESTROY) {
        (void)RemovePropW(control, kToggleStateProperty);
        RemoveWindowSubclass(control, toggle_subclass_proc, kToggleSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

void install_toggle_subclass(HWND control, HWND scroll_target) {
    if (control != nullptr) {
        (void)SetWindowSubclass(
            control,
            toggle_subclass_proc,
            kToggleSubclassId,
            reinterpret_cast<DWORD_PTR>(scroll_target));
    }
}

void install_owner_draw_subclass(HWND control) {
    if (control != nullptr) {
        (void)SetWindowSubclass(
            control, owner_draw_subclass_proc, kOwnerDrawSubclassId, 0);
    }
}

} // namespace

WindowsMainWindow::WindowsMainWindow(
    HINSTANCE instance,
    HICON icon,
    MainWindowViewModel& view_model,
    std::wstring window_class,
    std::wstring window_title,
    LifecycleHandler lifecycle_handler)
    : instance_(instance)
    , icon_(icon)
    , view_model_(view_model)
    , window_class_(std::move(window_class))
    , window_title_(std::move(window_title))
    , lifecycle_handler_(std::move(lifecycle_handler)) {}

WindowsMainWindow::~WindowsMainWindow() {
    destroy();
    destroy_fonts();
}

bool WindowsMainWindow::show(MainWindowStateSnapshot state, std::string& error) {
    error.clear();
    state_ = std::move(state);
    if (window_ == nullptr && (!register_window_class(error) || !create_window(error))) {
        return false;
    }
    render();
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(window_);
    notify_lifecycle("shown");
    return true;
}

void WindowsMainWindow::update(MainWindowStateSnapshot state) {
    state_ = std::move(state);
    if (window_ == nullptr) {
        return;
    }
    render();
    finish_pending_close();
}

bool WindowsMainWindow::prepare_for_application_exit(
    std::function<void()> continuation) {
    state_ = view_model_.snapshot();
    if (!state_) {
        return true;
    }
    std::string error;
    if (state_->command_pending || state_->draft.busy()) {
        if (!show(state_, error)) {
            set_local_status(utf8_to_wide(error), true);
            return false;
        }
        set_local_status(L"Wait for the current command to finish.", true);
        return false;
    }
    if (!state_->draft.dirty()) {
        return true;
    }
    if (!show(state_, error)) {
        set_local_status(utf8_to_wide(error), true);
        return false;
    }
    return request_close(CloseTarget::ExitApplication, std::move(continuation));
}

void WindowsMainWindow::destroy() {
    pending_close_ = {};
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

bool WindowsMainWindow::exists() const noexcept {
    return window_ != nullptr;
}

bool WindowsMainWindow::visible() const noexcept {
    return window_ != nullptr && IsWindowVisible(window_) != FALSE;
}

HWND WindowsMainWindow::handle() const noexcept {
    return window_;
}

bool WindowsMainWindow::register_window_class(std::string& error) {
    if (class_registered_) {
        return true;
    }
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    if (!InitCommonControlsEx(&controls)) {
        error = "failed to initialize Windows common controls";
        return false;
    }
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &WindowsMainWindow::window_proc;
    window_class.hInstance = instance_;
    window_class.hIcon = icon_;
    window_class.hIconSm = icon_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = window_class_.c_str();
    if (RegisterClassExW(&window_class) == 0
        && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = windows_error_message(
            "failed to register the main window class", GetLastError());
        return false;
    }
    class_registered_ = true;
    return true;
}

bool WindowsMainWindow::create_window(std::string& error) {
    dpi_ = GetDpiForSystem();
    RECT rectangle{0, 0, scale(kDefaultWidth), scale(kDefaultHeight)};
    (void)AdjustWindowRectExForDpi(
        &rectangle,
        WS_OVERLAPPEDWINDOW,
        FALSE,
        WS_EX_APPWINDOW,
        dpi_);
    const int width = rectangle.right - rectangle.left;
    const int height = rectangle.bottom - rectangle.top;
    const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
    const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);
    window_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        window_class_.c_str(),
        window_title_.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr) {
        error = windows_error_message("failed to create the main window", GetLastError());
        return false;
    }
    return true;
}

HWND WindowsMainWindow::create_control(
    DWORD extended_style,
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    int id) {
    return CreateWindowExW(
        extended_style,
        class_name,
        text,
        WS_CHILD | WS_VISIBLE | style,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance_,
        nullptr);
}

bool WindowsMainWindow::create_controls(std::string& error) {
    brand_text_ = create_control(
        0, WC_STATICW, L"ccs-trans", SS_CENTER | SS_CENTERIMAGE, 0);
    status_text_ = create_control(0, WC_STATICW, L"Stopped", SS_OWNERDRAW, 0);
    listener_text_ = create_control(0, WC_STATICW, L"Listener inactive", SS_LEFT | SS_CENTERIMAGE, 0);
    start_button_ = create_control(0, WC_BUTTONW, L"Start", BS_OWNERDRAW | WS_TABSTOP, kMainServiceStartId);
    stop_button_ = create_control(0, WC_BUTTONW, L"Stop", BS_OWNERDRAW | WS_TABSTOP, kMainServiceStopId);
    reload_button_ = create_control(0, WC_BUTTONW, L"Reload", BS_OWNERDRAW | WS_TABSTOP, kMainServiceReloadId);
    lightweight_checkbox_ = create_control(
        0, WC_BUTTONW, L"Lightweight mode",
        BS_OWNERDRAW | WS_TABSTOP, kMainLightweightId);

    nav_profiles_ = create_control(0, WC_BUTTONW, L"Profiles", BS_OWNERDRAW | WS_TABSTOP, kMainNavProfilesId);
    nav_rules_ = create_control(0, WC_BUTTONW, L"Rules", BS_OWNERDRAW | WS_TABSTOP, kMainNavRulesId);
    nav_settings_ = create_control(0, WC_BUTTONW, L"Settings", BS_OWNERDRAW | WS_TABSTOP, kMainNavSettingsId);

    profile_list_ = create_control(
        0,
        WC_LISTBOXW,
        L"",
        LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT
            | WS_VSCROLL | WS_TABSTOP,
        kMainProfileListId);
    new_profile_edit_ = create_control(
        0, WC_EDITW, L"", ES_MULTILINE | ES_AUTOHSCROLL | WS_TABSTOP,
        kMainNewProfileEditId);
    add_profile_button_ = create_control(
        0, WC_BUTTONW, L"+", BS_OWNERDRAW | WS_TABSTOP, kMainAddProfileId);
    remove_profile_button_ = create_control(
        0, WC_BUTTONW, L"\u2212", BS_OWNERDRAW | WS_TABSTOP, kMainRemoveProfileId);
    details_title_ = create_control(0, WC_STATICW, L"Profile", SS_LEFT, 0);
    profile_id_label_ = create_control(0, WC_STATICW, L"Profile ID", SS_LEFT | SS_CENTERIMAGE, 0);
    rename_profile_edit_ = create_control(
        0, WC_EDITW, L"", ES_MULTILINE | ES_AUTOHSCROLL | WS_TABSTOP,
        kMainRenameProfileEditId);
    rename_profile_button_ = create_control(
        0, WC_BUTTONW, L"Rename", BS_OWNERDRAW | WS_TABSTOP, kMainRenameProfileId);
    enabled_checkbox_ = create_control(
        0, WC_BUTTONW, L"Enabled",
        BS_OWNERDRAW | WS_TABSTOP, kMainProfileEnabledId);
    readiness_label_ = create_control(0, WC_STATICW, L"Configuration", SS_LEFT, 0);
    readiness_value_ = create_control(0, WC_STATICW, L"Incomplete", SS_RIGHT, 0);
    profile_status_ = create_control(
        0, WC_STATICW, L"", SS_LEFT | SS_EDITCONTROL, kMainProfileStatusId);
    update_profile_fields_button_ = create_control(
        0, WC_BUTTONW, L"Update profile", BS_OWNERDRAW | WS_TABSTOP,
        kMainUpdateProfileFieldsId);

    rules_title_ = create_control(0, WC_STATICW, L"Request rules", SS_LEFT, 0);
    rules_profile_combo_ = create_control(
        0,
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS
            | WS_VSCROLL | WS_TABSTOP,
        kMainRulesProfileId);
    rules_edit_ = create_control(
        0,
        WC_EDITW,
        L"",
        ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN
            | ES_NOHIDESEL | WS_TABSTOP,
        kMainRulesEditId);
    rules_format_button_ = create_control(
        0, WC_BUTTONW, L"Format", BS_OWNERDRAW | WS_TABSTOP, kMainRulesFormatId);
    rules_update_button_ = create_control(
        0, WC_BUTTONW, L"Update rules", BS_OWNERDRAW | WS_TABSTOP, kMainRulesUpdateId);
    rules_status_ = create_control(
        0, WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS, kMainRulesStatusId);

    settings_title_ = create_control(0, WC_STATICW, L"Application settings", SS_LEFT, 0);
    settings_subtitle_ = create_control(
        0, WC_STATICW, L"Listener, runtime, timeout, and logging values", SS_LEFT, 0);
    update_settings_button_ = create_control(
        0, WC_BUTTONW, L"Update settings", BS_OWNERDRAW | WS_TABSTOP,
        kMainUpdateSettingsId);
    settings_viewport_ = create_control(
        WS_EX_CONTROLPARENT,
        WC_STATICW,
        L"",
        SS_NOTIFY | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        kMainSettingsViewportId);
    settings_content_ = create_control(
        WS_EX_CONTROLPARENT,
        WC_STATICW,
        L"",
        SS_NOTIFY | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        kMainSettingsContentId);
    if (settings_content_ != nullptr) {
        if (settings_viewport_ == nullptr
            || SetParent(settings_content_, settings_viewport_) == nullptr) {
            DestroyWindow(settings_content_);
            settings_content_ = nullptr;
        }
    }
    settings_scrollbar_ = create_control(
        0,
        WC_STATICW,
        L"",
        SS_OWNERDRAW | SS_NOTIFY,
        kMainSettingsScrollbarId);

    command_status_ = create_control(
        0, WC_STATICW, L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, 0);
    reload_draft_button_ = create_control(
        0, WC_BUTTONW, L"Reload draft", BS_OWNERDRAW | WS_TABSTOP, kMainReloadDraftId);
    apply_button_ = create_control(
        0, WC_BUTTONW, L"Apply changes", BS_OWNERDRAW | WS_TABSTOP, kMainApplyId);
    discard_button_ = create_control(
        0, WC_BUTTONW, L"Discard", BS_OWNERDRAW | WS_TABSTOP, kMainDiscardId);
    tooltip_ = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_,
        nullptr,
        instance_,
        nullptr);

    const std::array required = {
        brand_text_, status_text_, listener_text_, start_button_, stop_button_, reload_button_,
        lightweight_checkbox_, nav_profiles_, nav_rules_, nav_settings_, profile_list_,
        new_profile_edit_, add_profile_button_, remove_profile_button_, details_title_,
        profile_id_label_, rename_profile_edit_, rename_profile_button_, enabled_checkbox_,
        readiness_label_, readiness_value_, profile_status_, update_profile_fields_button_,
        rules_title_, rules_profile_combo_, rules_edit_, rules_format_button_,
        rules_update_button_, rules_status_, settings_title_, settings_subtitle_,
        update_settings_button_, settings_viewport_, settings_content_, settings_scrollbar_,
        command_status_, reload_draft_button_, apply_button_,
        discard_button_, tooltip_,
    };
    if (std::any_of(required.begin(), required.end(), [](HWND control) {
            return control == nullptr;
        })) {
        error = windows_error_message(
            "failed to create main window controls", GetLastError());
        return false;
    }

    (void)SendMessageW(new_profile_edit_, EM_SETLIMITTEXT, 64, 0);
    (void)SendMessageW(rename_profile_edit_, EM_SETLIMITTEXT, 64, 0);
    (void)SendMessageW(rules_edit_, EM_SETLIMITTEXT, kMaxStoredProfileRulesTextBytes, 0);
    (void)SendMessageW(new_profile_edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"New Profile ID"));
    (void)SendMessageW(rename_profile_edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Profile ID"));
    (void)SendMessageW(profile_list_, LB_SETITEMHEIGHT, 0, scale(54));
    ShowScrollBar(rules_edit_, SB_BOTH, FALSE);
    install_toggle_subclass(lightweight_checkbox_, window_);
    install_toggle_subclass(enabled_checkbox_, window_);
    for (const auto control : {
             status_text_, start_button_, stop_button_, reload_button_,
             lightweight_checkbox_, nav_profiles_, nav_rules_, nav_settings_,
             add_profile_button_, remove_profile_button_, rename_profile_button_,
             enabled_checkbox_, update_profile_fields_button_, rules_format_button_,
             rules_update_button_, update_settings_button_, reload_draft_button_,
             apply_button_, discard_button_}) {
        install_owner_draw_subclass(control);
    }
    for (const auto control : {
             profile_list_, new_profile_edit_, rename_profile_edit_,
             rules_profile_combo_, rules_edit_}) {
        install_input_subclass(control);
    }
    for (const auto control : {
             brand_text_, listener_text_, details_title_, profile_id_label_,
             readiness_label_, readiness_value_, profile_status_, rules_title_,
             rules_status_, settings_title_, settings_subtitle_, command_status_}) {
        install_canvas_static_subclass(control);
    }
    (void)SetWindowSubclass(
        settings_scrollbar_,
        settings_scrollbar_subclass_proc,
        kSettingsScrollbarSubclassId,
        reinterpret_cast<DWORD_PTR>(this));
    (void)SetWindowSubclass(
        settings_viewport_,
        settings_viewport_subclass_proc,
        kSettingsViewportSubclassId,
        reinterpret_cast<DWORD_PTR>(this));
    (void)SetWindowSubclass(
        settings_content_,
        settings_viewport_subclass_proc,
        kSettingsViewportSubclassId,
        reinterpret_cast<DWORD_PTR>(this));
    ShowScrollBar(window_, SB_VERT, FALSE);
    (void)SendMessageW(new_profile_edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(scale(10), scale(10)));
    (void)SendMessageW(rename_profile_edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(scale(10), scale(10)));

    add_tooltip(start_button_, L"Start the local service");
    add_tooltip(stop_button_, L"Stop the local service");
    add_tooltip(reload_button_, L"Reload the saved configuration");
    add_tooltip(add_profile_button_, L"Create a disabled Profile draft");
    add_tooltip(remove_profile_button_, L"Remove the selected Profile from the draft");
    add_tooltip(rules_format_button_, L"Validate and rewrite the current Rule text canonically");
    add_tooltip(reload_draft_button_, L"Reload configuration from disk");

    dpi_ = GetDpiForWindow(window_);
    theme_.refresh(window_, dpi_);
    create_fonts();
    apply_fonts();
    apply_theme();
    return true;
}

WindowsMainWindow::FieldControl WindowsMainWindow::create_field_control(
    const ConfigurationFieldState& state,
    int id) {
    FieldControl control;
    control.state = state;
    control.id = id;
    control.label = create_control(
        0, WC_STATICW, field_display_name(state.display_name_key),
        SS_LEFT | SS_CENTERIMAGE, 0);
    install_canvas_static_subclass(control.label);
    if (state.input_kind == ConfigurationFieldInputKind::Boolean) {
        control.input = create_control(
            0, WC_BUTTONW, L"",
            BS_OWNERDRAW | WS_TABSTOP, id);
        install_toggle_subclass(control.input, window_);
        install_owner_draw_subclass(control.input);
    } else if (state.input_kind == ConfigurationFieldInputKind::Enumeration) {
        control.input = create_control(
            0, WC_COMBOBOXW, L"",
            CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS
                | WS_VSCROLL | WS_TABSTOP,
            id);
        if (!state.required) {
            (void)SendMessageW(control.input, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Not set"));
        }
        for (const auto& option : state.enum_values) {
            const auto wide = utf8_to_wide(option);
            (void)SendMessageW(control.input, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
        }
    } else {
        control.input = create_control(
            0, WC_EDITW, L"", ES_MULTILINE | ES_AUTOHSCROLL | WS_TABSTOP, id);
        (void)SendMessageW(
            control.input,
            EM_SETMARGINS,
            EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(scale(10), scale(10)));
        (void)SendMessageW(control.input, EM_SETLIMITTEXT, 32767, 0);
    }
    if (control.input != nullptr
        && state.input_kind != ConfigurationFieldInputKind::Boolean) {
        install_input_subclass(control.input);
    }
    return control;
}

bool WindowsMainWindow::ensure_field_controls(std::string& error) {
    error.clear();
    if (profile_fields_.empty() && state_ && state_->profile_editor) {
        int id = kMainProfileFieldBaseId;
        for (const auto& field : state_->profile_editor->fields) {
            if (field.key == "id" || field.key == "enabled") {
                continue;
            }
            auto control = create_field_control(field, id++);
            if (control.label == nullptr || control.input == nullptr) {
                error = windows_error_message("failed to create Profile field control", GetLastError());
                return false;
            }
            profile_fields_.push_back(std::move(control));
        }
    }
    if (settings_fields_.empty() && state_ && !state_->application_fields.empty()) {
        int id = kMainSettingsFieldBaseId;
        for (const auto& field : state_->application_fields) {
            auto control = create_field_control(field, id++);
            if (control.label == nullptr || control.input == nullptr) {
                error = windows_error_message("failed to create Settings field control", GetLastError());
                return false;
            }
            ShowWindow(control.label, SW_HIDE);
            ShowWindow(control.input, SW_HIDE);
            if (SetParent(control.label, settings_content_) == nullptr
                || SetParent(control.input, settings_content_) == nullptr) {
                error = windows_error_message(
                    "failed to attach Settings field control", GetLastError());
                return false;
            }
            settings_fields_.push_back(std::move(control));
        }
    }
    apply_fonts();
    apply_theme();
    return true;
}

void WindowsMainWindow::create_fonts() {
    destroy_fonts();
    const auto make_font = [&](int points, int weight, const wchar_t* family) {
        return CreateFontW(
            -MulDiv(points, static_cast<int>(dpi_), 72),
            0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, family);
    };
    normal_font_ = make_font(10, FW_NORMAL, L"Segoe UI");
    semibold_font_ = make_font(10, FW_SEMIBOLD, L"Segoe UI");
    title_font_ = make_font(18, FW_SEMIBOLD, L"Segoe UI");
    mono_font_ = make_font(10, FW_NORMAL, L"Cascadia Mono");
}

void WindowsMainWindow::destroy_fonts() noexcept {
    if (normal_font_ != nullptr) DeleteObject(normal_font_);
    if (semibold_font_ != nullptr) DeleteObject(semibold_font_);
    if (title_font_ != nullptr) DeleteObject(title_font_);
    if (mono_font_ != nullptr) DeleteObject(mono_font_);
    normal_font_ = nullptr;
    semibold_font_ = nullptr;
    title_font_ = nullptr;
    mono_font_ = nullptr;
}

void WindowsMainWindow::apply_fonts() {
    const std::array normal_controls = {
        status_text_, listener_text_, start_button_, stop_button_, reload_button_,
        lightweight_checkbox_, nav_profiles_, nav_rules_, nav_settings_, profile_list_,
        new_profile_edit_, add_profile_button_, remove_profile_button_, profile_id_label_,
        rename_profile_edit_, rename_profile_button_, enabled_checkbox_, readiness_label_,
        readiness_value_, profile_status_, update_profile_fields_button_, rules_profile_combo_,
        rules_format_button_, rules_update_button_, rules_status_, settings_subtitle_,
        update_settings_button_, command_status_, reload_draft_button_, apply_button_,
        discard_button_,
    };
    for (const auto control : normal_controls) {
        if (control != nullptr) {
            (void)SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
        }
    }
    for (const auto control : {brand_text_, details_title_, rules_title_, settings_title_}) {
        if (control != nullptr) {
            (void)SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
        }
    }
    if (rules_edit_ != nullptr) {
        (void)SendMessageW(rules_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_font_), TRUE);
    }
    for (const auto& field : profile_fields_) {
        (void)SendMessageW(field.label, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
        (void)SendMessageW(field.input, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
    }
    for (const auto& field : settings_fields_) {
        (void)SendMessageW(field.label, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
        (void)SendMessageW(field.input, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
    }
}

void WindowsMainWindow::apply_theme() {
    const std::array controls = {
        profile_list_, new_profile_edit_, rename_profile_edit_, rules_profile_combo_,
        rules_edit_, tooltip_,
    };
    for (const auto control : controls) {
        theme_.apply_to_control(control);
        InvalidateRect(control, nullptr, TRUE);
    }
    for (const auto& field : profile_fields_) {
        theme_.apply_to_control(field.input);
        InvalidateRect(field.input, nullptr, TRUE);
    }
    for (const auto& field : settings_fields_) {
        theme_.apply_to_control(field.input);
        InvalidateRect(field.input, nullptr, TRUE);
    }
    configure_combo_box(rules_profile_combo_);
    for (const auto& field : profile_fields_) {
        if (field.state.input_kind == ConfigurationFieldInputKind::Enumeration) {
            configure_combo_box(field.input);
        }
    }
    for (const auto& field : settings_fields_) {
        if (field.state.input_kind == ConfigurationFieldInputKind::Enumeration) {
            configure_combo_box(field.input);
        }
    }
    if (settings_viewport_ != nullptr) {
        RedrawWindow(
            settings_viewport_,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    InvalidateRect(window_, nullptr, TRUE);
}

void WindowsMainWindow::install_input_subclass(HWND control) {
    if (control != nullptr) {
        (void)SetWindowSubclass(
            control,
            input_subclass_proc,
            kInputSubclassId,
            reinterpret_cast<DWORD_PTR>(this));
    }
}

void WindowsMainWindow::install_canvas_static_subclass(HWND control) {
    if (control != nullptr) {
        (void)SetWindowSubclass(
            control,
            canvas_static_subclass_proc,
            kCanvasStaticSubclassId,
            reinterpret_cast<DWORD_PTR>(this));
    }
}

void WindowsMainWindow::draw_input_frame(HWND control, HDC dc) {
    if (control == nullptr || dc == nullptr) {
        return;
    }
    RECT rectangle{};
    GetClientRect(control, &rectangle);
    const int radius = control == profile_list_ || control == rules_edit_
        ? theme_.metrics().radius_large
        : theme_.metrics().radius;
    const auto& palette = theme_.palette();
    const COLORREF border = GetFocus() == control ? palette.accent : palette.border;
    theme_.draw_control_frame(
        dc,
        rectangle,
        radius,
        palette.canvas,
        border,
        theme_.metrics().border);
}

void WindowsMainWindow::configure_combo_box(HWND control) {
    if (control == nullptr) {
        return;
    }
    COMBOBOXINFO info{};
    info.cbSize = sizeof(info);
    if (!GetComboBoxInfo(control, &info) || info.hwndList == nullptr) {
        return;
    }
    theme_.apply_to_control(info.hwndList);
    theme_.apply_to_window(info.hwndList);
    (void)SetWindowSubclass(
        info.hwndList,
        combo_list_subclass_proc,
        kComboListSubclassId,
        reinterpret_cast<DWORD_PTR>(this));

    const LONG_PTR style = GetWindowLongPtrW(info.hwndList, GWL_STYLE);
    const LONG_PTR extended_style = GetWindowLongPtrW(info.hwndList, GWL_EXSTYLE);
    const LONG_PTR next_style = style & ~static_cast<LONG_PTR>(WS_BORDER);
    const LONG_PTR next_extended_style = extended_style
        & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
    if (next_style != style) {
        (void)SetWindowLongPtrW(info.hwndList, GWL_STYLE, next_style);
    }
    if (next_extended_style != extended_style) {
        (void)SetWindowLongPtrW(info.hwndList, GWL_EXSTYLE, next_extended_style);
    }
    if (next_style != style || next_extended_style != extended_style) {
        (void)SetWindowPos(
            info.hwndList,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE
                | SWP_NOACTIVATE | SWP_NOZORDER);
    }
    update_combo_list_region(info.hwndList);
}

void WindowsMainWindow::update_combo_list_region(HWND list) {
    if (list == nullptr) {
        return;
    }
    RECT rectangle{};
    if (!GetWindowRect(list, &rectangle)) {
        return;
    }
    const int width = rectangle.right - rectangle.left;
    const int height = rectangle.bottom - rectangle.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    const int diameter = std::max(scale(6), theme_.metrics().radius * 2);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
    if (region != nullptr && SetWindowRgn(list, region, TRUE) == 0) {
        DeleteObject(region);
    }
}

void WindowsMainWindow::draw_combo_box(HWND control, HDC dc) {
    if (control == nullptr || dc == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(control, &client);
    if (client.right <= client.left || client.bottom <= client.top) {
        return;
    }

    const HDC buffer = CreateCompatibleDC(dc);
    const HBITMAP bitmap = CreateCompatibleBitmap(
        dc, client.right - client.left, client.bottom - client.top);
    if (buffer == nullptr || bitmap == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (buffer != nullptr) DeleteDC(buffer);
        return;
    }
    const auto old_bitmap = SelectObject(buffer, bitmap);
    const auto& palette = theme_.palette();
    FillRect(buffer, &client, theme_.canvas_brush());
    RECT frame = client;
    InflateRect(&frame, -1, -1);
    const bool enabled = IsWindowEnabled(control) != FALSE;
    const bool focused = GetFocus() == control
        || SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
    theme_.draw_rounded_rectangle(
        buffer,
        frame,
        theme_.metrics().radius,
        palette.surface,
        focused ? palette.accent : palette.border,
        focused ? scale(2) : theme_.metrics().border);

    std::wstring text;
    const int selected = static_cast<int>(SendMessageW(control, CB_GETCURSEL, 0, 0));
    if (selected >= 0) {
        const int length = static_cast<int>(
            SendMessageW(control, CB_GETLBTEXTLEN, selected, 0));
        if (length > 0) {
            text.resize(static_cast<std::size_t>(length) + 1);
            (void)SendMessageW(
                control,
                CB_GETLBTEXT,
                selected,
                reinterpret_cast<LPARAM>(text.data()));
            text.resize(static_cast<std::size_t>(length));
        }
    }

    RECT arrow = frame;
    arrow.left = std::max(frame.left, frame.right - scale(32));
    RECT text_rectangle = frame;
    text_rectangle.left += scale(12);
    text_rectangle.right = arrow.left - scale(4);
    SetBkMode(buffer, TRANSPARENT);
    SetTextColor(buffer, enabled ? palette.text : palette.disabled);
    const auto old_font = SelectObject(buffer, normal_font_);
    DrawTextW(
        buffer,
        text.c_str(),
        static_cast<int>(text.size()),
        &text_rectangle,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(buffer, old_font);
    theme_.draw_chevron_down(
        buffer,
        arrow,
        enabled ? palette.text_muted : palette.disabled,
        scale(2));

    BitBlt(
        dc,
        client.left,
        client.top,
        client.right - client.left,
        client.bottom - client.top,
        buffer,
        0,
        0,
        SRCCOPY);
    SelectObject(buffer, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

void WindowsMainWindow::update_input_metrics(HWND control) {
    if (control == nullptr) {
        return;
    }
    wchar_t class_name[32]{};
    (void)GetClassNameW(control, class_name, static_cast<int>(std::size(class_name)));
    if (lstrcmpiW(class_name, L"ComboBox") == 0) {
        (void)SendMessageW(control, CB_SETITEMHEIGHT, -1, scale(27));
        (void)SendMessageW(control, CB_SETITEMHEIGHT, 0, scale(28));
        return;
    }
    if (lstrcmpiW(class_name, L"Edit") != 0) {
        return;
    }
    if (control == rules_edit_) {
        (void)SendMessageW(
            control,
            EM_SETMARGINS,
            EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(scale(10), scale(18)));
        return;
    }

    RECT rectangle{};
    GetClientRect(control, &rectangle);
    if (rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top) {
        return;
    }
    const HDC dc = GetDC(control);
    if (dc == nullptr) {
        return;
    }
    const auto font = reinterpret_cast<HFONT>(
        SendMessageW(control, WM_GETFONT, 0, 0));
    const auto old_font = font != nullptr ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics{};
    (void)GetTextMetricsW(dc, &metrics);
    if (old_font != nullptr) {
        SelectObject(dc, old_font);
    }
    ReleaseDC(control, dc);

    const int horizontal_padding = scale(12);
    const int text_height = std::max(
        1, static_cast<int>(metrics.tmHeight) + scale(2));
    const int available_height = static_cast<int>(rectangle.bottom - rectangle.top);
    const int top = std::max(1, (available_height - text_height) / 2);
    RECT format{
        horizontal_padding,
        top,
        std::max(
            horizontal_padding + 1,
            static_cast<int>(rectangle.right) - horizontal_padding),
        std::min(
            static_cast<int>(rectangle.bottom) - 1,
            top + text_height),
    };
    (void)SendMessageW(
        control,
        EM_SETRECTNP,
        0,
        reinterpret_cast<LPARAM>(&format));
}

void WindowsMainWindow::add_tooltip(HWND control, const wchar_t* text) {
    if (tooltip_ == nullptr || control == nullptr) {
        return;
    }
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = window_;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(text);
    (void)SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

void WindowsMainWindow::set_view(View view) {
    if (view_ == view) {
        return;
    }
    view_ = view;
    update_view_visibility();
    layout_controls();
    InvalidateRect(window_, nullptr, TRUE);
}

void WindowsMainWindow::update_view_visibility() {
    const auto set_visible = [](HWND control, bool visible) {
        if (control != nullptr) ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    };
    const bool profiles = view_ == View::Profiles;
    const bool rules = view_ == View::Rules;
    const bool settings = view_ == View::Settings;
    for (const auto control : {
             profile_list_, new_profile_edit_, add_profile_button_, remove_profile_button_,
             details_title_, profile_id_label_, rename_profile_edit_, rename_profile_button_,
             enabled_checkbox_, readiness_label_, readiness_value_, profile_status_,
             update_profile_fields_button_}) {
        set_visible(control, profiles);
    }
    for (const auto& field : profile_fields_) {
        set_visible(field.label, profiles);
        set_visible(field.input, profiles);
    }
    for (const auto control : {
             rules_title_, rules_profile_combo_, rules_edit_, rules_format_button_,
             rules_update_button_, rules_status_}) {
        set_visible(control, rules);
    }
    for (const auto control : {
             settings_title_, settings_subtitle_, update_settings_button_,
             settings_viewport_}) {
        set_visible(control, settings);
    }
    set_visible(
        settings_scrollbar_,
        settings && settings_scroll_maximum() > 0);
    ShowScrollBar(window_, SB_VERT, FALSE);
}

void WindowsMainWindow::layout_controls() {
    if (window_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int margin = scale(20);
    const int gap = scale(8);
    const int button_height = scale(34);
    const int nav_x = margin;
    const int nav_width = scale(kNavigationWidth - 28);
    const int header_row_y = scale(15);
    const int header_row_height = scale(32);

    MoveWindow(brand_text_, nav_x, header_row_y, nav_width, header_row_height, TRUE);
    MoveWindow(
        status_text_, scale(210), header_row_y + scale(2),
        scale(92), scale(28), TRUE);
    MoveWindow(
        listener_text_, scale(318), header_row_y,
        scale(260), header_row_height, TRUE);
    int action_x = width - margin - scale(88);
    MoveWindow(reload_button_, action_x, scale(15), scale(88), button_height, TRUE);
    action_x -= scale(88) + gap;
    MoveWindow(stop_button_, action_x, scale(15), scale(88), button_height, TRUE);
    action_x -= scale(88) + gap;
    MoveWindow(start_button_, action_x, scale(15), scale(88), button_height, TRUE);
    MoveWindow(
        lightweight_checkbox_, width - margin - scale(178), scale(52),
        scale(178), scale(kToggleButtonHeight), TRUE);

    int nav_y = scale(kHeaderHeight + 8);
    for (const auto control : {nav_profiles_, nav_rules_, nav_settings_}) {
        MoveWindow(control, nav_x, nav_y, nav_width, scale(42), TRUE);
        nav_y += scale(48);
    }

    const RECT content = content_rectangle();
    if (view_ == View::Profiles) layout_profile_view(content);
    if (view_ == View::Rules) layout_rules_view(content);
    if (view_ == View::Settings) layout_settings_view(content);

    const int footer_y = height - scale(kFooterHeight) + scale(kContentSeparatorGap);
    int footer_x = width - margin;
    auto place_footer = [&](HWND control, int logical_width) {
        const int control_width = scale(logical_width);
        footer_x -= control_width;
        MoveWindow(control, footer_x, footer_y, control_width, button_height, TRUE);
        footer_x -= gap;
    };
    place_footer(apply_button_, 126);
    place_footer(discard_button_, 92);
    place_footer(reload_draft_button_, 112);
    MoveWindow(
        command_status_,
        scale(kNavigationWidth + 18),
        footer_y,
        std::max(0, footer_x - scale(kNavigationWidth + 30)),
        button_height,
        TRUE);

    for (const auto control : {
             new_profile_edit_, rename_profile_edit_, rules_profile_combo_, rules_edit_}) {
        update_input_metrics(control);
    }
    for (const auto& field : profile_fields_) update_input_metrics(field.input);
    for (const auto& field : settings_fields_) update_input_metrics(field.input);

}

RECT WindowsMainWindow::content_rectangle() const {
    RECT client{};
    GetClientRect(window_, &client);
    return {
        scale(kNavigationWidth + 18),
        scale(kHeaderHeight + 8),
        client.right - scale(20),
        client.bottom - scale(kFooterHeight) - scale(kContentSeparatorGap),
    };
}

void WindowsMainWindow::layout_profile_view(const RECT& content) {
    const int gap = scale(18);
    const int left_width = std::clamp(
        static_cast<int>((content.right - content.left) / 3), scale(260), scale(310));
    const int row_height = scale(kInputHeight);
    const int add_y = content.bottom - row_height;
    MoveWindow(
        profile_list_, content.left, content.top,
        left_width, std::max(0, static_cast<int>(add_y - content.top - scale(10))), TRUE);
    MoveWindow(new_profile_edit_, content.left, add_y, left_width - scale(82), row_height, TRUE);
    MoveWindow(add_profile_button_, content.left + left_width - scale(74), add_y, scale(34), row_height, TRUE);
    MoveWindow(remove_profile_button_, content.left + left_width - scale(34), add_y, scale(34), row_height, TRUE);

    const int right_x = content.left + left_width + gap;
    const int right_width = content.right - right_x;
    int y = content.top;
    MoveWindow(details_title_, right_x, y, right_width - scale(120), scale(34), TRUE);
    MoveWindow(readiness_label_, right_x + right_width - scale(210), y + scale(5), scale(108), scale(22), TRUE);
    MoveWindow(readiness_value_, right_x + right_width - scale(96), y + scale(5), scale(96), scale(22), TRUE);
    y += scale(48);

    const int label_width = scale(170);
    const int rename_width = scale(88);
    MoveWindow(profile_id_label_, right_x, y, label_width, row_height, TRUE);
    MoveWindow(
        rename_profile_edit_, right_x + label_width, y,
        std::max(scale(100), right_width - label_width - rename_width - scale(8)), row_height, TRUE);
    MoveWindow(
        rename_profile_button_, right_x + right_width - rename_width, y,
        rename_width, row_height, TRUE);
    y += scale(48);
    MoveWindow(
        enabled_checkbox_, right_x + label_width, y,
        scale(150), scale(kToggleButtonHeight), TRUE);
    y += scale(44);
    for (auto& field : profile_fields_) {
        MoveWindow(field.label, right_x, y, label_width, row_height, TRUE);
        const bool toggle = field.state.input_kind
            == ConfigurationFieldInputKind::Boolean;
        const int input_height = toggle
            ? scale(kToggleFieldHeight)
            : (field.state.input_kind == ConfigurationFieldInputKind::Enumeration
                    ? scale(220)
                    : row_height);
        const int input_width = toggle
            ? scale(54)
            : right_width - label_width;
        MoveWindow(
            field.input,
            right_x + label_width,
            y + (toggle ? (row_height - input_height) / 2 : 0),
            input_width,
            input_height,
            TRUE);
        y += scale(47);
    }
    MoveWindow(
        update_profile_fields_button_, right_x + right_width - scale(132), y,
        scale(132), row_height, TRUE);
    MoveWindow(
        profile_status_, right_x, y + scale(4),
        std::max(0, right_width - scale(148)), scale(56), TRUE);
}

void WindowsMainWindow::layout_rules_view(const RECT& content) {
    MoveWindow(rules_title_, content.left, content.top, scale(260), scale(34), TRUE);
    const int row_y = content.top + scale(44);
    MoveWindow(rules_profile_combo_, content.left, row_y, scale(260), scale(220), TRUE);
    MoveWindow(rules_update_button_, content.right - scale(128), row_y, scale(128), scale(36), TRUE);
    MoveWindow(rules_format_button_, content.right - scale(226), row_y, scale(90), scale(36), TRUE);
    const int edit_top = row_y + scale(48);
    MoveWindow(
        rules_edit_, content.left, edit_top,
        content.right - content.left,
        std::max(0, static_cast<int>(content.bottom - edit_top - scale(34))), TRUE);
    MoveWindow(
        rules_status_, content.left, content.bottom - scale(26),
        content.right - content.left, scale(24), TRUE);
}

void WindowsMainWindow::layout_settings_view(const RECT& content, bool redraw) {
    const BOOL repaint = redraw ? TRUE : FALSE;
    MoveWindow(settings_title_, content.left, content.top, scale(330), scale(34), repaint);
    MoveWindow(settings_subtitle_, content.left, content.top + scale(38), scale(520), scale(24), repaint);
    MoveWindow(
        update_settings_button_, content.right - scale(136), content.top,
        scale(136), scale(36), repaint);
    const int viewport_top = content.top + scale(72);
    const int viewport_bottom = content.bottom;
    MoveWindow(
        settings_viewport_,
        content.left,
        viewport_top,
        std::max(0, static_cast<int>(content.right - content.left)),
        std::max(0, viewport_bottom - viewport_top),
        repaint);
    MoveWindow(
        settings_scrollbar_,
        content.right + scale(8),
        viewport_top,
        scale(8),
        std::max(0, viewport_bottom - viewport_top),
        repaint);
    const int row_height = scale(48);
    settings_content_height_ = static_cast<int>(settings_fields_.size()) * row_height;
    update_settings_scrollbar(content);
    position_settings_content(false);
    layout_settings_fields(redraw);
}

void WindowsMainWindow::layout_settings_fields(bool redraw) {
    if (settings_content_ == nullptr) {
        return;
    }
    RECT content{};
    GetClientRect(settings_content_, &content);
    const int row_height = scale(48);
    const int label_width = scale(260);
    const int content_width = content.right - content.left;
    int y = 0;
    for (auto& field : settings_fields_) {
        const UINT position_flags = SWP_NOACTIVATE | SWP_NOZORDER
            | (redraw ? 0U : SWP_NOREDRAW);
        (void)SetWindowPos(
            field.label,
            nullptr,
            0,
            y,
            label_width,
            scale(kInputHeight),
            position_flags | SWP_SHOWWINDOW);
        const bool toggle = field.state.input_kind
            == ConfigurationFieldInputKind::Boolean;
        const int input_height = toggle
            ? scale(kToggleFieldHeight)
            : (field.state.input_kind == ConfigurationFieldInputKind::Enumeration
                    ? scale(220)
                    : scale(kInputHeight));
        const int input_width = toggle
            ? scale(54)
            : content_width - label_width - scale(10);
        (void)SetWindowPos(
            field.input,
            nullptr,
            label_width,
            y + (toggle ? (scale(kInputHeight) - input_height) / 2 : 0),
            input_width,
            input_height,
            position_flags | SWP_SHOWWINDOW);
        y += row_height;
    }
    RedrawWindow(
        settings_content_,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN
            | (redraw ? RDW_UPDATENOW : 0));
}

void WindowsMainWindow::position_settings_content(bool redraw) {
    if (settings_viewport_ == nullptr || settings_content_ == nullptr) {
        return;
    }
    RECT viewport{};
    GetClientRect(settings_viewport_, &viewport);
    const UINT flags = SWP_NOACTIVATE | SWP_NOZORDER
        | (redraw ? 0U : SWP_NOREDRAW);
    (void)SetWindowPos(
        settings_content_,
        nullptr,
        0,
        -settings_scroll_,
        std::max(0, static_cast<int>(viewport.right - viewport.left)),
        std::max(static_cast<int>(viewport.bottom), settings_content_height_),
        flags | SWP_SHOWWINDOW);
    if (redraw) {
        RedrawWindow(
            settings_viewport_,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN | RDW_UPDATENOW);
    }
}

void WindowsMainWindow::update_settings_scrollbar(const RECT& content) {
    (void)content;
    const int maximum = settings_scroll_maximum();
    settings_scroll_ = std::clamp(settings_scroll_, 0, maximum);
    settings_scroll_target_ = std::clamp(settings_scroll_target_, 0, maximum);
    if (settings_scrollbar_ != nullptr) {
        ShowWindow(
            settings_scrollbar_,
            view_ == View::Settings && maximum > 0 ? SW_SHOW : SW_HIDE);
        InvalidateRect(settings_scrollbar_, nullptr, FALSE);
    }
}

int WindowsMainWindow::settings_page_rows() const noexcept {
    if (settings_viewport_ == nullptr) {
        return 1;
    }
    RECT viewport{};
    GetClientRect(settings_viewport_, &viewport);
    return std::max(1, static_cast<int>(viewport.bottom) / std::max(1, scale(48)));
}

int WindowsMainWindow::settings_scroll_maximum() const noexcept {
    const int hidden_rows = std::max(
        0,
        static_cast<int>(settings_fields_.size()) - settings_page_rows());
    return hidden_rows * scale(48);
}

RECT WindowsMainWindow::settings_scrollbar_thumb() const {
    RECT scrollbar{};
    if (settings_scrollbar_ == nullptr) {
        return scrollbar;
    }
    GetClientRect(settings_scrollbar_, &scrollbar);
    const int height = scrollbar.bottom - scrollbar.top;
    RECT viewport{};
    GetClientRect(settings_viewport_, &viewport);
    const int page = std::max(1, static_cast<int>(viewport.bottom));
    const int maximum = settings_scroll_maximum();
    const int minimum_thumb = scale(36);
    const int thumb_height = maximum == 0
        ? height
        : std::clamp(
              MulDiv(height, page, std::max(1, settings_content_height_)),
              std::min(height, minimum_thumb),
              height);
    const int travel = std::max(0, height - thumb_height);
    const int top = maximum == 0
        ? 0
        : MulDiv(travel, settings_scroll_, maximum);
    return {0, top, scrollbar.right, top + thumb_height};
}

void WindowsMainWindow::handle_settings_scrollbar_pointer(
    HWND control,
    UINT message,
    WPARAM,
    LPARAM lparam) {
    if (control == nullptr || view_ != View::Settings) {
        return;
    }
    const int y = GET_Y_LPARAM(lparam);
    const RECT thumb = settings_scrollbar_thumb();
    if (message == WM_LBUTTONDOWN) {
        SetCapture(control);
        POINT point{GET_X_LPARAM(lparam), y};
        if (PtInRect(&thumb, point)) {
            settings_scrollbar_dragging_ = true;
            settings_scrollbar_drag_offset_ = y - thumb.top;
        } else {
            handle_settings_scroll(y < thumb.top ? SB_PAGEUP : SB_PAGEDOWN);
        }
        return;
    }
    if (message == WM_MOUSEMOVE && settings_scrollbar_dragging_) {
        RECT scrollbar{};
        GetClientRect(control, &scrollbar);
        const int thumb_height = thumb.bottom - thumb.top;
        const int travel = std::max(
            0, static_cast<int>(scrollbar.bottom) - thumb_height);
        const int maximum = settings_scroll_maximum();
        const int top = std::clamp(
            y - settings_scrollbar_drag_offset_, 0, travel);
        const int next = travel == 0 ? 0 : MulDiv(maximum, top, travel);
        handle_settings_scroll(SB_THUMBTRACK, next);
        return;
    }
    if (message == WM_LBUTTONUP) {
        settings_scrollbar_dragging_ = false;
        if (GetCapture() == control) {
            ReleaseCapture();
        }
        return;
    }
    if (message == WM_CAPTURECHANGED) {
        settings_scrollbar_dragging_ = false;
    }
}

void WindowsMainWindow::render() {
    if (!state_) {
        return;
    }
    if (combo_box_dropped()) {
        return;
    }
    if (pending_profile_selection_) {
        if (state_->command_pending) {
            pending_profile_selection_started_ = true;
        } else if (pending_profile_selection_started_) {
            pending_profile_selection_.reset();
            pending_profile_selection_started_ = false;
        }
    }
    const auto previous_profile_field_count = profile_fields_.size();
    const auto previous_settings_field_count = settings_fields_.size();
    std::string error;
    if (!ensure_field_controls(error)) {
        set_local_status(utf8_to_wide(error), true);
    }
    const bool controls_added = profile_fields_.size() != previous_profile_field_count
        || settings_fields_.size() != previous_settings_field_count;
    updating_ = true;
    const auto status = utf8_to_wide(application_state_name(state_->application.state));
    if (control_text(status_text_) != status) {
        SetWindowTextW(status_text_, status.c_str());
    }
    const auto listener = listener_status_text(state_->application);
    if (control_text(listener_text_) != listener) {
        SetWindowTextW(listener_text_, listener.c_str());
    }
    Button_SetCheck(
        lightweight_checkbox_, state_->lightweight_mode ? BST_CHECKED : BST_UNCHECKED);
    render_profile_list();
    render_profile_details();
    render_rules_editor();
    render_settings();
    render_command_status();
    update_enabled_states();
    updating_ = false;
    if (controls_added) {
        update_view_visibility();
        layout_controls();
    }
}

void WindowsMainWindow::render_profile_list() {
    if (state_->profiles != rendered_profiles_) {
        SendMessageW(profile_list_, WM_SETREDRAW, FALSE, 0);
        (void)SendMessageW(profile_list_, LB_RESETCONTENT, 0, 0);
        (void)SendMessageW(rules_profile_combo_, CB_RESETCONTENT, 0, 0);
        for (const auto& profile : state_->profiles) {
            const auto id = utf8_to_wide(profile.id);
            (void)SendMessageW(profile_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(id.c_str()));
            (void)SendMessageW(rules_profile_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(id.c_str()));
        }
        rendered_profiles_ = state_->profiles;
        SendMessageW(profile_list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(profile_list_, nullptr, TRUE);
    }
    int selected_index = -1;
    const auto& selected_key = pending_profile_selection_
        ? pending_profile_selection_
        : state_->selected_profile_key;
    if (selected_key) {
        for (std::size_t index = 0; index < state_->profiles.size(); ++index) {
            if (state_->profiles[index].key == *selected_key) {
                selected_index = static_cast<int>(index);
                break;
            }
        }
    }
    if (SendMessageW(profile_list_, LB_GETCURSEL, 0, 0) != selected_index) {
        (void)SendMessageW(profile_list_, LB_SETCURSEL, selected_index, 0);
    }
    if (SendMessageW(rules_profile_combo_, CB_GETCURSEL, 0, 0) != selected_index
        && SendMessageW(rules_profile_combo_, CB_GETDROPPEDSTATE, 0, 0) == FALSE) {
        (void)SendMessageW(rules_profile_combo_, CB_SETCURSEL, selected_index, 0);
    }
}

void WindowsMainWindow::populate_field_controls(
    std::vector<FieldControl>& controls,
    const std::vector<ConfigurationFieldState>& fields) {
    for (auto& control : controls) {
        const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& state) {
            return state.key == control.state.key;
        });
        if (found == fields.end()) {
            continue;
        }
        control.state = *found;
        if (control.state.input_kind == ConfigurationFieldInputKind::Boolean) {
            const bool checked = control.state.value
                && std::holds_alternative<bool>(*control.state.value)
                && std::get<bool>(*control.state.value);
            Button_SetCheck(control.input, checked ? BST_CHECKED : BST_UNCHECKED);
        } else if (control.state.input_kind == ConfigurationFieldInputKind::Enumeration) {
            int index = control.state.required ? -1 : 0;
            if (control.state.value) {
                const auto value = field_value_text(*control.state.value);
                index = static_cast<int>(SendMessageW(
                    control.input, CB_FINDSTRINGEXACT, -1, reinterpret_cast<LPARAM>(value.c_str())));
            }
            if (SendMessageW(control.input, CB_GETCURSEL, 0, 0) != index
                && SendMessageW(control.input, CB_GETDROPPEDSTATE, 0, 0) == FALSE) {
                (void)SendMessageW(control.input, CB_SETCURSEL, index, 0);
            }
        } else {
            const auto value = control.state.value
                ? field_value_text(*control.state.value)
                : std::wstring{};
            set_control_text_if_changed(control.input, value);
        }
    }
}

void WindowsMainWindow::render_profile_details() {
    if (pending_profile_selection_) {
        return;
    }
    const auto* profile = selected_profile();
    if (profile == nullptr || !state_->profile_editor) {
        set_control_text_if_changed(rename_profile_edit_, L"");
        Button_SetCheck(enabled_checkbox_, BST_UNCHECKED);
        set_control_text_if_changed(readiness_value_, L"No selection");
        set_control_text_if_changed(profile_status_, L"Select or create a Profile.");
        rendered_profile_editor_.reset();
        return;
    }
    set_control_text_if_changed(readiness_value_, readiness_text(profile->readiness));
    std::wstring detail = L"Rules: "
        + std::to_wstring(profile->enabled_rule_count) + L" enabled / "
        + std::to_wstring(profile->rule_count) + L" total";
    if (!profile->status_detail.empty()) {
        detail += L"\r\n" + utf8_to_wide(profile->status_detail);
    }
    set_control_text_if_changed(profile_status_, detail);
    const bool editor_changed = rendered_profile_editor_ != state_->profile_editor;
    if (editor_changed || control_text(rename_profile_edit_).empty()) {
        const auto profile_id = utf8_to_wide(state_->profile_editor->profile_id);
        SetWindowTextW(rename_profile_edit_, profile_id.c_str());
    }
    if (!profile_local_dirty_) {
        const auto enabled = std::find_if(
            state_->profile_editor->fields.begin(),
            state_->profile_editor->fields.end(),
            [](const auto& field) { return field.key == "enabled"; });
        const bool checked = enabled != state_->profile_editor->fields.end()
            && enabled->value
            && std::get<bool>(*enabled->value);
        Button_SetCheck(enabled_checkbox_, checked ? BST_CHECKED : BST_UNCHECKED);
        populate_field_controls(profile_fields_, state_->profile_editor->fields);
        rendered_profile_editor_ = state_->profile_editor;
    }
}

void WindowsMainWindow::render_rules_editor() {
    if (!state_->rules_editor) {
        if (!rules_local_dirty_) set_control_text_if_changed(rules_edit_, L"");
        set_control_text_if_changed(
            rules_status_, L"Select a Profile to edit its request rules.");
        rendered_rules_editor_.reset();
        return;
    }
    if (!rules_local_dirty_ && rendered_rules_editor_ != state_->rules_editor) {
        const auto text = to_windows_edit_newlines(
            utf8_to_wide(state_->rules_editor->text));
        set_control_text_if_changed(rules_edit_, text);
        rendered_rules_editor_ = state_->rules_editor;
    }
    if (state_->rules_editor->diagnostic) {
        const auto& diagnostic = *state_->rules_editor->diagnostic;
        const auto message = L"Line " + std::to_wstring(diagnostic.line)
            + L", column " + std::to_wstring(diagnostic.column)
            + L": " + utf8_to_wide(diagnostic.message);
        set_control_text_if_changed(rules_status_, message);
    } else {
        set_control_text_if_changed(
            rules_status_, L"Canonical ccs-trans.rules/v1 JSON");
    }
}

void WindowsMainWindow::render_settings() {
    if (!settings_local_dirty_ && rendered_application_fields_ != state_->application_fields) {
        populate_field_controls(settings_fields_, state_->application_fields);
        rendered_application_fields_ = state_->application_fields;
    }
}

void WindowsMainWindow::render_command_status() {
    if (!local_status_.empty()) {
        set_control_text_if_changed(command_status_, local_status_);
        return;
    }
    if (pending_profile_selection_) {
        return;
    }
    if (state_->command_pending) {
        set_control_text_if_changed(command_status_, L"Working...");
        return;
    }
    if (!state_->last_command) {
        const auto text = state_->draft.dirty() ? L"Draft has unapplied changes" : L"Configuration is up to date";
        set_control_text_if_changed(command_status_, text);
        return;
    }
    const auto& result = *state_->last_command;
    if (!result.detail.empty()) {
        set_control_text_if_changed(command_status_, utf8_to_wide(result.detail));
    } else if (result.succeeded()) {
        set_control_text_if_changed(command_status_, L"Completed");
    } else if (result.outcome == CommandOutcome::SavedPendingRuntimeApply) {
        set_control_text_if_changed(
            command_status_, L"Saved; restart or reload is still required");
    } else {
        set_control_text_if_changed(command_status_, L"Command failed");
    }
}

void WindowsMainWindow::update_enabled_states() {
    const bool pending = state_->command_pending && !pending_profile_selection_;
    const auto actions = service_actions_for(state_->application.state);
    const auto set_enabled = [](HWND control, bool enabled) {
        if ((IsWindowEnabled(control) != FALSE) != enabled) {
            EnableWindow(control, enabled ? TRUE : FALSE);
        }
    };
    set_enabled(start_button_, !pending && actions.can_start);
    set_enabled(stop_button_, !pending && actions.can_stop);
    set_enabled(reload_button_, !pending && actions.can_reload);
    set_enabled(lightweight_checkbox_, !pending);
    set_enabled(profile_list_, !pending);
    set_enabled(rules_profile_combo_, !pending);
    set_enabled(new_profile_edit_, !pending);
    set_enabled(add_profile_button_, !pending);
    const bool has_profile = selected_profile() != nullptr;
    set_enabled(remove_profile_button_, !pending && has_profile);
    set_enabled(rename_profile_edit_, !pending && has_profile);
    set_enabled(rename_profile_button_, !pending && has_profile);
    set_enabled(enabled_checkbox_, !pending && has_profile);
    set_enabled(update_profile_fields_button_, !pending && has_profile);
    set_enabled(rules_edit_, !pending && has_profile);
    set_enabled(rules_format_button_, !pending && has_profile);
    set_enabled(rules_update_button_, !pending && has_profile);
    set_enabled(update_settings_button_, !pending && !settings_fields_.empty());
    for (const auto& field : profile_fields_) set_enabled(field.input, !pending && has_profile);
    for (const auto& field : settings_fields_) set_enabled(field.input, !pending);
    set_enabled(
        reload_draft_button_,
        !pending && state_->draft.loaded() && !state_->draft.busy());
    set_enabled(
        apply_button_,
        !pending && state_->draft.dirty()
            && !profile_local_dirty_ && !settings_local_dirty_ && !rules_local_dirty_);
    set_enabled(discard_button_, !pending && state_->draft.dirty());
}

void WindowsMainWindow::paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    FillRect(dc, &client, theme_.canvas_brush());
    const auto& palette = theme_.palette();
    HPEN separator = CreatePen(PS_SOLID, theme_.metrics().border, palette.border);
    const auto previous_pen = SelectObject(dc, separator);
    const int nav_right = scale(20 + kNavigationWidth - 28);
    const int content_left = scale(kNavigationWidth + 18);
    const int separator_x = (nav_right + content_left) / 2;
    const int separator_top = scale(kHeaderHeight);
    const int separator_bottom = client.bottom - scale(kFooterHeight);
    const int separator_right = client.right - scale(20);
    MoveToEx(dc, separator_x, separator_top, nullptr);
    LineTo(dc, separator_right, separator_top);
    MoveToEx(dc, separator_x, separator_top, nullptr);
    LineTo(dc, separator_x, separator_bottom);
    MoveToEx(dc, separator_x, separator_bottom, nullptr);
    LineTo(dc, separator_right, separator_bottom);
    SelectObject(dc, previous_pen);
    DeleteObject(separator);
}

bool WindowsMainWindow::draw_item(const DRAWITEMSTRUCT& item) {
    if (item.CtlType == ODT_STATIC && item.hwndItem == status_text_) {
        draw_status(item);
        return true;
    }
    if (item.CtlType == ODT_STATIC && item.hwndItem == settings_scrollbar_) {
        draw_settings_scrollbar(item);
        return true;
    }
    if (item.CtlType == ODT_BUTTON) {
        draw_button(item);
        return true;
    }
    if (item.CtlType == ODT_LISTBOX && item.CtlID == kMainProfileListId) {
        draw_profile_item(item);
        return true;
    }
    if (item.CtlType == ODT_COMBOBOX) {
        draw_combo_item(item);
        return true;
    }
    return false;
}

void WindowsMainWindow::draw_settings_scrollbar(const DRAWITEMSTRUCT& item) {
    const auto& palette = theme_.palette();
    FillRect(item.hDC, &item.rcItem, theme_.canvas_brush());
    RECT track = item.rcItem;
    const int track_width = std::max(scale(3), theme_.metrics().border * 2);
    track.left = (item.rcItem.left + item.rcItem.right - track_width) / 2;
    track.right = track.left + track_width;
    theme_.fill_rounded_rectangle(
        item.hDC,
        track,
        track_width / 2,
        palette.surface_subtle);
    RECT thumb = settings_scrollbar_thumb();
    thumb.left = item.rcItem.left;
    thumb.right = item.rcItem.right;
    InflateRect(&thumb, -scale(1), 0);
    theme_.fill_rounded_rectangle(
        item.hDC,
        thumb,
        std::max(1, static_cast<int>(thumb.right - thumb.left) / 2),
        palette.text_muted);
}

void WindowsMainWindow::draw_rules_scroll_indicator(HDC dc) {
    if (!rules_scroll_indicator_visible_ || rules_edit_ == nullptr || dc == nullptr) {
        return;
    }
    const int line_count = std::max(
        1, static_cast<int>(SendMessageW(rules_edit_, EM_GETLINECOUNT, 0, 0)));
    RECT format{};
    (void)SendMessageW(
        rules_edit_, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&format));
    const auto old_font = SelectObject(dc, mono_font_);
    TEXTMETRICW metrics{};
    (void)GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old_font);
    const int line_height = std::max(
        1, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
    const int visible_lines = std::max(
        1, static_cast<int>(format.bottom - format.top) / line_height);
    const int maximum = std::max(0, line_count - visible_lines);
    if (maximum == 0) {
        return;
    }

    RECT client{};
    GetClientRect(rules_edit_, &client);
    RECT track{
        client.right - scale(9),
        format.top + scale(6),
        client.right - scale(5),
        format.bottom - scale(6),
    };
    if (track.bottom <= track.top) {
        return;
    }
    const int track_height = track.bottom - track.top;
    const int thumb_height = std::clamp(
        MulDiv(track_height, visible_lines, line_count),
        std::min(track_height, scale(28)),
        track_height);
    const int travel = std::max(0, track_height - thumb_height);
    const int first_line = std::clamp(
        static_cast<int>(SendMessageW(rules_edit_, EM_GETFIRSTVISIBLELINE, 0, 0)),
        0,
        maximum);
    const int thumb_top = track.top + MulDiv(travel, first_line, maximum);
    RECT thumb{track.left, thumb_top, track.right, thumb_top + thumb_height};
    theme_.fill_rounded_rectangle(
        dc,
        thumb,
        std::max(1, static_cast<int>(thumb.right - thumb.left) / 2),
        theme_.palette().text_muted);
}

void WindowsMainWindow::show_rules_scroll_indicator() {
    if (rules_edit_ == nullptr || window_ == nullptr) {
        return;
    }
    rules_scroll_indicator_visible_ = true;
    (void)SetTimer(
        window_,
        kRulesScrollIndicatorTimerId,
        kRulesScrollIndicatorDurationMs,
        nullptr);
    InvalidateRect(rules_edit_, nullptr, FALSE);
}

void WindowsMainWindow::hide_rules_scroll_indicator() {
    if (window_ != nullptr) {
        KillTimer(window_, kRulesScrollIndicatorTimerId);
    }
    if (!rules_scroll_indicator_visible_) {
        return;
    }
    rules_scroll_indicator_visible_ = false;
    InvalidateRect(rules_edit_, nullptr, FALSE);
}

void WindowsMainWindow::draw_status(const DRAWITEMSTRUCT& item) {
    const auto& palette = theme_.palette();
    COLORREF text = palette.text_muted;
    if (state_) {
        if (state_->application.state == ApplicationState::Running) {
            text = palette.success;
        } else if (state_->application.state == ApplicationState::Faulted) {
            text = palette.danger;
        } else if (state_->application.state == ApplicationState::Starting
                   || state_->application.state == ApplicationState::Reloading
                   || state_->application.state == ApplicationState::Stopping) {
            text = palette.warning;
        }
    }

    FillRect(item.hDC, &item.rcItem, theme_.canvas_brush());
    theme_.fill_rounded_rectangle(
        item.hDC,
        item.rcItem,
        theme_.metrics().radius_large,
        palette.surface_subtle);

    wchar_t label[64]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, text);
    const auto old_font = SelectObject(item.hDC, semibold_font_);
    RECT text_rectangle = item.rcItem;
    DrawTextW(
        item.hDC,
        label,
        -1,
        &text_rectangle,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(item.hDC, old_font);
}

void WindowsMainWindow::draw_toggle(
    HDC dc,
    const RECT& rectangle,
    bool checked,
    bool enabled) {
    const auto& palette = theme_.palette();
    RECT track = rectangle;
    track.left += scale(8);
    track.right = track.left + scale(38);
    const int track_height = scale(kToggleTrackHeight);
    track.top = rectangle.top
        + (rectangle.bottom - rectangle.top - track_height) / 2;
    track.bottom = track.top + track_height;
    const COLORREF track_color = !enabled
        ? palette.disabled
        : (checked ? palette.accent : palette.border);
    theme_.fill_rounded_rectangle(
        dc,
        track,
        (track.bottom - track.top) / 2,
        track_color);
    const int diameter = scale(16);
    const int circle_left = checked
        ? track.right - diameter - scale(3)
        : track.left + scale(3);
    RECT circle{
        circle_left,
        track.top + (track.bottom - track.top - diameter) / 2,
        circle_left + diameter,
        track.top + (track.bottom - track.top - diameter) / 2 + diameter,
    };
    theme_.fill_ellipse(
        dc,
        circle,
        checked ? palette.accent_text : palette.surface);
}

void WindowsMainWindow::draw_button(const DRAWITEMSTRUCT& item) {
    const auto& palette = theme_.palette();
    FillRect(item.hDC, &item.rcItem, theme_.canvas_brush());
    RECT rectangle = item.rcItem;
    InflateRect(&rectangle, -1, -1);
    const bool enabled = (item.itemState & ODS_DISABLED) == 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const int id = static_cast<int>(item.CtlID);
    bool selected_navigation = false;
    if (id == kMainNavProfilesId) selected_navigation = view_ == View::Profiles;
    if (id == kMainNavRulesId) selected_navigation = view_ == View::Rules;
    if (id == kMainNavSettingsId) selected_navigation = view_ == View::Settings;
    if (pressed) InflateRect(&rectangle, -1, -1);

    COLORREF fill = palette.surface;
    COLORREF text = enabled ? palette.text : palette.disabled;
    if (is_primary_id(id) && enabled) {
        fill = palette.accent;
        text = palette.accent_text;
    } else if (selected_navigation) {
        fill = palette.surface_subtle;
        text = palette.accent;
    } else if ((id == kMainAddProfileId || id == kMainRemoveProfileId) && enabled) {
        fill = palette.surface_subtle;
        text = id == kMainRemoveProfileId ? palette.danger : palette.text;
    } else if (is_toggle_id(id)) {
        fill = palette.canvas;
    }

    theme_.draw_rounded_rectangle(
        item.hDC,
        rectangle,
        theme_.metrics().radius,
        fill,
        selected_navigation || is_primary_id(id) ? fill : palette.border,
        theme_.metrics().border);

    wchar_t label[128]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, text);
    const auto old_font = SelectObject(
        item.hDC,
        is_navigation_id(id) || is_primary_id(id) ? semibold_font_ : normal_font_);
    RECT text_rectangle = rectangle;
    UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    if (is_toggle_id(id)) {
        const bool checked = Button_GetCheck(item.hwndItem) == BST_CHECKED;
        draw_toggle(item.hDC, rectangle, checked, enabled);
        text_rectangle.left += scale(56);
        format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    } else if (is_navigation_id(id)) {
        text_rectangle.left += scale(14);
        format = DT_LEFT | DT_VCENTER | DT_SINGLELINE;
    }
    DrawTextW(item.hDC, label, -1, &text_rectangle, format);
    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus = rectangle;
        InflateRect(&focus, -2, -2);
        theme_.stroke_rounded_rectangle(
            item.hDC,
            focus,
            theme_.metrics().radius,
            palette.accent,
            scale(2));
    }
    SelectObject(item.hDC, old_font);
}

void WindowsMainWindow::draw_profile_item(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1)
        || item.itemID >= state_->profiles.size()) {
        return;
    }
    const auto& profile = state_->profiles[item.itemID];
    const auto& palette = theme_.palette();
    FillRect(item.hDC, &item.rcItem, theme_.surface_brush());
    RECT row = item.rcItem;
    row.left += scale(5);
    row.right -= scale(5);
    row.top += scale(4) - (item.itemID > 0 ? scale(1) : 0);
    row.bottom -= scale(4)
        - (item.itemID + 1 < state_->profiles.size() ? scale(2) : 0);
    if ((item.itemState & ODS_SELECTED) != 0) {
        theme_.fill_rounded_rectangle(
            item.hDC,
            row,
            theme_.metrics().radius,
            palette.surface_subtle);
    }
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, palette.text);
    const auto old_font = SelectObject(item.hDC, semibold_font_);
    RECT name = row;
    name.left += scale(10);
    name.right -= scale(72);
    name.bottom = name.top + scale(24);
    const auto wide_id = utf8_to_wide(profile.id);
    DrawTextW(item.hDC, wide_id.c_str(), -1, &name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(item.hDC, normal_font_);
    SetTextColor(item.hDC, palette.text_muted);
    RECT detail = row;
    detail.left += scale(10);
    detail.top += scale(24);
    detail.right -= scale(10);
    std::wstring detail_text = profile.protocol
        ? utf8_to_wide(*profile.protocol)
        : std::wstring{L"Not configured"};
    detail_text += L"  |  ";
    detail_text += readiness_text(profile.readiness);
    DrawTextW(item.hDC, detail_text.c_str(), -1, &detail, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const COLORREF indicator = !profile.enabled
        ? palette.disabled
        : (profile.readiness == ProfileReadiness::Ready ? palette.success : palette.warning);
    const int diameter = scale(9);
    const RECT indicator_rectangle{
        row.right - scale(22),
        row.top + scale(10),
        row.right - scale(22) + diameter,
        row.top + scale(10) + diameter,
    };
    theme_.fill_ellipse(item.hDC, indicator_rectangle, indicator);
    SelectObject(item.hDC, old_font);
}

void WindowsMainWindow::draw_combo_item(const DRAWITEMSTRUCT& item) {
    const auto& palette = theme_.palette();
    FillRect(item.hDC, &item.rcItem, theme_.surface_brush());
    if (item.itemID == static_cast<UINT>(-1)) {
        return;
    }

    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    RECT row = item.rcItem;
    InflateRect(&row, -scale(3), -scale(2));
    if (selected) {
        theme_.fill_rounded_rectangle(
            item.hDC,
            row,
            std::max(scale(4), theme_.metrics().radius / 2),
            palette.surface_subtle);
    }

    const int length = static_cast<int>(
        SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0));
    std::wstring text;
    if (length > 0) {
        text.resize(static_cast<std::size_t>(length) + 1);
        (void)SendMessageW(
            item.hwndItem,
            CB_GETLBTEXT,
            item.itemID,
            reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<std::size_t>(length));
    }
    row.left += scale(10);
    row.right -= scale(8);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(
        item.hDC,
        (item.itemState & ODS_DISABLED) != 0
            ? palette.disabled
            : (selected ? palette.accent : palette.text));
    const auto old_font = SelectObject(item.hDC, normal_font_);
    DrawTextW(
        item.hDC,
        text.c_str(),
        static_cast<int>(text.size()),
        &row,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(item.hDC, old_font);
}

bool WindowsMainWindow::collect_field_edits(
    const std::vector<FieldControl>& controls,
    std::vector<ConfigurationFieldEdit>& edits,
    std::wstring& error) const {
    edits.clear();
    error.clear();
    edits.reserve(controls.size());
    for (const auto& control : controls) {
        ConfigurationFieldEdit edit;
        edit.key = control.state.key;
        if (control.state.input_kind == ConfigurationFieldInputKind::Boolean) {
            edit.value = Button_GetCheck(control.input) == BST_CHECKED;
        } else {
            const auto text = wide_to_utf8(control_text(control.input));
            if (text.empty() && !control.state.required) {
                edit.value.reset();
            } else {
                const auto* descriptor = find_configuration_field_descriptor(
                    control.state.scope, control.state.key);
                ConfigurationFieldValue value;
                std::string parse_error;
                if (descriptor == nullptr
                    || !parse_configuration_field_value(*descriptor, text, value, parse_error)) {
                    error = std::wstring(field_display_name(control.state.display_name_key))
                        + L": " + utf8_to_wide(parse_error);
                    SetFocus(control.input);
                    return false;
                }
                edit.value = std::move(value);
            }
        }
        edits.push_back(std::move(edit));
    }
    return true;
}

void WindowsMainWindow::handle_profile_selection() {
    if (updating_) {
        return;
    }
    const int index = static_cast<int>(SendMessageW(profile_list_, LB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<std::size_t>(index) >= state_->profiles.size()) {
        return;
    }
    profile_local_dirty_ = false;
    rules_local_dirty_ = false;
    const auto& selected = state_->profiles[static_cast<std::size_t>(index)];
    const auto selected_id = utf8_to_wide(selected.id);
    SetWindowTextW(rename_profile_edit_, selected_id.c_str());
    pending_profile_selection_ = selected.key;
    pending_profile_selection_started_ = false;
    MainWindowCommandRequest request;
    request.command = MainWindowCommand::SelectProfile;
    request.profile_id = selected.id;
    request.profile_key = selected.key;
    if (!submit(std::move(request))) {
        pending_profile_selection_.reset();
    }
}

void WindowsMainWindow::handle_rules_profile_selection() {
    if (updating_) {
        return;
    }
    const int index = static_cast<int>(SendMessageW(rules_profile_combo_, CB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<std::size_t>(index) >= state_->profiles.size()) {
        return;
    }
    rules_local_dirty_ = false;
    pending_profile_selection_ = state_->profiles[static_cast<std::size_t>(index)].key;
    pending_profile_selection_started_ = false;
    MainWindowCommandRequest request;
    request.command = MainWindowCommand::SelectProfile;
    request.profile_id = state_->profiles[static_cast<std::size_t>(index)].id;
    request.profile_key = state_->profiles[static_cast<std::size_t>(index)].key;
    if (!submit(std::move(request))) {
        pending_profile_selection_.reset();
    }
}

void WindowsMainWindow::handle_command(int id, int notification_code) {
    if (id >= kMainProfileFieldBaseId && id < kMainSettingsFieldBaseId) {
        if (notification_code == CBN_DROPDOWN) {
            configure_combo_box(GetDlgItem(window_, id));
        }
        if (!updating_ && (notification_code == EN_CHANGE
                || notification_code == CBN_SELCHANGE
                || notification_code == BN_CLICKED)) {
            profile_local_dirty_ = true;
            update_enabled_states();
        }
        return;
    }
    if (id >= kMainSettingsFieldBaseId && id < kMainSettingsFieldBaseId + 100) {
        if (notification_code == CBN_DROPDOWN) {
            configure_combo_box(GetDlgItem(settings_content_, id));
        }
        if (!updating_ && (notification_code == EN_CHANGE
                || notification_code == CBN_SELCHANGE
                || notification_code == BN_CLICKED)) {
            settings_local_dirty_ = true;
            update_enabled_states();
        }
        return;
    }
    switch (id) {
    case kMainNavProfilesId:
        if (notification_code == BN_CLICKED) set_view(View::Profiles);
        break;
    case kMainNavRulesId:
        if (notification_code == BN_CLICKED) set_view(View::Rules);
        break;
    case kMainNavSettingsId:
        if (notification_code == BN_CLICKED) set_view(View::Settings);
        break;
    case kMainServiceStartId:
        if (notification_code == BN_CLICKED) submit({MainWindowCommand::StartService});
        break;
    case kMainServiceStopId:
        if (notification_code == BN_CLICKED) submit({MainWindowCommand::StopService});
        break;
    case kMainServiceReloadId:
        if (notification_code == BN_CLICKED) submit({MainWindowCommand::ReloadService});
        break;
    case kMainLightweightId:
        if (notification_code == BN_CLICKED) {
            submit({
                MainWindowCommand::SetLightweightMode,
                {},
                {},
                Button_GetCheck(lightweight_checkbox_) == BST_CHECKED,
            });
        }
        break;
    case kMainProfileListId:
        if (notification_code == LBN_SELCHANGE) handle_profile_selection();
        break;
    case kMainRulesProfileId:
        if (notification_code == CBN_DROPDOWN) {
            configure_combo_box(rules_profile_combo_);
        }
        if (notification_code == CBN_SELCHANGE) handle_rules_profile_selection();
        break;
    case kMainAddProfileId:
        if (notification_code == BN_CLICKED) {
            const auto id_value = control_text(new_profile_edit_);
            if (id_value.empty()) {
                set_local_status(L"Enter a Profile ID before adding it.", true);
            } else {
                submit({MainWindowCommand::CreateProfile, wide_to_utf8(id_value)});
                SetWindowTextW(new_profile_edit_, L"");
            }
        }
        break;
    case kMainRemoveProfileId:
        if (notification_code == BN_CLICKED && state_->selected_profile_id) {
            if (MessageBoxW(
                    window_,
                    L"Remove the selected Profile from this draft?",
                    L"Remove Profile",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)
                == IDYES) {
                MainWindowCommandRequest request{
                    MainWindowCommand::RemoveProfile, *state_->selected_profile_id};
                request.profile_key = state_->selected_profile_key;
                submit(std::move(request));
            }
        }
        break;
    case kMainRenameProfileId:
        if (notification_code == BN_CLICKED && state_->selected_profile_id) {
            const auto replacement = control_text(rename_profile_edit_);
            if (replacement.empty()) {
                set_local_status(L"Profile ID cannot be empty.", true);
            } else {
                profile_local_dirty_ = false;
                rendered_profile_editor_.reset();
                MainWindowCommandRequest request{
                    MainWindowCommand::RenameProfile,
                    *state_->selected_profile_id,
                    wide_to_utf8(replacement)};
                request.profile_key = state_->selected_profile_key;
                submit(std::move(request));
            }
        }
        break;
    case kMainProfileEnabledId:
        if (notification_code == BN_CLICKED && state_->selected_profile_id) {
            profile_local_dirty_ = false;
            rendered_profile_editor_.reset();
            MainWindowCommandRequest request{
                MainWindowCommand::SetProfileEnabled,
                *state_->selected_profile_id,
                {},
                Button_GetCheck(enabled_checkbox_) == BST_CHECKED};
            request.profile_key = state_->selected_profile_key;
            submit(std::move(request));
        }
        break;
    case kMainUpdateProfileFieldsId:
        if (notification_code == BN_CLICKED && state_->selected_profile_id) {
            std::vector<ConfigurationFieldEdit> edits;
            std::wstring error;
            if (!collect_field_edits(profile_fields_, edits, error)) {
                set_local_status(error, true);
                break;
            }
            profile_local_dirty_ = false;
            MainWindowCommandRequest request;
            request.command = MainWindowCommand::UpdateProfileFields;
            request.profile_id = *state_->selected_profile_id;
            request.profile_key = state_->selected_profile_key;
            request.field_edits = std::move(edits);
            submit(std::move(request));
        }
        break;
    case kMainRulesEditId:
        if (!updating_ && notification_code == EN_CHANGE) {
            rules_local_dirty_ = true;
            update_enabled_states();
        }
        break;
    case kMainRulesFormatId:
    case kMainRulesUpdateId:
        if (notification_code == BN_CLICKED && state_->selected_profile_key) {
            rules_local_dirty_ = false;
            MainWindowCommandRequest request;
            request.command = id == kMainRulesFormatId
                ? MainWindowCommand::FormatRulesText
                : MainWindowCommand::ReplaceRulesText;
            request.profile_id = state_->selected_profile_id.value_or("");
            request.profile_key = state_->selected_profile_key;
            request.text = wide_to_utf8(
                to_canonical_newlines(control_text(rules_edit_)));
            submit(std::move(request));
        }
        break;
    case kMainUpdateSettingsId:
        if (notification_code == BN_CLICKED) {
            std::vector<ConfigurationFieldEdit> edits;
            std::wstring error;
            if (!collect_field_edits(settings_fields_, edits, error)) {
                set_local_status(error, true);
                break;
            }
            settings_local_dirty_ = false;
            MainWindowCommandRequest request;
            request.command = MainWindowCommand::UpdateApplicationFields;
            request.field_edits = std::move(edits);
            submit(std::move(request));
        }
        break;
    case kMainApplyId:
        if (notification_code == BN_CLICKED) {
            if (profile_local_dirty_ || settings_local_dirty_ || rules_local_dirty_) {
                set_local_status(L"Update the current editor before applying the draft.", true);
            } else {
                submit({MainWindowCommand::ApplyDraft});
            }
        }
        break;
    case kMainReloadDraftId:
        if (notification_code == BN_CLICKED) {
            if (!state_->draft.dirty()
                && !profile_local_dirty_ && !settings_local_dirty_ && !rules_local_dirty_) {
                submit({MainWindowCommand::ReloadDraft});
                break;
            }
            if (MessageBoxW(
                    window_,
                    L"Discard local edits and reload the configuration from disk?",
                    L"Reload Profile Configuration",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)
                == IDYES) {
                profile_local_dirty_ = false;
                settings_local_dirty_ = false;
                rules_local_dirty_ = false;
                rendered_profile_editor_.reset();
                rendered_application_fields_.clear();
                rendered_rules_editor_.reset();
                submit({
                    MainWindowCommand::ReloadDraft,
                    {},
                    {},
                    false,
                    UnsavedChangesDecision::Discard,
                });
            }
        }
        break;
    case kMainDiscardId:
        if (notification_code == BN_CLICKED) {
            profile_local_dirty_ = false;
            settings_local_dirty_ = false;
            rules_local_dirty_ = false;
            rendered_profile_editor_.reset();
            rendered_application_fields_.clear();
            rendered_rules_editor_.reset();
            submit({MainWindowCommand::DiscardDraft});
        }
        break;
    default:
        break;
    }
}

void WindowsMainWindow::handle_settings_scroll(int request, int thumb_position) {
    const int row_height = scale(48);
    const int page = settings_page_rows() * row_height;
    int next = settings_scroll_target_;
    switch (request) {
    case SB_LINEUP: next -= row_height; break;
    case SB_LINEDOWN: next += row_height; break;
    case SB_PAGEUP: next -= page; break;
    case SB_PAGEDOWN: next += page; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK:
        next = ((thumb_position + row_height / 2) / row_height) * row_height;
        break;
    case SB_TOP: next = 0; break;
    case SB_BOTTOM: next = settings_scroll_maximum(); break;
    default: return;
    }
    const int maximum = settings_scroll_maximum();
    next = std::clamp(next, 0, maximum);
    settings_scroll_target_ = next;
    if (request == SB_THUMBTRACK || request == SB_THUMBPOSITION) {
        KillTimer(window_, kSettingsScrollTimerId);
        if (settings_scroll_ == settings_scroll_target_) {
            return;
        }
        settings_scroll_ = settings_scroll_target_;
        position_settings_content(true);
        InvalidateRect(settings_scrollbar_, nullptr, FALSE);
        return;
    }
    if (settings_scroll_ != settings_scroll_target_) {
        (void)SetTimer(
            window_,
            kSettingsScrollTimerId,
            kSettingsScrollIntervalMs,
            nullptr);
    }
}

void WindowsMainWindow::advance_settings_scroll_animation() {
    const int distance = settings_scroll_target_ - settings_scroll_;
    if (distance == 0) {
        KillTimer(window_, kSettingsScrollTimerId);
        return;
    }
    const int magnitude = std::abs(distance);
    const int step = std::max(1, (magnitude + 2) / 3);
    if (distance > 0) {
        settings_scroll_ = std::min(settings_scroll_ + step, settings_scroll_target_);
    } else {
        settings_scroll_ = std::max(settings_scroll_ - step, settings_scroll_target_);
    }
    position_settings_content(true);
    InvalidateRect(settings_scrollbar_, nullptr, FALSE);
    if (settings_scroll_ == settings_scroll_target_) {
        KillTimer(window_, kSettingsScrollTimerId);
    }
}

bool WindowsMainWindow::submit(MainWindowCommandRequest request) {
    local_status_.clear();
    local_status_is_error_ = false;
    const bool accepted = view_model_.submit(std::move(request));
    if (!accepted) {
        set_local_status(L"Another command is already running.", true);
    }
    return accepted;
}

void WindowsMainWindow::request_window_close() {
    if (!state_) {
        perform_close(CloseTarget::Destroy);
        return;
    }
    if (profile_local_dirty_ || settings_local_dirty_ || rules_local_dirty_) {
        const int choice = MessageBoxW(
            window_,
            L"Discard edits that have not been added to the draft?",
            L"Uncommitted editor changes",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (choice != IDYES) {
            return;
        }
        profile_local_dirty_ = false;
        settings_local_dirty_ = false;
        rules_local_dirty_ = false;
    }
    const auto action = resolve_main_window_close(
        state_->draft, state_->lightweight_mode, std::nullopt);
    if (action == MainWindowCloseAction::Hide) {
        perform_close(CloseTarget::Hide);
    } else if (action == MainWindowCloseAction::Destroy) {
        perform_close(CloseTarget::Destroy);
    } else if (action == MainWindowCloseAction::KeepOpen) {
        set_local_status(L"Wait for the current command to finish.", true);
    } else {
        (void)request_close(
            state_->lightweight_mode ? CloseTarget::Destroy : CloseTarget::Hide);
    }
}

bool WindowsMainWindow::request_close(
    CloseTarget target,
    std::function<void()> continuation) {
    if (!state_) {
        return true;
    }
    if (state_->draft.busy() || state_->command_pending) {
        set_local_status(L"Wait for the current command to finish.", true);
        return false;
    }
    if (!state_->draft.dirty()) {
        return true;
    }
    const auto decision = prompt_unsaved_changes();
    if (!decision || *decision == UnsavedChangesDecision::Cancel) {
        return false;
    }
    const auto command = *decision == UnsavedChangesDecision::Apply
        ? MainWindowCommand::ApplyDraft
        : MainWindowCommand::DiscardDraft;
    const auto previous_sequence = state_->last_command
        ? state_->last_command->sequence
        : 0;
    if (!view_model_.submit({command})) {
        set_local_status(L"Another command is already running.", true);
        return false;
    }
    pending_close_ = PendingClose{
        target,
        command,
        previous_sequence,
        std::move(continuation),
    };
    return false;
}

void WindowsMainWindow::finish_pending_close() {
    if (pending_close_.target == CloseTarget::None
        || !state_
        || state_->command_pending
        || !state_->last_command
        || state_->last_command->sequence <= pending_close_.previous_sequence
        || state_->last_command->command != pending_close_.command) {
        return;
    }
    auto pending = std::move(pending_close_);
    pending_close_ = {};
    const bool completed = state_->last_command->succeeded()
        || (pending.command == MainWindowCommand::ApplyDraft
            && state_->last_command->configuration_saved());
    if (completed) {
        perform_close(pending.target, std::move(pending.continuation));
    }
}

void WindowsMainWindow::perform_close(
    CloseTarget target,
    std::function<void()> continuation) {
    if (target == CloseTarget::Hide) {
        ShowWindow(window_, SW_HIDE);
        notify_lifecycle("hidden");
    } else if (target == CloseTarget::Destroy) {
        destroy();
    } else if (target == CloseTarget::ExitApplication && continuation) {
        continuation();
    }
}

std::optional<UnsavedChangesDecision> WindowsMainWindow::prompt_unsaved_changes() {
    const int choice = MessageBoxW(
        window_,
        L"Apply changes before closing?\n\nYes: Apply\nNo: Discard\nCancel: Keep editing",
        L"Unsaved Profile changes",
        MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON1);
    if (choice == IDYES) return UnsavedChangesDecision::Apply;
    if (choice == IDNO) return UnsavedChangesDecision::Discard;
    return UnsavedChangesDecision::Cancel;
}

void WindowsMainWindow::set_local_status(
    const std::wstring& message,
    bool error) {
    local_status_ = message;
    local_status_is_error_ = error;
    if (command_status_ != nullptr) {
        set_control_text_if_changed(command_status_, local_status_);
    }
}

std::wstring WindowsMainWindow::control_text(HWND control) const {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    (void)GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

void WindowsMainWindow::set_control_text_if_changed(
    HWND control,
    std::wstring_view text) const {
    if (control == nullptr || control_text(control) == text) {
        return;
    }
    const std::wstring value(text);
    SetWindowTextW(control, value.c_str());
}

bool WindowsMainWindow::combo_box_dropped() const noexcept {
    const auto dropped = [](HWND control) {
        return control != nullptr
            && SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
    };
    if (dropped(rules_profile_combo_)) {
        return true;
    }
    for (const auto& field : profile_fields_) {
        if (field.state.input_kind == ConfigurationFieldInputKind::Enumeration
            && dropped(field.input)) {
            return true;
        }
    }
    for (const auto& field : settings_fields_) {
        if (field.state.input_kind == ConfigurationFieldInputKind::Enumeration
            && dropped(field.input)) {
            return true;
        }
    }
    return false;
}

const ProfileListItem* WindowsMainWindow::selected_profile() const noexcept {
    if (!state_ || !state_->selected_profile_key) {
        return nullptr;
    }
    return find_profile_list_item(*state_, *state_->selected_profile_key);
}

int WindowsMainWindow::scale(int logical_pixels) const noexcept {
    return MulDiv(logical_pixels, static_cast<int>(dpi_), 96);
}

void WindowsMainWindow::notify_lifecycle(std::string_view event) const {
    if (lifecycle_handler_) {
        lifecycle_handler_(event);
    }
}

void WindowsMainWindow::reset_handles() noexcept {
    brand_text_ = nullptr;
    status_text_ = nullptr;
    listener_text_ = nullptr;
    start_button_ = nullptr;
    stop_button_ = nullptr;
    reload_button_ = nullptr;
    lightweight_checkbox_ = nullptr;
    nav_profiles_ = nullptr;
    nav_rules_ = nullptr;
    nav_settings_ = nullptr;
    profile_list_ = nullptr;
    new_profile_edit_ = nullptr;
    add_profile_button_ = nullptr;
    remove_profile_button_ = nullptr;
    details_title_ = nullptr;
    profile_id_label_ = nullptr;
    rename_profile_edit_ = nullptr;
    rename_profile_button_ = nullptr;
    enabled_checkbox_ = nullptr;
    readiness_label_ = nullptr;
    readiness_value_ = nullptr;
    profile_status_ = nullptr;
    update_profile_fields_button_ = nullptr;
    profile_fields_.clear();
    rules_title_ = nullptr;
    rules_profile_combo_ = nullptr;
    rules_edit_ = nullptr;
    rules_format_button_ = nullptr;
    rules_update_button_ = nullptr;
    rules_status_ = nullptr;
    settings_title_ = nullptr;
    settings_subtitle_ = nullptr;
    update_settings_button_ = nullptr;
    settings_viewport_ = nullptr;
    settings_content_ = nullptr;
    settings_scrollbar_ = nullptr;
    settings_fields_.clear();
    command_status_ = nullptr;
    reload_draft_button_ = nullptr;
    apply_button_ = nullptr;
    discard_button_ = nullptr;
    tooltip_ = nullptr;
    rendered_profiles_.clear();
    rendered_profile_editor_.reset();
    rendered_application_fields_.clear();
    rendered_rules_editor_.reset();
    settings_scrollbar_dragging_ = false;
    settings_scrollbar_drag_offset_ = 0;
    settings_scroll_ = 0;
    settings_scroll_target_ = 0;
    settings_content_height_ = 0;
    rules_wheel_remainder_ = 0;
    rules_scroll_indicator_visible_ = false;
    pending_profile_selection_.reset();
    pending_profile_selection_started_ = false;
}

LRESULT CALLBACK WindowsMainWindow::input_subclass_proc(
    HWND control,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR reference_data) {
    auto* main_window = reinterpret_cast<WindowsMainWindow*>(reference_data);
    wchar_t class_name[32]{};
    (void)GetClassNameW(control, class_name, static_cast<int>(std::size(class_name)));
    const bool combo_box = lstrcmpiW(class_name, L"ComboBox") == 0;
    if (message == WM_MOUSEWHEEL) {
        const bool combo_dropped = combo_box
            && SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
        if (main_window != nullptr
            && main_window->view_ == View::Settings
            && !combo_dropped) {
            (void)SendMessageW(
                main_window->window_, WM_MOUSEWHEEL, wparam, lparam);
            return 0;
        }
        if (main_window != nullptr && control == main_window->rules_edit_) {
            main_window->rules_wheel_remainder_ += GET_WHEEL_DELTA_WPARAM(wparam);
            const int notches = main_window->rules_wheel_remainder_ / WHEEL_DELTA;
            if (notches != 0) {
                main_window->rules_wheel_remainder_ -= notches * WHEEL_DELTA;
                UINT lines = 3;
                (void)SystemParametersInfoW(
                    SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                const int line_count = lines == WHEEL_PAGESCROLL
                    ? 8
                    : std::max(1, static_cast<int>(lines));
                (void)SendMessageW(
                    control, EM_LINESCROLL, 0, -notches * line_count);
                main_window->show_rules_scroll_indicator();
            }
            return 0;
        }
        if (combo_box && !combo_dropped) {
            return 0;
        }
    }
    if (message == WM_KEYDOWN && main_window != nullptr
        && control == main_window->rules_edit_
        && (wparam == VK_UP || wparam == VK_DOWN
            || wparam == VK_PRIOR || wparam == VK_NEXT
            || wparam == VK_HOME || wparam == VK_END)) {
        const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
        main_window->show_rules_scroll_indicator();
        return result;
    }
    if (message == WM_CHAR && main_window != nullptr
        && control != main_window->rules_edit_
        && (wparam == L'\r' || wparam == L'\n')) {
        const HWND tab_parent = GetParent(control) == main_window->settings_content_
            ? main_window->settings_content_
            : main_window->window_;
        const HWND next = GetNextDlgTabItem(
            tab_parent, control, FALSE);
        if (next != nullptr) {
            SetFocus(next);
        }
        return 0;
    }
    if (!combo_box && (message == WM_SIZE || message == WM_SETTEXT
            || message == WM_SETFONT)) {
        const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
        if (main_window != nullptr) {
            main_window->update_input_metrics(control);
        }
        return result;
    }
    if (message == WM_ERASEBKGND && main_window != nullptr) {
        RECT rectangle{};
        GetClientRect(control, &rectangle);
        FillRect(
            reinterpret_cast<HDC>(wparam),
            &rectangle,
            combo_box
                ? main_window->theme_.canvas_brush()
                : main_window->theme_.surface_brush());
        return 1;
    }
    if (message == WM_PAINT && combo_box && main_window != nullptr) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(control, &paint);
        main_window->draw_combo_box(control, dc);
        EndPaint(control, &paint);
        return 0;
    }
    if (message == WM_PAINT) {
        const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
        const HDC dc = GetDC(control);
        if (main_window != nullptr && dc != nullptr) {
            main_window->draw_input_frame(control, dc);
            if (control == main_window->rules_edit_) {
                main_window->draw_rules_scroll_indicator(dc);
            }
            ReleaseDC(control, dc);
        }
        return result;
    }
    if (message == WM_PRINTCLIENT && combo_box && main_window != nullptr) {
        main_window->draw_combo_box(control, reinterpret_cast<HDC>(wparam));
        return 0;
    }
    if (message == WM_PRINTCLIENT) {
        const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
        if (main_window != nullptr) {
            main_window->draw_input_frame(control, reinterpret_cast<HDC>(wparam));
            if (control == main_window->rules_edit_) {
                main_window->draw_rules_scroll_indicator(
                    reinterpret_cast<HDC>(wparam));
            }
        }
        return result;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
        InvalidateRect(control, nullptr, FALSE);
        return result;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, input_subclass_proc, kInputSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK WindowsMainWindow::combo_list_subclass_proc(
    HWND control,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR reference_data) {
    auto* main_window = reinterpret_cast<WindowsMainWindow*>(reference_data);
    if (message == WM_ERASEBKGND && main_window != nullptr) {
        RECT rectangle{};
        GetClientRect(control, &rectangle);
        FillRect(
            reinterpret_cast<HDC>(wparam),
            &rectangle,
            main_window->theme_.surface_brush());
        return 1;
    }
    if (message == WM_PAINT) {
        const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
        if (main_window != nullptr) {
            const HDC dc = GetDC(control);
            if (dc != nullptr) {
                RECT rectangle{};
                GetClientRect(control, &rectangle);
                main_window->theme_.stroke_rounded_rectangle(
                    dc,
                    rectangle,
                    main_window->theme_.metrics().radius,
                    main_window->theme_.palette().border,
                    main_window->theme_.metrics().border);
                ReleaseDC(control, dc);
            }
        }
        return result;
    }
    if (message == WM_SHOWWINDOW || message == WM_SIZE) {
        const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
        if (main_window != nullptr) {
            main_window->update_combo_list_region(control);
        }
        return result;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(
            control, combo_list_subclass_proc, kComboListSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK WindowsMainWindow::settings_scrollbar_subclass_proc(
    HWND control,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR reference_data) {
    auto* main_window = reinterpret_cast<WindowsMainWindow*>(reference_data);
    if (message == WM_MOUSEWHEEL && main_window != nullptr) {
        (void)SendMessageW(main_window->window_, message, wparam, lparam);
        return 0;
    }
    if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONUP
            || message == WM_MOUSEMOVE || message == WM_CAPTURECHANGED)
        && main_window != nullptr) {
        main_window->handle_settings_scrollbar_pointer(
            control, message, wparam, lparam);
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(
            control,
            settings_scrollbar_subclass_proc,
            kSettingsScrollbarSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK WindowsMainWindow::settings_viewport_subclass_proc(
    HWND control,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR reference_data) {
    auto* main_window = reinterpret_cast<WindowsMainWindow*>(reference_data);
    if (main_window != nullptr
        && (message == WM_COMMAND || message == WM_DRAWITEM
            || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT
            || message == WM_CTLCOLORLISTBOX)) {
        return SendMessageW(main_window->window_, message, wparam, lparam);
    }
    if (message == WM_MOUSEWHEEL && main_window != nullptr) {
        return SendMessageW(main_window->window_, message, wparam, lparam);
    }
    if (message == WM_ERASEBKGND && main_window != nullptr) {
        RECT rectangle{};
        GetClientRect(control, &rectangle);
        FillRect(
            reinterpret_cast<HDC>(wparam),
            &rectangle,
            main_window->theme_.canvas_brush());
        return 1;
    }
    if (message == WM_PAINT && main_window != nullptr) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(control, &paint);
        FillRect(dc, &paint.rcPaint, main_window->theme_.canvas_brush());
        EndPaint(control, &paint);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(
            control,
            settings_viewport_subclass_proc,
            kSettingsViewportSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK WindowsMainWindow::canvas_static_subclass_proc(
    HWND control,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR reference_data) {
    auto* main_window = reinterpret_cast<WindowsMainWindow*>(reference_data);
    if (message == WM_ERASEBKGND && main_window != nullptr) {
        RECT rectangle{};
        GetClientRect(control, &rectangle);
        FillRect(
            reinterpret_cast<HDC>(wparam),
            &rectangle,
            main_window->theme_.canvas_brush());
        return 1;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(
            control, canvas_static_subclass_proc, kCanvasStaticSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK WindowsMainWindow::window_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    auto* main_window = reinterpret_cast<WindowsMainWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        main_window = static_cast<WindowsMainWindow*>(create->lpCreateParams);
        main_window->window_ = window;
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(main_window));
    }
    if (main_window != nullptr) {
        return main_window->handle_message(message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT WindowsMainWindow::handle_message(
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        std::string error;
        if (!create_controls(error)) {
            return -1;
        }
        update_view_visibility();
        layout_controls();
        notify_lifecycle("created");
        return 0;
    }
    case WM_SIZE:
        layout_controls();
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(
            window_, nullptr,
            suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        theme_.refresh(window_, dpi_);
        create_fonts();
        apply_fonts();
        apply_theme();
        (void)SendMessageW(profile_list_, LB_SETITEMHEIGHT, 0, scale(54));
        layout_controls();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = scale(kMinimumWidth);
        info->ptMinTrackSize.y = scale(kMinimumHeight);
        return 0;
    }
    case WM_COMMAND:
        handle_command(LOWORD(wparam), HIWORD(wparam));
        return 0;
    case WM_DRAWITEM:
        if (draw_item(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) {
            return TRUE;
        }
        break;
    case WM_VSCROLL:
        if (view_ == View::Settings) {
            handle_settings_scroll(LOWORD(wparam), HIWORD(wparam));
            return 0;
        }
        break;
    case WM_TIMER:
        if (wparam == kSettingsScrollTimerId) {
            advance_settings_scroll_animation();
            return 0;
        }
        if (wparam == kRulesScrollIndicatorTimerId) {
            hide_rules_scroll_indicator();
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (view_ == View::Settings) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            handle_settings_scroll(delta > 0 ? SB_LINEUP : SB_LINEDOWN);
            return 0;
        }
        break;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        theme_.refresh(window_, dpi_);
        apply_theme();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint_state{};
        const HDC dc = BeginPaint(window_, &paint_state);
        RECT client{};
        GetClientRect(window_, &client);
        const HDC buffer_dc = CreateCompatibleDC(dc);
        const HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        const auto old_bitmap = SelectObject(buffer_dc, bitmap);
        paint(buffer_dc);
        BitBlt(dc, 0, 0, client.right, client.bottom, buffer_dc, 0, 0, SRCCOPY);
        SelectObject(buffer_dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer_dc);
        EndPaint(window_, &paint_state);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        const HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, theme_.palette().canvas);
        const HWND control = reinterpret_cast<HWND>(lparam);
        COLORREF color = theme_.palette().text;
        if (control == listener_text_ || control == settings_subtitle_
            || control == profile_status_ || control == rules_status_) {
            color = theme_.palette().text_muted;
        }
        if (control == command_status_ && local_status_is_error_) {
            color = theme_.palette().danger;
        }
        SetTextColor(dc, color);
        return reinterpret_cast<LRESULT>(theme_.canvas_brush());
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, theme_.palette().text);
        SetBkColor(dc, theme_.palette().surface);
        return reinterpret_cast<LRESULT>(theme_.surface_brush());
    }
    case WM_CLOSE:
        request_window_close();
        return 0;
    case WM_NCDESTROY: {
        const HWND destroyed_window = window_;
        destroy_fonts();
        window_ = nullptr;
        reset_handles();
        profile_local_dirty_ = false;
        settings_local_dirty_ = false;
        rules_local_dirty_ = false;
        updating_ = false;
        notify_lifecycle("destroyed");
        return DefWindowProcW(destroyed_window, message, wparam, lparam);
    }
    default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

} // namespace ccs

#endif
