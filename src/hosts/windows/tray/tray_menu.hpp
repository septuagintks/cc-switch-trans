#pragma once

#ifdef _WIN32

#include "app/application_status.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace ccs {

inline constexpr UINT kTrayMenuStatus = 1000;
inline constexpr UINT kTrayMenuStart = 1001;
inline constexpr UINT kTrayMenuStop = 1002;
inline constexpr UINT kTrayMenuReload = 1003;
inline constexpr UINT kTrayMenuOpenConfig = 1004;
inline constexpr UINT kTrayMenuOpenLogs = 1005;
inline constexpr UINT kTrayMenuStartup = 1006;
inline constexpr UINT kTrayMenuExit = 1007;
inline constexpr UINT kTrayMenuOpenMain = 1008;
inline constexpr UINT kTrayMenuLightweight = 1009;

struct TrayMenuState {
    ApplicationStatus application;
    bool service_command_pending = false;
    bool view_available = false;
    bool view_command_pending = false;
    bool lightweight_mode = true;
    bool startup_known = false;
    bool startup_enabled = false;
};

bool show_tray_menu(
    HWND owner,
    const TrayMenuState& state,
    UINT& selected,
    std::string& error);

} // namespace ccs

#endif
