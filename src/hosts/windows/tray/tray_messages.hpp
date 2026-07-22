#pragma once

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ccs::tray_messages {

inline constexpr UINT_PTR status_timer = 1;
inline constexpr UINT command_result = WM_APP + 2;
inline constexpr UINT shutdown_complete = WM_APP + 3;
inline constexpr UINT maintenance_shutdown = WM_APP + 5;
inline constexpr UINT gui_shutdown = WM_APP + 6;

} // namespace ccs::tray_messages

#endif
