#pragma once

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ccs {

struct WindowsThemeMetrics {
    int radius = 8;
    int radius_large = 12;
    int padding = 10;
    int border = 1;
};

struct WindowsThemePalette {
    COLORREF canvas = RGB(246, 248, 247);
    COLORREF surface = RGB(255, 255, 255);
    COLORREF surface_subtle = RGB(237, 242, 240);
    COLORREF border = RGB(207, 217, 213);
    COLORREF text = RGB(24, 33, 29);
    COLORREF text_muted = RGB(94, 109, 102);
    COLORREF accent = RGB(16, 122, 103);
    COLORREF accent_text = RGB(255, 255, 255);
    COLORREF success = RGB(39, 132, 92);
    COLORREF warning = RGB(173, 111, 22);
    COLORREF danger = RGB(190, 58, 62);
    COLORREF disabled = RGB(151, 161, 156);
};

class WindowsTheme final {
public:
    WindowsTheme();
    ~WindowsTheme();

    WindowsTheme(const WindowsTheme&) = delete;
    WindowsTheme& operator=(const WindowsTheme&) = delete;

    void refresh(HWND window, UINT dpi);
    void apply_to_window(HWND window) const;
    void apply_to_control(HWND control) const;

    const WindowsThemeMetrics& metrics() const noexcept;
    const WindowsThemePalette& palette() const noexcept;
    HBRUSH canvas_brush() const noexcept;
    HBRUSH surface_brush() const noexcept;
    HBRUSH subtle_brush() const noexcept;
    bool dark() const noexcept;
    bool high_contrast() const noexcept;

    void draw_rounded_rectangle(
        HDC dc,
        const RECT& rectangle,
        int radius,
        COLORREF fill,
        COLORREF border,
        int border_width = 1) const;
    void fill_rounded_rectangle(
        HDC dc,
        const RECT& rectangle,
        int radius,
        COLORREF fill) const;
    void stroke_rounded_rectangle(
        HDC dc,
        const RECT& rectangle,
        int radius,
        COLORREF border,
        int border_width = 1) const;
    void fill_ellipse(HDC dc, const RECT& rectangle, COLORREF fill) const;
    void draw_chevron_down(
        HDC dc,
        const RECT& rectangle,
        COLORREF color,
        int stroke_width = 1) const;
    void draw_control_frame(
        HDC dc,
        const RECT& rectangle,
        int radius,
        COLORREF parent_background,
        COLORREF border,
        int border_width = 1) const;

private:
    void destroy_brushes() noexcept;

    WindowsThemeMetrics metrics_;
    WindowsThemePalette palette_;
    HBRUSH canvas_brush_ = nullptr;
    HBRUSH surface_brush_ = nullptr;
    HBRUSH subtle_brush_ = nullptr;
    ULONG_PTR gdiplus_token_ = 0;
    bool dark_ = false;
    bool high_contrast_ = false;
};

} // namespace ccs

#endif
