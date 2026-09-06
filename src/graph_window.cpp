#include "hardwarescope/graph_window.hpp"

#include "hardwarescope/app_commands.hpp"
#include "hardwarescope/ui_palette.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <string>

namespace hardwarescope {
namespace {
constexpr UINT_PTR kGraphAgeTimer = 0x48534741U;

COLORREF WinColor(const std::uint32_t rgb) noexcept {
    return RGB((rgb >> 16U) & 0xFFU, (rgb >> 8U) & 0xFFU, rgb & 0xFFU);
}

int Scale(const UINT dpi, const int value) noexcept {
    return MulDiv(value, static_cast<int>(dpi == 0U ? 96U : dpi), 96);
}

RECT ButtonRect(const UINT dpi, const int index) noexcept {
    const auto left = Scale(dpi, 12 + index * 88);
    return RECT{left, Scale(dpi, 8), left + Scale(dpi, 80), Scale(dpi, 38)};
}

bool Contains(const RECT& rectangle, const POINT point) noexcept {
    return point.x >= rectangle.left && point.x < rectangle.right
        && point.y >= rectangle.top && point.y < rectangle.bottom;
}

} // namespace

GraphWindow::~GraphWindow() {
    if (window_ != nullptr) DestroyWindow(window_);
    if (font_ != nullptr) DeleteObject(font_);
}

bool GraphWindow::RegisterWindowClass() const noexcept {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &GraphWindow::StaticWindowProcedure;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));
    window_class.lpszClassName = kWindowClass;
    return RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool GraphWindow::Initialize(const HWND owner, const AppSettings& settings) noexcept {
    owner_ = owner;
    settings_ = settings;
    history_.Configure(settings_);
    view_history_seconds_ = settings_.osd_graph_history_seconds;
    dpi_ = owner_ == nullptr ? 96U : std::max(96U, GetDpiForWindow(owner_));
    RecreateFont();
    if (!RegisterWindowClass()) return false;
    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT frame{0, 0, Scale(dpi_, static_cast<int>(settings_.floating_graph_width_px)), Scale(dpi_, static_cast<int>(settings_.floating_graph_height_px))};
    static_cast<void>(AdjustWindowRectExForDpi(&frame, style, FALSE, 0U, dpi_));
    const auto x = settings_.floating_graph_x >= 0 ? settings_.floating_graph_x : CW_USEDEFAULT;
    const auto y = settings_.floating_graph_y >= 0 ? settings_.floating_graph_y : CW_USEDEFAULT;
    window_ = CreateWindowExW(
        settings_.floating_graph_topmost ? WS_EX_TOPMOST : 0U,
        kWindowClass,
        L"HardwareScope Graph",
        style,
        x,
        y,
        frame.right - frame.left,
        frame.bottom - frame.top,
        owner_,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr) return false;
    const BOOL dark = settings_.theme != Theme::light ? TRUE : FALSE;
    static_cast<void>(DwmSetWindowAttribute(window_, 20U, &dark, sizeof(dark)));
    if (settings_.floating_graph_enabled) ShowWindow(window_, SW_SHOWNOACTIVATE);
    return true;
}

void GraphWindow::ApplySettings(const AppSettings& settings) noexcept {
    const bool font_changed = settings.interface_text_scale_percent != settings_.interface_text_scale_percent;
    settings_ = settings;
    history_.Configure(settings_);
    view_history_seconds_ = std::min(view_history_seconds_, settings_.osd_graph_history_seconds);
    if (font_changed) RecreateFont();
    if (window_ == nullptr) return;
    SetWindowPos(
        window_,
        settings_.floating_graph_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const BOOL dark = settings_.theme != Theme::light ? TRUE : FALSE;
    static_cast<void>(DwmSetWindowAttribute(window_, 20U, &dark, sizeof(dark)));
    ShowWindow(window_, settings_.floating_graph_enabled ? SW_SHOWNOACTIVATE : SW_HIDE);
    InvalidateRect(window_, nullptr, FALSE);
}

void GraphWindow::Update(const SensorSnapshot& snapshot) noexcept {
    if (!settings_.floating_graph_enabled || window_ == nullptr || !IsWindowVisible(window_)) return;
    history_.Update(snapshot, GetTickCount64());
    InvalidateRect(window_, nullptr, FALSE);
}

void GraphWindow::CapturePlacement(AppSettings& settings) const noexcept {
    if (window_ == nullptr || IsIconic(window_)) return;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(window_, &placement)) return;
    const auto& bounds = placement.rcNormalPosition;
    settings.floating_graph_x = bounds.left;
    settings.floating_graph_y = bounds.top;
    settings.floating_graph_width_px = static_cast<std::uint32_t>(std::max(360, MulDiv(bounds.right - bounds.left, 96, static_cast<int>(dpi_))));
    settings.floating_graph_height_px = static_cast<std::uint32_t>(std::max(220, MulDiv(bounds.bottom - bounds.top, 96, static_cast<int>(dpi_))));
}

