#include "hosts/windows/windows_theme.hpp"

#ifdef _WIN32

#include <dwmapi.h>
#include <gdiplus.h>
#include <uxtheme.h>

#include <algorithm>
#include <iterator>

namespace ccs {

namespace {

bool system_uses_dark_apps() {
    DWORD light_theme = 1;
    DWORD size = sizeof(light_theme);
    const auto result = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &light_theme,
        &size);
    return result == ERROR_SUCCESS && light_theme == 0;
}

bool system_high_contrast() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

WindowsThemePalette light_palette() {
    return {};
}

WindowsThemePalette dark_palette() {
    WindowsThemePalette palette;
    palette.canvas = RGB(24, 28, 26);
    palette.surface = RGB(33, 38, 35);
    palette.surface_subtle = RGB(42, 49, 45);
    palette.border = RGB(69, 80, 74);
    palette.text = RGB(238, 243, 240);
    palette.text_muted = RGB(164, 177, 170);
    palette.accent = RGB(54, 180, 151);
    palette.accent_text = RGB(12, 31, 25);
    palette.success = RGB(76, 190, 130);
    palette.warning = RGB(223, 160, 66);
    palette.danger = RGB(235, 101, 105);
    palette.disabled = RGB(105, 117, 111);
    return palette;
}

WindowsThemePalette high_contrast_palette() {
    WindowsThemePalette palette;
    palette.canvas = GetSysColor(COLOR_WINDOW);
    palette.surface = GetSysColor(COLOR_WINDOW);
    palette.surface_subtle = GetSysColor(COLOR_BTNFACE);
    palette.border = GetSysColor(COLOR_WINDOWTEXT);
    palette.text = GetSysColor(COLOR_WINDOWTEXT);
    palette.text_muted = GetSysColor(COLOR_GRAYTEXT);
    palette.accent = GetSysColor(COLOR_HIGHLIGHT);
    palette.accent_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
    palette.success = GetSysColor(COLOR_HIGHLIGHT);
    palette.warning = GetSysColor(COLOR_HIGHLIGHT);
    palette.danger = GetSysColor(COLOR_HIGHLIGHT);
    palette.disabled = GetSysColor(COLOR_GRAYTEXT);
    return palette;
}

Gdiplus::Color gdiplus_color(COLORREF color) {
    return Gdiplus::Color(
        255,
        GetRValue(color),
        GetGValue(color),
        GetBValue(color));
}

void add_rounded_rectangle(
    Gdiplus::GraphicsPath& path,
    const Gdiplus::RectF& rectangle,
    float radius) {
    const float diameter = std::max(
        0.0F,
        std::min(radius * 2.0F, std::min(rectangle.Width, rectangle.Height)));
    if (diameter <= 0.0F) {
        path.AddRectangle(rectangle);
        return;
    }
    path.AddArc(rectangle.X, rectangle.Y, diameter, diameter, 180.0F, 90.0F);
    path.AddArc(
        rectangle.GetRight() - diameter,
        rectangle.Y,
        diameter,
        diameter,
        270.0F,
        90.0F);
    path.AddArc(
        rectangle.GetRight() - diameter,
        rectangle.GetBottom() - diameter,
        diameter,
        diameter,
        0.0F,
        90.0F);
    path.AddArc(
        rectangle.X,
        rectangle.GetBottom() - diameter,
        diameter,
        diameter,
        90.0F,
        90.0F);
    path.CloseFigure();
}

void configure_graphics(Gdiplus::Graphics& graphics) {
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}

void fallback_rounded_rectangle(
    HDC dc,
    const RECT& rectangle,
    int radius,
    COLORREF fill,
    COLORREF border,
    int border_width) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, border_width), border);
    const auto old_brush = SelectObject(dc, brush);
    const auto old_pen = SelectObject(dc, pen);
    RoundRect(
        dc,
        rectangle.left,
        rectangle.top,
        rectangle.right,
        rectangle.bottom,
        radius * 2,
        radius * 2);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

} // namespace

WindowsTheme::WindowsTheme() {
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &input, nullptr) != Gdiplus::Ok) {
        gdiplus_token_ = 0;
    }
}

WindowsTheme::~WindowsTheme() {
    destroy_brushes();
    if (gdiplus_token_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplus_token_);
    }
}

void WindowsTheme::refresh(HWND window, UINT dpi) {
    high_contrast_ = system_high_contrast();
    dark_ = !high_contrast_ && system_uses_dark_apps();
    palette_ = high_contrast_
        ? high_contrast_palette()
        : (dark_ ? dark_palette() : light_palette());
    metrics_.radius = MulDiv(8, static_cast<int>(dpi), 96);
    metrics_.radius_large = MulDiv(12, static_cast<int>(dpi), 96);
    metrics_.padding = MulDiv(10, static_cast<int>(dpi), 96);
    metrics_.border = std::max(1, MulDiv(1, static_cast<int>(dpi), 96));
    destroy_brushes();
    canvas_brush_ = CreateSolidBrush(palette_.canvas);
    surface_brush_ = CreateSolidBrush(palette_.surface);
    subtle_brush_ = CreateSolidBrush(palette_.surface_subtle);
    apply_to_window(window);
}

