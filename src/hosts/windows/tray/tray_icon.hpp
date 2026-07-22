#pragma once

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>

namespace ccs {

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool add(
        HWND owner,
        HICON icon,
        UINT callback_message,
        UINT identifier,
        bool enabled,
        std::string& error);
    void remove() noexcept;
    void taskbar_recreated() noexcept;
    void show_notification(
        const std::wstring& title,
        const std::wstring& message,
        DWORD flags) const noexcept;

    [[nodiscard]] bool added() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;

private:
    NOTIFYICONDATAW notification_{};
    bool added_ = false;
    bool enabled_ = true;
};

} // namespace ccs

#endif