void GraphWindow::DisplayChanged() noexcept {
    if (window_ == nullptr) return;
    dpi_ = std::max(96U, GetDpiForWindow(window_));
    RecreateFont();
    InvalidateRect(window_, nullptr, FALSE);
}

void GraphWindow::RecreateFont() noexcept {
    if (font_ != nullptr) DeleteObject(font_);
    const auto height = -std::max(Scale(dpi_, 11), MulDiv(Scale(dpi_, 13), static_cast<int>(settings_.interface_text_scale_percent), 100));
    font_ = CreateFontW(height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

LRESULT CALLBACK GraphWindow::StaticWindowProcedure(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) noexcept {
    GraphWindow* instance{};
    if (message == WM_NCCREATE) {
        instance = static_cast<GraphWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        instance->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
    } else {
        instance = reinterpret_cast<GraphWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return instance == nullptr ? DefWindowProcW(window, message, wparam, lparam) : instance->WindowProcedure(message, wparam, lparam);
}

LRESULT GraphWindow::WindowProcedure(const UINT message, const WPARAM wparam, const LPARAM lparam) noexcept {
    switch (message) {
    case WM_SHOWWINDOW:
        if (wparam) SetTimer(window_, kGraphAgeTimer, 1'000U, nullptr);
        else KillTimer(window_, kGraphAgeTimer);
        break;
    case WM_TIMER:
        if (wparam == kGraphAgeTimer && !IsIconic(window_)) {
            const auto now = GetTickCount64();
            if (now >= history_.LatestTick() + 500U && history_.AdvanceTime(now))
                InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        Render(dc, client);
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_LBUTTONUP:
        HandleClick(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
    case WM_DPICHANGED: {
        dpi_ = std::max(96U, static_cast<UINT>(HIWORD(wparam)));
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOACTIVATE | SWP_NOZORDER);
        RecreateFont();
        return 0;
    }
    case WM_EXITSIZEMOVE:
        static_cast<void>(PostMessageW(owner_, kGraphWindowPlacementChangedMessage, 0U, 0U));
        return 0;
    case WM_CLOSE:
        ShowWindow(window_, SW_HIDE);
        static_cast<void>(PostMessageW(owner_, kGraphWindowClosedMessage, 0U, 0U));
        return 0;
    default: break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void GraphWindow::HandleClick(const POINT point) noexcept {
    if (Contains(ButtonRect(dpi_, 0), point)) {
        history_.SetPaused(!history_.Paused());
    } else if (Contains(ButtonRect(dpi_, 1), point)) {
        history_.Clear();
    } else if (Contains(ButtonRect(dpi_, 2), point)) {
        view_history_seconds_ = std::max(5U, view_history_seconds_ / 2U);
    } else if (Contains(ButtonRect(dpi_, 3), point)) {
        view_history_seconds_ = std::min(settings_.osd_graph_history_seconds, std::max(5U, view_history_seconds_ * 2U));
    } else {
        return;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void GraphWindow::Render(const HDC destination, const RECT& client) noexcept {
    const auto width = std::max(1L, client.right - client.left);
    const auto height = std::max(1L, client.bottom - client.top);
    const auto memory = CreateCompatibleDC(destination);
    const auto bitmap = CreateCompatibleBitmap(destination, width, height);
    const auto previous_bitmap = SelectObject(memory, bitmap);
    const auto palette = PaletteFor(settings_.theme, settings_.text_color_rgb, settings_.high_contrast);
    const auto background = CreateSolidBrush(WinColor(palette.background));
    const auto surface = CreateSolidBrush(WinColor(palette.surface));
    const auto line = CreateSolidBrush(WinColor(palette.line));
    FillRect(memory, &client, background);
    const RECT toolbar{0, 0, width, Scale(dpi_, 48)};
    FillRect(memory, &toolbar, surface);
    auto toolbar_edge = toolbar;
    toolbar_edge.top = toolbar.bottom - 1;
    FillRect(memory, &toolbar_edge, line);
    const auto old_font = SelectObject(memory, font_);
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, WinColor(palette.text));
    std::array<const wchar_t*, 4U> labels{L"Pause", L"Reset", L"Zoom in", L"Zoom out"};
    for (int index{}; index < static_cast<int>(labels.size()); ++index) {
        auto button = ButtonRect(dpi_, index);
        if (index == 0 && history_.Paused()) labels[0] = L"Resume";
        FillRect(memory, &button, background);
        FrameRect(memory, &button, line);
        DrawTextW(memory, labels[index], -1, &button, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }

    RECT plot{Scale(dpi_, 58), Scale(dpi_, 82), width - Scale(dpi_, 64), height - Scale(dpi_, 42)};
    if (plot.right <= plot.left || plot.bottom <= plot.top) {
        BitBlt(destination, 0, 0, width, height, memory, 0, 0, SRCCOPY);
        SelectObject(memory, previous_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        DeleteObject(background);
        DeleteObject(surface);
        DeleteObject(line);
        return;
    }
    const auto grid_pen = CreatePen(PS_DOT, 1, WinColor(palette.line));
    auto old_pen = SelectObject(memory, grid_pen);
    for (int division{}; division <= 4; ++division) {
        const auto y = plot.top + (plot.bottom - plot.top) * division / 4;
        MoveToEx(memory, plot.left, y, nullptr);
        LineTo(memory, plot.right, y);
        const auto x = plot.left + (plot.right - plot.left) * division / 4;
        MoveToEx(memory, x, plot.top, nullptr);
        LineTo(memory, x, plot.bottom);
    }
    SelectObject(memory, old_pen);
    DeleteObject(grid_pen);

    auto range = history_.Range(view_history_seconds_);
    const auto span = std::max(0.001, range.maximum - range.minimum);
    const auto view_milliseconds = static_cast<std::uint64_t>(view_history_seconds_) * 1'000ULL;
    for (std::size_t series_index{}; series_index < history_.SeriesCount(); ++series_index) {
        const auto& series = history_.Series(series_index);
        if (series.count < 2U) continue;
        const auto graph_pen = CreatePen(PS_SOLID, Scale(dpi_, static_cast<int>(settings_.osd_graph_line_thickness_px)), WinColor(settings_.osd_graph_colors_rgb[series_index]));
        old_pen = SelectObject(memory, graph_pen);
        const auto newest_tick = history_.LatestTick();
        std::size_t start{};
        while (start + 1U < series.count && newest_tick - series.Timestamp(start) > view_milliseconds) ++start;
        for (std::size_t sample_index = start; sample_index < series.count; ++sample_index) {
            const auto sample_tick = series.Timestamp(sample_index);
            const auto age = newest_tick >= sample_tick ? newest_tick - sample_tick : 0U;
            const auto x = plot.right - static_cast<int>(std::min(age, view_milliseconds) * static_cast<std::uint64_t>(plot.right - plot.left) / std::max<std::uint64_t>(1U, view_milliseconds));
            const auto normalized = std::clamp((series.Sample(sample_index) - range.minimum) / span, 0.0, 1.0);
            const auto y = plot.bottom - static_cast<int>(std::llround(normalized * static_cast<double>(plot.bottom - plot.top)));
            if (sample_index == start || series.BreakBefore(sample_index)) MoveToEx(memory, x, y, nullptr);
            else LineTo(memory, x, y);
        }
        SelectObject(memory, old_pen);
        DeleteObject(graph_pen);
    }

    SetTextColor(memory, WinColor(palette.muted));
    wchar_t scale[48]{};
    static_cast<void>(swprintf_s(scale, L"%.1f", range.maximum));
    RECT maximum_label{plot.right + Scale(dpi_, 6), plot.top - Scale(dpi_, 9), width - Scale(dpi_, 6), plot.top + Scale(dpi_, 14)};
    DrawTextW(memory, scale, -1, &maximum_label, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    static_cast<void>(swprintf_s(scale, L"%.1f", range.minimum));
    RECT minimum_label{plot.right + Scale(dpi_, 6), plot.bottom - Scale(dpi_, 12), width - Scale(dpi_, 6), plot.bottom + Scale(dpi_, 12)};
    DrawTextW(memory, scale, -1, &minimum_label, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    wchar_t history_label[64]{};
    static_cast<void>(swprintf_s(history_label, L"%u seconds", view_history_seconds_));
    RECT time_label{plot.left, plot.bottom + Scale(dpi_, 7), plot.right, height - Scale(dpi_, 4)};
    DrawTextW(memory, history_label, -1, &time_label, DT_SINGLELINE | DT_CENTER | DT_TOP);

    const auto legend_width = std::max(1L, width / static_cast<LONG>(std::max<std::size_t>(1U, history_.SeriesCount())));
    for (std::size_t index{}; index < history_.SeriesCount(); ++index) {
        const auto& series = history_.Series(index);
        if (!series.available) continue;
        std::wstring legend = series.name.data();
        wchar_t value[64]{};
        static_cast<void>(swprintf_s(value, L"  %.1f %s", series.current, GraphUnitSuffix(series.unit)));
        legend += value;
        SetTextColor(memory, WinColor(settings_.osd_graph_colors_rgb[index]));
        RECT legend_bounds{static_cast<LONG>(index) * legend_width + Scale(dpi_, 8), Scale(dpi_, 52), static_cast<LONG>(index + 1U) * legend_width - Scale(dpi_, 6), Scale(dpi_, 78)};
        DrawTextW(memory, legend.c_str(), static_cast<int>(legend.size()), &legend_bounds, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }
    if (history_.Paused()) {
        SetTextColor(memory, WinColor(palette.accent));
        RECT paused{width - Scale(dpi_, 130), Scale(dpi_, 8), width - Scale(dpi_, 12), Scale(dpi_, 38)};
        DrawTextW(memory, L"PAUSED", -1, &paused, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
    }

    SelectObject(memory, old_font);
    BitBlt(destination, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, previous_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    DeleteObject(background);
    DeleteObject(surface);
    DeleteObject(line);
}

} // namespace hardwarescope