void WindowsTheme::apply_to_window(HWND window) const {
    if (window == nullptr) {
        return;
    }
    const BOOL use_dark = dark_ ? TRUE : FALSE;
    constexpr auto dark_attribute = static_cast<DWMWINDOWATTRIBUTE>(20);
    (void)DwmSetWindowAttribute(
        window, dark_attribute, &use_dark, sizeof(use_dark));
    constexpr auto corner_attribute = static_cast<DWMWINDOWATTRIBUTE>(33);
    constexpr int round_corner = 2;
    (void)DwmSetWindowAttribute(
        window, corner_attribute, &round_corner, sizeof(round_corner));
}

void WindowsTheme::apply_to_control(HWND control) const {
    if (control == nullptr || high_contrast_) {
        return;
    }
    wchar_t class_name[32]{};
    (void)GetClassNameW(control, class_name, static_cast<int>(std::size(class_name)));
    const bool cfd_control = lstrcmpiW(class_name, L"ComboBox") == 0
        || lstrcmpiW(class_name, L"Edit") == 0;
    const wchar_t* sub_app = dark_
        ? (cfd_control ? L"DarkMode_CFD" : L"DarkMode_Explorer")
        : L"Explorer";
    (void)SetWindowTheme(control, sub_app, nullptr);
}

const WindowsThemeMetrics& WindowsTheme::metrics() const noexcept {
    return metrics_;
}

const WindowsThemePalette& WindowsTheme::palette() const noexcept {
    return palette_;
}

HBRUSH WindowsTheme::canvas_brush() const noexcept {
    return canvas_brush_;
}

HBRUSH WindowsTheme::surface_brush() const noexcept {
    return surface_brush_;
}

HBRUSH WindowsTheme::subtle_brush() const noexcept {
    return subtle_brush_;
}

bool WindowsTheme::dark() const noexcept {
    return dark_;
}

bool WindowsTheme::high_contrast() const noexcept {
    return high_contrast_;
}

void WindowsTheme::draw_rounded_rectangle(
    HDC dc,
    const RECT& rectangle,
    int radius,
    COLORREF fill,
    COLORREF border,
    int border_width) const {
    if (dc == nullptr || rectangle.right <= rectangle.left
        || rectangle.bottom <= rectangle.top) {
        return;
    }
    if (gdiplus_token_ == 0) {
        fallback_rounded_rectangle(
            dc, rectangle, radius, fill, border, border_width);
        return;
    }
    Gdiplus::Graphics graphics(dc);
    configure_graphics(graphics);
    const float inset = std::max(0.5F, static_cast<float>(border_width) / 2.0F);
    const Gdiplus::RectF bounds(
        static_cast<float>(rectangle.left) + inset,
        static_cast<float>(rectangle.top) + inset,
        std::max(0.0F, static_cast<float>(rectangle.right - rectangle.left) - inset * 2.0F),
        std::max(0.0F, static_cast<float>(rectangle.bottom - rectangle.top) - inset * 2.0F));
    Gdiplus::GraphicsPath path;
    add_rounded_rectangle(path, bounds, std::max(0.0F, static_cast<float>(radius) - inset));
    Gdiplus::SolidBrush brush(gdiplus_color(fill));
    Gdiplus::Pen pen(gdiplus_color(border), static_cast<float>(std::max(1, border_width)));
    graphics.FillPath(&brush, &path);
    graphics.DrawPath(&pen, &path);
}

void WindowsTheme::fill_rounded_rectangle(
    HDC dc,
    const RECT& rectangle,
    int radius,
    COLORREF fill) const {
    draw_rounded_rectangle(dc, rectangle, radius, fill, fill, 1);
}

void WindowsTheme::stroke_rounded_rectangle(
    HDC dc,
    const RECT& rectangle,
    int radius,
    COLORREF border,
    int border_width) const {
    if (dc == nullptr || rectangle.right <= rectangle.left
        || rectangle.bottom <= rectangle.top) {
        return;
    }
    if (gdiplus_token_ == 0) {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, border_width), border);
        const auto old_pen = SelectObject(dc, pen);
        const auto old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(
            dc,
            rectangle.left,
            rectangle.top,
            rectangle.right,
            rectangle.bottom,
            radius * 2,
            radius * 2);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        return;
    }
    Gdiplus::Graphics graphics(dc);
    configure_graphics(graphics);
    const float inset = std::max(0.5F, static_cast<float>(border_width) / 2.0F);
    const Gdiplus::RectF bounds(
        static_cast<float>(rectangle.left) + inset,
        static_cast<float>(rectangle.top) + inset,
        std::max(0.0F, static_cast<float>(rectangle.right - rectangle.left) - inset * 2.0F),
        std::max(0.0F, static_cast<float>(rectangle.bottom - rectangle.top) - inset * 2.0F));
    Gdiplus::GraphicsPath path;
    add_rounded_rectangle(path, bounds, std::max(0.0F, static_cast<float>(radius) - inset));
    Gdiplus::Pen pen(gdiplus_color(border), static_cast<float>(std::max(1, border_width)));
    graphics.DrawPath(&pen, &path);
}

