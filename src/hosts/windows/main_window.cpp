#include "hosts/windows/main_window.hpp"

#ifdef _WIN32

#include "hosts/windows/resource_ids.hpp"
#include "hosts/windows/windows_error.hpp"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <utility>

namespace ccs {

namespace {

constexpr int kDefaultWidth = 900;
constexpr int kDefaultHeight = 590;
constexpr int kMinimumWidth = 760;
constexpr int kMinimumHeight = 500;

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

std::wstring application_status_text(const ApplicationStatus& status) {
    std::wstring text = L"Service: ";
    text += utf8_to_wide(application_state_name(status.state));
    return text;
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
    if (error_brush_ != nullptr) {
        DeleteObject(error_brush_);
    }
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
        error = "failed to initialize Windows list-view and tooltip controls";
        return false;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &WindowsMainWindow::window_proc;
    window_class.hInstance = instance_;
    window_class.hIcon = icon_;
    window_class.hIconSm = icon_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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
    status_text_ = create_control(0, WC_STATICW, L"Service: stopped", SS_LEFT, 0);
    listener_text_ = create_control(0, WC_STATICW, L"Listener inactive", SS_LEFT, 0);
    start_button_ = create_control(
        0, WC_BUTTONW, L"\u25B6 Start", BS_PUSHBUTTON, kMainServiceStartId);
    stop_button_ = create_control(
        0, WC_BUTTONW, L"\u25A0 Stop", BS_PUSHBUTTON, kMainServiceStopId);
    reload_button_ = create_control(
        0, WC_BUTTONW, L"\u21BB Reload", BS_PUSHBUTTON, kMainServiceReloadId);
    lightweight_checkbox_ = create_control(
        0,
        WC_BUTTONW,
        L"Lightweight mode",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        kMainLightweightId);
    header_separator_ = create_control(0, WC_STATICW, L"", SS_ETCHEDHORZ, 0);
    profile_list_ = create_control(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"Profiles",
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
        kMainProfileListId);
    new_profile_edit_ = create_control(
        WS_EX_CLIENTEDGE,
        WC_EDITW,
        L"",
        ES_AUTOHSCROLL | WS_TABSTOP,
        kMainNewProfileEditId);
    add_profile_button_ = create_control(
        0, WC_BUTTONW, L"+ Add", BS_PUSHBUTTON, kMainAddProfileId);
    remove_profile_button_ = create_control(
        0, WC_BUTTONW, L"\u2212 Remove", BS_PUSHBUTTON, kMainRemoveProfileId);
    details_title_ = create_control(0, WC_STATICW, L"Profile details", SS_LEFT, 0);
    profile_id_label_ = create_control(0, WC_STATICW, L"Profile ID", SS_LEFT, 0);
    rename_profile_edit_ = create_control(
        WS_EX_CLIENTEDGE,
        WC_EDITW,
        L"",
        ES_AUTOHSCROLL | WS_TABSTOP,
        kMainRenameProfileEditId);
    rename_profile_button_ = create_control(
        0, WC_BUTTONW, L"\u270E Rename", BS_PUSHBUTTON, kMainRenameProfileId);
    enabled_checkbox_ = create_control(
        0,
        WC_BUTTONW,
        L"Enabled",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        kMainProfileEnabledId);
    protocol_label_ = create_control(0, WC_STATICW, L"Protocol", SS_LEFT, 0);
    protocol_value_ = create_control(0, WC_STATICW, L"Not configured", SS_LEFT, 0);
    readiness_label_ = create_control(0, WC_STATICW, L"Configuration", SS_LEFT, 0);
    readiness_value_ = create_control(0, WC_STATICW, L"Incomplete", SS_LEFT, 0);
    profile_status_ = create_control(
        0,
        WC_STATICW,
        L"",
        SS_LEFT | SS_EDITCONTROL,
        kMainProfileStatusId);
    footer_separator_ = create_control(0, WC_STATICW, L"", SS_ETCHEDHORZ, 0);
    command_status_ = create_control(
        0,
        WC_STATICW,
        L"",
        SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
        0);
    reload_draft_button_ = create_control(
        0, WC_BUTTONW, L"Reload Draft", BS_PUSHBUTTON, kMainReloadDraftId);
    apply_button_ = create_control(
        0, WC_BUTTONW, L"Apply", BS_DEFPUSHBUTTON, kMainApplyId);
    discard_button_ = create_control(
        0, WC_BUTTONW, L"Discard", BS_PUSHBUTTON, kMainDiscardId);
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

    const std::array<HWND, 25> required = {
        status_text_,
        listener_text_,
        start_button_,
        stop_button_,
        reload_button_,
        lightweight_checkbox_,
        header_separator_,
        profile_list_,
        new_profile_edit_,
        add_profile_button_,
        remove_profile_button_,
        details_title_,
        profile_id_label_,
        rename_profile_edit_,
        rename_profile_button_,
        enabled_checkbox_,
        protocol_label_,
        protocol_value_,
        readiness_label_,
        readiness_value_,
        profile_status_,
        footer_separator_,
        command_status_,
        reload_draft_button_,
        apply_button_,
    };
    if (std::any_of(required.begin(), required.end(), [](HWND control) {
            return control == nullptr;
        })
        || discard_button_ == nullptr
        || tooltip_ == nullptr) {
        error = windows_error_message(
            "failed to create main window controls", GetLastError());
        return false;
    }

    (void)SendMessageW(new_profile_edit_, EM_SETLIMITTEXT, 64, 0);
    (void)SendMessageW(rename_profile_edit_, EM_SETLIMITTEXT, 64, 0);
    ListView_SetExtendedListViewStyle(
        profile_list_,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(L"Profile");
    column.cx = scale(145);
    (void)ListView_InsertColumn(profile_list_, 0, &column);
    column.iSubItem = 1;
    column.pszText = const_cast<wchar_t*>(L"Enabled");
    column.cx = scale(64);
    (void)ListView_InsertColumn(profile_list_, 1, &column);
    column.iSubItem = 2;
    column.pszText = const_cast<wchar_t*>(L"Status");
    column.cx = scale(90);
    (void)ListView_InsertColumn(profile_list_, 2, &column);

    add_tooltip(start_button_, L"Start the local service");
    add_tooltip(stop_button_, L"Stop the local service");
    add_tooltip(reload_button_, L"Reload saved configuration");
    add_tooltip(add_profile_button_, L"Create a disabled Profile draft");
    add_tooltip(remove_profile_button_, L"Remove the selected Profile from the draft");
    add_tooltip(rename_profile_button_, L"Rename the selected Profile in the draft");
    add_tooltip(
        reload_draft_button_,
        L"Reload Profile configuration from disk; unsaved changes require confirmation");
    create_fonts();
    apply_fonts();
    if (error_brush_ == nullptr) {
        error_brush_ = CreateSolidBrush(RGB(255, 235, 235));
    }
    return true;
}

void WindowsMainWindow::create_fonts() {
    destroy_fonts();
    normal_font_ = CreateFontW(
        -MulDiv(9, static_cast<int>(dpi_), 72),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    title_font_ = CreateFontW(
        -MulDiv(15, static_cast<int>(dpi_), 72),
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

void WindowsMainWindow::destroy_fonts() noexcept {
    if (normal_font_ != nullptr) {
        DeleteObject(normal_font_);
        normal_font_ = nullptr;
    }
    if (title_font_ != nullptr) {
        DeleteObject(title_font_);
        title_font_ = nullptr;
    }
}

void WindowsMainWindow::apply_fonts() {
    const std::array<HWND, 22> normal_controls = {
        status_text_,
        listener_text_,
        start_button_,
        stop_button_,
        reload_button_,
        lightweight_checkbox_,
        profile_list_,
        new_profile_edit_,
        add_profile_button_,
        remove_profile_button_,
        profile_id_label_,
        rename_profile_edit_,
        rename_profile_button_,
        enabled_checkbox_,
        protocol_label_,
        protocol_value_,
        readiness_label_,
        readiness_value_,
        profile_status_,
        command_status_,
        reload_draft_button_,
        apply_button_,
    };
    for (const auto control : normal_controls) {
        if (control != nullptr) {
            (void)SendMessageW(
                control, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
        }
    }
    (void)SendMessageW(
        discard_button_, WM_SETFONT, reinterpret_cast<WPARAM>(normal_font_), TRUE);
    (void)SendMessageW(
        details_title_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
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

void WindowsMainWindow::layout_controls() {
    if (window_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = scale(16);
    const int gap = scale(10);
    const int button_height = scale(30);
    const int header_height = scale(76);
    const int footer_height = scale(62);
    const int service_button_width = scale(82);

    int service_x = width - margin - service_button_width;
    MoveWindow(reload_button_, service_x, margin, service_button_width, button_height, TRUE);
    service_x -= service_button_width + scale(6);
    MoveWindow(stop_button_, service_x, margin, service_button_width, button_height, TRUE);
    service_x -= service_button_width + scale(6);
    MoveWindow(start_button_, service_x, margin, service_button_width, button_height, TRUE);
    MoveWindow(status_text_, margin, margin, std::max(0, service_x - margin - gap), scale(24), TRUE);
    MoveWindow(listener_text_, margin, margin + scale(27), scale(250), scale(22), TRUE);
    MoveWindow(
        lightweight_checkbox_,
        service_x,
        margin + scale(38),
        width - margin - service_x,
        scale(24),
        TRUE);
    MoveWindow(header_separator_, margin, header_height, width - margin * 2, scale(2), TRUE);

    const int body_top = header_height + scale(14);
    const int body_bottom = height - footer_height;
    const int body_height = std::max(0, body_bottom - body_top);
    const int left_width = std::clamp(width / 3, scale(258), scale(330));
    const int right_x = margin + left_width + scale(24);
    const int right_width = std::max(0, width - right_x - margin);
    const int add_width = scale(72);
    const int remove_width = scale(96);
    const int list_bottom_controls = scale(34);

    MoveWindow(
        profile_list_,
        margin,
        body_top,
        left_width,
        std::max(0, body_height - list_bottom_controls - gap),
        TRUE);
    RECT profile_client{};
    GetClientRect(profile_list_, &profile_client);
    const int enabled_column_width = scale(64);
    const int status_column_width = scale(90);
    const int profile_column_width = std::max(
        scale(90),
        static_cast<int>(profile_client.right)
            - enabled_column_width - status_column_width);
    ListView_SetColumnWidth(profile_list_, 0, profile_column_width);
    ListView_SetColumnWidth(profile_list_, 1, enabled_column_width);
    ListView_SetColumnWidth(profile_list_, 2, status_column_width);
    const int bottom_row_y = body_bottom - list_bottom_controls;
    MoveWindow(
        new_profile_edit_,
        margin,
        bottom_row_y,
        std::max(scale(70), left_width - add_width - remove_width - gap * 2),
        list_bottom_controls,
        TRUE);
    int left_button_x = margin + left_width - add_width - remove_width - gap;
    MoveWindow(add_profile_button_, left_button_x, bottom_row_y, add_width, list_bottom_controls, TRUE);
    left_button_x += add_width + gap;
    MoveWindow(
        remove_profile_button_,
        left_button_x,
        bottom_row_y,
        remove_width,
        list_bottom_controls,
        TRUE);

    MoveWindow(details_title_, right_x, body_top, right_width, scale(32), TRUE);
    int y = body_top + scale(46);
    MoveWindow(profile_id_label_, right_x, y, right_width, scale(20), TRUE);
    y += scale(24);
    const int rename_width = scale(104);
    MoveWindow(
        rename_profile_edit_,
        right_x,
        y,
        std::max(0, right_width - rename_width - gap),
        button_height,
        TRUE);
    MoveWindow(
        rename_profile_button_,
        right_x + right_width - rename_width,
        y,
        rename_width,
        button_height,
        TRUE);
    y += button_height + scale(14);
    MoveWindow(enabled_checkbox_, right_x, y, scale(120), scale(24), TRUE);
    y += scale(42);
    const int label_width = scale(105);
    MoveWindow(protocol_label_, right_x, y, label_width, scale(22), TRUE);
    MoveWindow(protocol_value_, right_x + label_width, y, right_width - label_width, scale(22), TRUE);
    y += scale(34);
    MoveWindow(readiness_label_, right_x, y, label_width, scale(22), TRUE);
    MoveWindow(readiness_value_, right_x + label_width, y, right_width - label_width, scale(22), TRUE);
    y += scale(38);
    MoveWindow(
        profile_status_,
        right_x,
        y,
        right_width,
        std::max(scale(48), body_bottom - y),
        TRUE);

    MoveWindow(footer_separator_, margin, height - footer_height + scale(8), width - margin * 2, scale(2), TRUE);
    const int footer_y = height - scale(43);
    const int action_width = scale(88);
    const int reload_draft_width = scale(112);
    MoveWindow(discard_button_, width - margin - action_width, footer_y, action_width, button_height, TRUE);
    MoveWindow(
        apply_button_,
        width - margin - action_width * 2 - gap,
        footer_y,
        action_width,
        button_height,
        TRUE);
    MoveWindow(
        reload_draft_button_,
        width - margin - action_width * 2 - reload_draft_width - gap * 2,
        footer_y,
        reload_draft_width,
        button_height,
        TRUE);
    MoveWindow(
        command_status_,
        margin,
        footer_y,
        std::max(0,
            width - margin * 2 - action_width * 2 - reload_draft_width - gap * 3),
        button_height,
        TRUE);
}

void WindowsMainWindow::render() {
    if (window_ == nullptr || !state_) {
        return;
    }
    updating_ = true;
    SetWindowTextW(status_text_, application_status_text(state_->application).c_str());
    SetWindowTextW(listener_text_, listener_status_text(state_->application).c_str());
    Button_SetCheck(
        lightweight_checkbox_, state_->lightweight_mode ? BST_CHECKED : BST_UNCHECKED);
    render_profile_list();
    render_profile_details();
    render_command_status();
    update_enabled_states();
    updating_ = false;
}

void WindowsMainWindow::render_profile_list() {
    const auto selected_id = state_->selected_profile_id;
    ListView_DeleteAllItems(profile_list_);
    for (std::size_t index = 0; index < state_->profiles.size(); ++index) {
        const auto& profile = state_->profiles[index];
        auto id = utf8_to_wide(profile.id);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = id.data();
        const int inserted = ListView_InsertItem(profile_list_, &item);
        if (inserted < 0) {
            continue;
        }
        auto enabled = profile.enabled ? std::wstring{L"Yes"} : std::wstring{L"No"};
        ListView_SetItemText(profile_list_, inserted, 1, enabled.data());
        auto readiness = std::wstring{readiness_text(profile.readiness)};
        ListView_SetItemText(profile_list_, inserted, 2, readiness.data());
        if (selected_id && profile.id == *selected_id) {
            ListView_SetItemState(
                profile_list_,
                inserted,
                LVIS_SELECTED | LVIS_FOCUSED,
                LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(profile_list_, inserted, FALSE);
        }
    }
}

void WindowsMainWindow::render_profile_details() {
    const auto* profile = selected_profile();
    if (profile == nullptr) {
        rendered_profile_id_.clear();
        SetWindowTextW(rename_profile_edit_, L"");
        Button_SetCheck(enabled_checkbox_, BST_UNCHECKED);
        SetWindowTextW(protocol_value_, L"No Profile selected");
        SetWindowTextW(readiness_value_, L"");
        SetWindowTextW(profile_status_, L"");
        return;
    }
    const auto profile_id = utf8_to_wide(profile->id);
    if (rendered_profile_id_ != profile_id) {
        rendered_profile_id_ = profile_id;
        SetWindowTextW(rename_profile_edit_, profile_id.c_str());
    }
    Button_SetCheck(enabled_checkbox_, profile->enabled ? BST_CHECKED : BST_UNCHECKED);
    const auto protocol = profile->protocol
        ? utf8_to_wide(*profile->protocol)
        : std::wstring{L"Not configured"};
    SetWindowTextW(protocol_value_, protocol.c_str());
    SetWindowTextW(readiness_value_, readiness_text(profile->readiness));
    std::wstring detail = L"Rules: "
        + std::to_wstring(profile->enabled_rule_count) + L" enabled / "
        + std::to_wstring(profile->rule_count) + L" total";
    if (!profile->status_detail.empty()) {
        detail += L"\r\n\r\n" + utf8_to_wide(profile->status_detail);
    }
    SetWindowTextW(profile_status_, detail.c_str());
}

void WindowsMainWindow::render_command_status() {
    if (!local_status_.empty()) {
        SetWindowTextW(command_status_, local_status_.c_str());
        InvalidateRect(command_status_, nullptr, TRUE);
        return;
    }
    if (state_->command_pending) {
        SetWindowTextW(command_status_, L"Working...");
        return;
    }
    if (!state_->last_command) {
        SetWindowTextW(command_status_, L"");
        return;
    }
    const auto& result = *state_->last_command;
    if (!result.detail.empty()) {
        SetWindowTextW(command_status_, utf8_to_wide(result.detail).c_str());
    } else if (result.succeeded()) {
        SetWindowTextW(command_status_, L"Completed");
    } else if (result.outcome == CommandOutcome::SavedPendingRuntimeApply) {
        SetWindowTextW(command_status_, L"Saved; runtime update is still pending");
    } else {
        SetWindowTextW(command_status_, L"Command failed");
    }
}

void WindowsMainWindow::update_enabled_states() {
    const bool pending = state_->command_pending;
    const auto actions = service_actions_for(state_->application.state);
    EnableWindow(start_button_, !pending && actions.can_start);
    EnableWindow(stop_button_, !pending && actions.can_stop);
    EnableWindow(reload_button_, !pending && actions.can_reload);
    EnableWindow(lightweight_checkbox_, !pending);
    EnableWindow(profile_list_, !pending);
    EnableWindow(new_profile_edit_, !pending);
    EnableWindow(add_profile_button_, !pending);
    const bool has_profile = selected_profile() != nullptr;
    EnableWindow(remove_profile_button_, !pending && has_profile);
    EnableWindow(rename_profile_edit_, !pending && has_profile);
    EnableWindow(rename_profile_button_, !pending && has_profile);
    EnableWindow(enabled_checkbox_, !pending && has_profile);
    EnableWindow(
        reload_draft_button_,
        !pending && state_->draft.loaded() && !state_->draft.busy());
    EnableWindow(apply_button_, !pending && state_->draft.dirty());
    EnableWindow(discard_button_, !pending && state_->draft.dirty());
}

void WindowsMainWindow::handle_command(int id, int notification_code) {
    switch (id) {
    case kMainServiceStartId:
        if (notification_code == BN_CLICKED) {
            submit({MainWindowCommand::StartService});
        }
        break;
    case kMainServiceStopId:
        if (notification_code == BN_CLICKED) {
            submit({MainWindowCommand::StopService});
        }
        break;
    case kMainServiceReloadId:
        if (notification_code == BN_CLICKED) {
            submit({MainWindowCommand::ReloadService});
        }
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
                submit({MainWindowCommand::RemoveProfile, *state_->selected_profile_id});
            }
        }
        break;
    case kMainRenameProfileId:
        if (notification_code == BN_CLICKED && state_->selected_profile_id) {
            const auto replacement = control_text(rename_profile_edit_);
            if (replacement.empty()) {
                set_local_status(L"Profile ID cannot be empty.", true);
            } else {
                submit({
                    MainWindowCommand::RenameProfile,
                    *state_->selected_profile_id,
                    wide_to_utf8(replacement),
                });
            }
        }
        break;
    case kMainProfileEnabledId:
        if (notification_code == BN_CLICKED && state_->selected_profile_id) {
            submit({
                MainWindowCommand::SetProfileEnabled,
                *state_->selected_profile_id,
                {},
                Button_GetCheck(enabled_checkbox_) == BST_CHECKED,
            });
        }
        break;
    case kMainApplyId:
        if (notification_code == BN_CLICKED) {
            submit({MainWindowCommand::ApplyDraft});
        }
        break;
    case kMainReloadDraftId:
        if (notification_code == BN_CLICKED) {
            if (!state_->draft.dirty()) {
                submit({MainWindowCommand::ReloadDraft});
                break;
            }
            if (MessageBoxW(
                    window_,
                    L"Discard unsaved Profile changes and reload the configuration from disk?",
                    L"Reload Profile Configuration",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)
                == IDYES) {
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
            submit({MainWindowCommand::DiscardDraft});
        }
        break;
    default:
        break;
    }
}

void WindowsMainWindow::handle_list_notification(const NMLISTVIEW& notification) {
    if (updating_
        || (notification.uChanged & LVIF_STATE) == 0
        || (notification.uNewState & LVIS_SELECTED) == 0
        || notification.iItem < 0) {
        return;
    }
    wchar_t buffer[128]{};
    ListView_GetItemText(
        profile_list_, notification.iItem, 0, buffer, static_cast<int>(std::size(buffer)));
    const auto profile_id = wide_to_utf8(buffer);
    if (!profile_id.empty()
        && (!state_->selected_profile_id || *state_->selected_profile_id != profile_id)) {
        submit({MainWindowCommand::SelectProfile, profile_id});
    }
}

void WindowsMainWindow::submit(MainWindowCommandRequest request) {
    local_status_.clear();
    local_status_is_error_ = false;
    if (!view_model_.submit(std::move(request))) {
        set_local_status(L"Another command is already running.", true);
    }
}

void WindowsMainWindow::request_window_close() {
    if (!state_) {
        perform_close(CloseTarget::Destroy);
        return;
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
    if (choice == IDYES) {
        return UnsavedChangesDecision::Apply;
    }
    if (choice == IDNO) {
        return UnsavedChangesDecision::Discard;
    }
    return UnsavedChangesDecision::Cancel;
}

void WindowsMainWindow::set_local_status(
    const std::wstring& message,
    bool error) {
    local_status_ = message;
    local_status_is_error_ = error;
    if (command_status_ != nullptr) {
        SetWindowTextW(command_status_, local_status_.c_str());
        InvalidateRect(command_status_, nullptr, TRUE);
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

const ProfileListItem* WindowsMainWindow::selected_profile() const noexcept {
    if (!state_ || !state_->selected_profile_id) {
        return nullptr;
    }
    return find_profile_list_item(*state_, *state_->selected_profile_id);
}

int WindowsMainWindow::scale(int logical_pixels) const noexcept {
    return MulDiv(logical_pixels, static_cast<int>(dpi_), 96);
}

void WindowsMainWindow::notify_lifecycle(std::string_view event) const {
    if (lifecycle_handler_) {
        lifecycle_handler_(event);
    }
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
        dpi_ = GetDpiForWindow(window_);
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
            window_,
            nullptr,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        create_fonts();
        apply_fonts();
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
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->hwndFrom == profile_list_ && header->code == LVN_ITEMCHANGED) {
            handle_list_notification(*reinterpret_cast<const NMLISTVIEW*>(lparam));
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lparam) == command_status_ && local_status_is_error_) {
            SetTextColor(reinterpret_cast<HDC>(wparam), RGB(160, 20, 20));
            SetBkColor(reinterpret_cast<HDC>(wparam), RGB(255, 235, 235));
            return reinterpret_cast<LRESULT>(error_brush_);
        }
        break;
    case WM_CLOSE:
        request_window_close();
        return 0;
    case WM_NCDESTROY: {
        const HWND destroyed_window = window_;
        destroy_fonts();
        tooltip_ = nullptr;
        window_ = nullptr;
        status_text_ = nullptr;
        listener_text_ = nullptr;
        start_button_ = nullptr;
        stop_button_ = nullptr;
        reload_button_ = nullptr;
        lightweight_checkbox_ = nullptr;
        header_separator_ = nullptr;
        profile_list_ = nullptr;
        new_profile_edit_ = nullptr;
        add_profile_button_ = nullptr;
        remove_profile_button_ = nullptr;
        details_title_ = nullptr;
        profile_id_label_ = nullptr;
        rename_profile_edit_ = nullptr;
        rename_profile_button_ = nullptr;
        enabled_checkbox_ = nullptr;
        protocol_label_ = nullptr;
        protocol_value_ = nullptr;
        readiness_label_ = nullptr;
        readiness_value_ = nullptr;
        profile_status_ = nullptr;
        footer_separator_ = nullptr;
        command_status_ = nullptr;
        reload_draft_button_ = nullptr;
        apply_button_ = nullptr;
        discard_button_ = nullptr;
        rendered_profile_id_.clear();
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
