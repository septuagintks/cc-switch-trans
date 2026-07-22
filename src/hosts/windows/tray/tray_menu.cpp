#include "hosts/windows/tray/tray_menu.hpp"

#ifdef _WIN32

#include "hosts/windows/windows_error.hpp"

#include <string_view>

namespace ccs {

namespace {

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) return L"unknown";
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

std::wstring status_text(const ApplicationStatus& status) {
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

} // namespace

bool show_tray_menu(
    HWND owner,
    const TrayMenuState& state,
    UINT& selected,
    std::string& error) {
    selected = 0;
    error.clear();
    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        error = windows_error_message(
            "failed to create the tray menu", GetLastError());
        return false;
    }

    const auto application_state = state.application.state;
    const bool transition = application_state == ApplicationState::Starting
        || application_state == ApplicationState::Reloading
        || application_state == ApplicationState::Stopping;
    const bool blocked = state.service_command_pending
        || state.view_command_pending || transition;
    const bool can_start = !blocked
        && (application_state == ApplicationState::Stopped
            || application_state == ApplicationState::Faulted);
    const bool can_stop = !blocked
        && application_state == ApplicationState::Running;

    (void)AppendMenuW(menu, MF_STRING, kTrayMenuOpenMain, L"Open ccs-trans");
    (void)SetMenuDefaultItem(menu, kTrayMenuOpenMain, FALSE);
    (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const auto status = status_text(state.application);
    (void)AppendMenuW(
        menu,
        MF_STRING | MF_DISABLED | MF_GRAYED,
        kTrayMenuStatus,
        status.c_str());
    (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    (void)AppendMenuW(
        menu,
        MF_STRING | (can_start ? MF_ENABLED : MF_GRAYED),
        kTrayMenuStart,
        L"Start");
    (void)AppendMenuW(
        menu,
        MF_STRING | (can_stop ? MF_ENABLED : MF_GRAYED),
        kTrayMenuStop,
        L"Stop");
    (void)AppendMenuW(
        menu,
        MF_STRING | (can_stop ? MF_ENABLED : MF_GRAYED),
        kTrayMenuReload,
        L"Reload configuration");
    (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    (void)AppendMenuW(
        menu, MF_STRING, kTrayMenuOpenConfig, L"Open configuration");
    (void)AppendMenuW(menu, MF_STRING, kTrayMenuOpenLogs, L"Open logs");
    UINT lightweight_flags = MF_STRING;
    if (!state.view_available || state.view_command_pending) {
        lightweight_flags |= MF_GRAYED;
    } else if (state.lightweight_mode) {
        lightweight_flags |= MF_CHECKED;
    }
    (void)AppendMenuW(
        menu,
        lightweight_flags,
        kTrayMenuLightweight,
        L"Lightweight mode");
    UINT startup_flags = MF_STRING;
    if (!state.startup_known) {
        startup_flags |= MF_GRAYED;
    } else if (state.startup_enabled) {
        startup_flags |= MF_CHECKED;
    }
    (void)AppendMenuW(
        menu, startup_flags, kTrayMenuStartup, L"Start at sign-in");
    (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    (void)AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"Exit");

    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        error = windows_error_message(
            "failed to locate the tray menu", GetLastError());
        DestroyMenu(menu);
        return false;
    }
    (void)SetForegroundWindow(owner);
    selected = TrackPopupMenuEx(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        owner,
        nullptr);
    DestroyMenu(menu);
    return true;
}

} // namespace ccs

#endif