void WindowsTheme::fill_ellipse(
    HDC dc,
    const RECT& rectangle,
    COLORREF fill) const {
    if (dc == nullptr || rectangle.right <= rectangle.left
        || rectangle.bottom <= rectangle.top) {
        return;
    }
    if (gdiplus_token_ == 0) {
        HBRUSH brush = CreateSolidBrush(fill);
        const auto old_brush = SelectObject(dc, brush);
        const auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(brush);
        return;
    }
    Gdiplus::Graphics graphics(dc);
    configure_graphics(graphics);
    Gdiplus::SolidBrush brush(gdiplus_color(fill));
    graphics.FillEllipse(
        &brush,
        static_cast<Gdiplus::REAL>(rectangle.left),
        static_cast<Gdiplus::REAL>(rectangle.top),
        static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left),
        static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top));
}

void WindowsTheme::draw_chevron_down(
    HDC dc,
    const RECT& rectangle,
    COLORREF color,
    int stroke_width) const {
    if (dc == nullptr || rectangle.right <= rectangle.left
        || rectangle.bottom <= rectangle.top) {
        return;
    }
    const float center_x = static_cast<float>(rectangle.left + rectangle.right) / 2.0F;
    const float center_y = static_cast<float>(rectangle.top + rectangle.bottom) / 2.0F;
    const int available = static_cast<int>(std::min(
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top));
    const float half_width = static_cast<float>(std::max(2, available / 5));
    const float half_height = std::max(1.0F, half_width * 0.55F);
    if (gdiplus_token_ == 0) {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, stroke_width), color);
        const auto old_pen = SelectObject(dc, pen);
        MoveToEx(
            dc,
            static_cast<int>(center_x - half_width),
            static_cast<int>(center_y - half_height),
            nullptr);
        LineTo(dc, static_cast<int>(center_x), static_cast<int>(center_y + half_height));
        LineTo(
            dc,
            static_cast<int>(center_x + half_width),
            static_cast<int>(center_y - half_height));
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        return;
    }
    Gdiplus::Graphics graphics(dc);
    configure_graphics(graphics);
    Gdiplus::Pen pen(
        gdiplus_color(color),
        static_cast<Gdiplus::REAL>(std::max(1, stroke_width)));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawLine(
        &pen,
        center_x - half_width,
        center_y - half_height,
        center_x,
        center_y + half_height);
    graphics.DrawLine(
        &pen,
        center_x,
        center_y + half_height,
        center_x + half_width,
        center_y - half_height);
}

void WindowsTheme::draw_control_frame(
    HDC dc,
    const RECT& rectangle,
    int radius,
    COLORREF parent_background,
    COLORREF border,
    int border_width) const {
    if (dc == nullptr || rectangle.right <= rectangle.left
        || rectangle.bottom <= rectangle.top) {
        return;
    }
    if (gdiplus_token_ == 0) {
        stroke_rounded_rectangle(
            dc, rectangle, radius, border, border_width);
        return;
    }

    const int width = rectangle.right - rectangle.left;
    const int height = rectangle.bottom - rectangle.top;
    Gdiplus::Graphics graphics(dc);
    configure_graphics(graphics);
    const Gdiplus::RectF bounds(
        static_cast<float>(rectangle.left),
        static_cast<float>(rectangle.top),
        static_cast<float>(width),
        static_cast<float>(height));
    Gdiplus::GraphicsPath fill_path;
    add_rounded_rectangle(fill_path, bounds, static_cast<float>(radius));
    Gdiplus::SolidBrush parent_brush(gdiplus_color(parent_background));
    Gdiplus::GraphicsPath outside(Gdiplus::FillModeAlternate);
    outside.AddRectangle(bounds);
    outside.AddPath(&fill_path, FALSE);
    graphics.FillPath(&parent_brush, &outside);
    stroke_rounded_rectangle(
        dc, rectangle, radius, border, border_width);
}

void WindowsTheme::destroy_brushes() noexcept {
    if (canvas_brush_ != nullptr) DeleteObject(canvas_brush_);
    if (surface_brush_ != nullptr) DeleteObject(surface_brush_);
    if (subtle_brush_ != nullptr) DeleteObject(subtle_brush_);
    canvas_brush_ = nullptr;
    surface_brush_ = nullptr;
    subtle_brush_ = nullptr;
}

} // namespace ccs

#endif
