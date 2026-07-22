#include "hosts/windows/tray/tray_icon.hpp"

#ifdef _WIN32

#include <strsafe.h>

namespace ccs {

TrayIcon::~TrayIcon() {
    remove();
}

bool TrayIcon::add(
    HWND owner,
    HICON icon,
    UINT callback_message,
    UINT identifier,
    bool enabled,
    std::string& error) {
    error.clear();
    enabled_ = enabled;
    if (!enabled_) return true;
    if (owner == nullptr || icon == nullptr || callback_message == 0) {
        error = "tray icon configuration is incomplete";
        return false;
    }
    notification_ = {};
    notification_.cbSize = sizeof(notification_);
    notification_.hWnd = owner;
    notification_.uID = identifier;
    notification_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    notification_.uCallbackMessage = callback_message;
    notification_.hIcon = icon;
    (void)StringCchCopyW(
        notification_.szTip, ARRAYSIZE(notification_.szTip), L"ccs-trans");
    for (int attempt = 0; attempt < 20 && !added_; ++attempt) {
        added_ = Shell_NotifyIconW(NIM_ADD, &notification_) != FALSE;
        if (!added_) Sleep(100);
    }
    if (!added_) {
        error = "failed to add the ccs-trans notification area icon after 2 seconds";
        return false;
    }
    notification_.uVersion = NOTIFYICON_VERSION_4;
    (void)Shell_NotifyIconW(NIM_SETVERSION, &notification_);
    auto tooltip = notification_;
    tooltip.uFlags = NIF_TIP | NIF_SHOWTIP;
    (void)Shell_NotifyIconW(NIM_MODIFY, &tooltip);
    return true;
}

void TrayIcon::remove() noexcept {
    if (added_) {
        (void)Shell_NotifyIconW(NIM_DELETE, &notification_);
        added_ = false;
    }
}

void TrayIcon::taskbar_recreated() noexcept {
    added_ = false;
}

void TrayIcon::show_notification(
    const std::wstring& title,
    const std::wstring& message,
    DWORD flags) const noexcept {
    if (!added_) return;
    auto notification = notification_;
    notification.uFlags = NIF_INFO;
    notification.dwInfoFlags = flags;
    (void)StringCchCopyW(
        notification.szInfoTitle,
        ARRAYSIZE(notification.szInfoTitle),
        title.c_str());
    (void)StringCchCopyW(
        notification.szInfo,
        ARRAYSIZE(notification.szInfo),
        message.c_str());
    (void)Shell_NotifyIconW(NIM_MODIFY, &notification);
}

bool TrayIcon::added() const noexcept { return added_; }
bool TrayIcon::enabled() const noexcept { return enabled_; }

} // namespace ccs

#endif
