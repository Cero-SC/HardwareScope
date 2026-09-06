#include "hardwarescope/osd_window.hpp"
#include "hardwarescope/osd_model.hpp"
#include "hardwarescope/sensor_view_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hardwarescope {
namespace {

SIZE TextSize(const HDC dc, const HFONT font, const std::wstring& text) noexcept {
    const auto previous = SelectObject(dc, font);
    SIZE size{};
    static_cast<void>(GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size));
    static_cast<void>(SelectObject(dc, previous));
    return size;
}

} // namespace

OsdWindow::OsdWindow(const HINSTANCE instance, const OsdWindowRole role) noexcept
    : instance_(instance), role_(role) {}

OsdWindow::~OsdWindow() {
    if (window_ != nullptr) DestroyWindow(window_);
    DestroySurface();
    if (sensor_font_ != nullptr) DeleteObject(sensor_font_);
    if (fps_font_ != nullptr) DeleteObject(fps_font_);
}

bool OsdWindow::Initialize(const HWND monitor_anchor, const AppSettings& settings) noexcept {
    monitor_anchor_ = monitor_anchor;
    settings_ = settings;
    if (!RegisterWindowClass()) return false;
    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WindowClassName(),
        role_ == OsdWindowRole::fps ? L"HardwareScope FPS OSD" : L"HardwareScope OSD",
        WS_POPUP,
        0,
        0,
        1,
        1,
        nullptr,
        nullptr,
        instance_,
        nullptr);
    if (window_ == nullptr) return false;
    static_cast<void>(RefreshDpi());
    RecreateFonts();
    visible_ = settings_.show_osd;
    return true;
}

bool OsdWindow::RefreshDpi() noexcept {
    const auto next_dpi = monitor_anchor_ != nullptr ? GetDpiForWindow(monitor_anchor_) : 96U;
    const auto normalized = next_dpi == 0U ? 96U : next_dpi;
    if (normalized == dpi_) return false;
    dpi_ = normalized;
    return true;
}

int OsdWindow::ScaleForDpi(const int value) const noexcept {
    return MulDiv(value, static_cast<int>(dpi_), 96);
}

void OsdWindow::DisplayChanged() noexcept {
    if (RefreshDpi()) RecreateFonts();
    Render();
}

bool OsdWindow::RegisterWindowClass() const noexcept {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &OsdWindow::WindowProcedure;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = WindowClassName();
    return RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

const wchar_t* OsdWindow::WindowClassName() const noexcept {
    return role_ == OsdWindowRole::fps ? kFpsWindowClass : kWindowClass;
}

LRESULT CALLBACK OsdWindow::WindowProcedure(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) noexcept {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, message, wparam, lparam);
}

void OsdWindow::ApplySettings(const AppSettings& settings) noexcept {
    const bool fonts_changed = settings.osd_scale_percent != settings_.osd_scale_percent
        || settings.fps_scale_percent != settings_.fps_scale_percent;
    settings_ = settings;
    if (fonts_changed) RecreateFonts();
    graph_history_.Configure(settings_);
    if (!settings_.osd_graph_enabled) graph_history_.Clear();
    visible_ = settings_.show_osd;
    Render();
}

void OsdWindow::Update(const SensorSnapshot& snapshot) noexcept {
    CopySnapshot(snapshot, snapshot_);
    if (GraphBelongsOnThisSurface()) graph_history_.Update(snapshot_, GetTickCount64());
    Render();
}

void OsdWindow::SetVisible(const bool visible) noexcept {
    visible_ = visible;
    if (!visible_ && window_ != nullptr) ShowWindow(window_, SW_HIDE);
    else Render();
}

void OsdWindow::RecreateFonts() noexcept {
    if (sensor_font_ != nullptr) DeleteObject(sensor_font_);
    if (fps_font_ != nullptr) DeleteObject(fps_font_);
    const auto sensor_height = -std::max(
        ScaleForDpi(10),
        MulDiv(ScaleForDpi(16), static_cast<int>(settings_.osd_scale_percent), 100));
    const auto fps_height = -std::max(10, MulDiv(-sensor_height, static_cast<int>(settings_.fps_scale_percent), 100));
    sensor_font_ = CreateFontW(sensor_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    fps_font_ = CreateFontW(fps_height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

bool OsdWindow::EnsureSurface(const int width, const int height) noexcept {
    if (memory_dc_ != nullptr && width == surface_width_ && height == surface_height_) return true;
    DestroySurface();
    memory_dc_ = CreateCompatibleDC(nullptr);
    if (memory_dc_ == nullptr) return false;
    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = width;
    information.bmiHeader.biHeight = -height;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(memory_dc_, &information, DIB_RGB_COLORS, &pixels_, nullptr, 0);
    if (bitmap_ == nullptr || pixels_ == nullptr) {
        DestroySurface();
        return false;
    }
    original_bitmap_ = SelectObject(memory_dc_, bitmap_);
    surface_width_ = width;
    surface_height_ = height;
    return true;
}

void OsdWindow::DestroySurface() noexcept {
    if (memory_dc_ != nullptr && original_bitmap_ != nullptr) static_cast<void>(SelectObject(memory_dc_, original_bitmap_));
    if (bitmap_ != nullptr) DeleteObject(bitmap_);
    if (memory_dc_ != nullptr) DeleteDC(memory_dc_);
    memory_dc_ = nullptr;
    bitmap_ = nullptr;
    original_bitmap_ = nullptr;
    pixels_ = nullptr;
    surface_width_ = 0;
    surface_height_ = 0;
}

bool OsdWindow::GraphBelongsOnThisSurface() const noexcept {
    if (!settings_.osd_graph_enabled || settings_.osd_graph_sensor_count == 0U) return false;
    const auto primary_id = settings_.osd_graph_sensor_ids[0];
    const auto fps_graph = primary_id == kFpsSensorId
        || primary_id == kFpsOnePercentLowSensorId
        || primary_id == kFpsFrameTimeSensorId;
    if (fps_graph) return settings_.fps_separate_position
        ? role_ == OsdWindowRole::fps
        : role_ == OsdWindowRole::primary;
    return role_ == OsdWindowRole::primary;
}

void OsdWindow::Render() noexcept {
    if (window_ == nullptr || !visible_) return;
    const auto items = BuildOsdSurfaceItems(snapshot_, settings_, role_ == OsdWindowRole::fps);
    const auto draw_graph = GraphBelongsOnThisSurface()
        && graph_history_.SeriesCount() != 0U
        && graph_history_.Series(0U).count >= 2U;
    if (items.empty() && !draw_graph) {
        ShowWindow(window_, SW_HIDE);
        return;
    }
    if (memory_dc_ == nullptr && !EnsureSurface(1, 1)) return;

    const auto padding = std::max(
        1,
        MulDiv(ScaleForDpi(settings_.osd_background ? 8 : 2), static_cast<int>(settings_.osd_scale_percent), 100));
    const auto spacing = std::max(
        0,
        MulDiv(ScaleForDpi(static_cast<int>(settings_.osd_spacing_px)), static_cast<int>(settings_.osd_scale_percent), 100));
    std::vector<SIZE> sizes;
    sizes.reserve(items.size());
    int width = padding * 2;
    int height = padding * 2;
    if (settings_.osd_layout == OsdLayout::vertical) {
        for (const auto& item : items) {
            const auto size = TextSize(memory_dc_, item.fps ? fps_font_ : sensor_font_, item.text);
            sizes.push_back(size);
            width = std::max(width, static_cast<int>(size.cx) + padding * 2);
            height += size.cy;
        }
        if (!items.empty()) height += spacing * static_cast<int>(items.size() - 1U);
    } else {
        const auto separator_size = TextSize(memory_dc_, sensor_font_, L"|");
        OsdHardwareGroup previous_group = OsdHardwareGroup::fps;
        bool have_previous = false;
        for (const auto& item : items) {
            const auto size = TextSize(memory_dc_, item.fps ? fps_font_ : sensor_font_, item.text);
            sizes.push_back(size);
            if (have_previous) width += spacing;
            if (have_previous && settings_.osd_group_separators && item.group != OsdHardwareGroup::fps && previous_group != item.group) width += separator_size.cx + spacing;
            width += size.cx;
            height = std::max(height, static_cast<int>(size.cy) + padding * 2);
            previous_group = item.group;
            have_previous = true;
        }
    }
    int graph_width{};
    int graph_height{};
    if (draw_graph) {
        graph_width = std::max(ScaleForDpi(80), MulDiv(ScaleForDpi(static_cast<int>(settings_.osd_graph_width_px)), static_cast<int>(settings_.osd_scale_percent), 100));
        graph_height = std::max(ScaleForDpi(24), MulDiv(ScaleForDpi(static_cast<int>(settings_.osd_graph_height_px)), static_cast<int>(settings_.osd_scale_percent), 100));
        width = std::max(width, graph_width + padding * 2);
        if (!items.empty()) height += spacing;
        height += graph_height;
    }
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (!EnsureSurface(width, height)) return;
    std::memset(pixels_, 0, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * sizeof(std::uint32_t));
    SetBkMode(memory_dc_, TRANSPARENT);
    SetTextColor(memory_dc_, RGB(255, 255, 255));

    auto draw_item = [&](const OsdDisplayItem& item, const SIZE size, const int x, const int y) {
        const auto previous_font = SelectObject(memory_dc_, item.fps ? fps_font_ : sensor_font_);
        static_cast<void>(TextOutW(memory_dc_, x, y, item.text.c_str(), static_cast<int>(item.text.size())));
        static_cast<void>(SelectObject(memory_dc_, previous_font));
        auto* pixels = static_cast<std::uint32_t*>(pixels_);
        const auto red = static_cast<std::uint8_t>((item.color_rgb >> 16U) & 0xFFU);
        const auto green = static_cast<std::uint8_t>((item.color_rgb >> 8U) & 0xFFU);
        const auto blue = static_cast<std::uint8_t>(item.color_rgb & 0xFFU);
        for (int row = std::max(0, y); row < std::min(height, y + static_cast<int>(size.cy) + 2); ++row) {
            for (int column = std::max(0, x); column < std::min(width, x + static_cast<int>(size.cx) + 2); ++column) {
                auto& pixel = pixels[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(column)];
                const auto coverage = static_cast<std::uint8_t>(std::max({pixel & 0xFFU, (pixel >> 8U) & 0xFFU, (pixel >> 16U) & 0xFFU}));
                if (coverage == 0U) continue;
                const auto alpha = static_cast<std::uint8_t>((static_cast<unsigned>(coverage) * settings_.osd_opacity_percent) / 100U);
                pixel = (static_cast<std::uint32_t>(alpha) << 24U)
                    | (static_cast<std::uint32_t>(red) * alpha / 255U << 16U)
                    | (static_cast<std::uint32_t>(green) * alpha / 255U << 8U)
                    | (static_cast<std::uint32_t>(blue) * alpha / 255U);
            }
        }
    };

    if (settings_.osd_layout == OsdLayout::vertical) {
        int y = padding;
        for (std::size_t index = 0; index < items.size(); ++index) {
            draw_item(items[index], sizes[index], padding, y);
            y += sizes[index].cy + spacing;
        }
    } else {
        int x = padding;
        OsdHardwareGroup previous_group = OsdHardwareGroup::fps;
        bool have_previous = false;
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (have_previous) x += spacing;
            if (have_previous && settings_.osd_group_separators && items[index].group != OsdHardwareGroup::fps && previous_group != items[index].group) {
                OsdDisplayItem separator{0U, L"|", items[index].group, items[index].color_rgb, false};
                const auto separator_size = TextSize(memory_dc_, sensor_font_, separator.text);
                draw_item(separator, separator_size, x, padding);
                x += separator_size.cx + spacing;
            }
            draw_item(items[index], sizes[index], x, padding);
            x += sizes[index].cx;
            previous_group = items[index].group;
            have_previous = true;
        }
    }

    if (draw_graph) {
        auto* const pixels = static_cast<std::uint32_t*>(pixels_);
        const auto graph_x = padding;
        const auto graph_y = height - padding - graph_height;
        const auto label_margin = settings_.osd_graph_labels ? std::min(ScaleForDpi(46), graph_width / 3) : 2;
        const auto legend_height = settings_.osd_graph_labels ? std::min(ScaleForDpi(18), graph_height / 4) : 2;
        const RECT plot{
            graph_x + 2,
            graph_y + legend_height,
            graph_x + graph_width - label_margin,
            graph_y + graph_height - (settings_.osd_graph_labels ? ScaleForDpi(14) : 2)};
        const auto range = graph_history_.Range();
        const auto range_span = std::max(0.001, range.maximum - range.minimum);
        const auto opacity = settings_.osd_opacity_percent;

        auto pixel_for = [&](const std::uint32_t rgb, const unsigned raw_alpha) noexcept {
            const auto alpha = static_cast<std::uint8_t>(raw_alpha * opacity / 100U);
            const auto red = static_cast<std::uint8_t>((rgb >> 16U) & 0xFFU);
            const auto green = static_cast<std::uint8_t>((rgb >> 8U) & 0xFFU);
            const auto blue = static_cast<std::uint8_t>(rgb & 0xFFU);
            return (static_cast<std::uint32_t>(alpha) << 24U)
                | (static_cast<std::uint32_t>(red) * alpha / 255U << 16U)
                | (static_cast<std::uint32_t>(green) * alpha / 255U << 8U)
                | (static_cast<std::uint32_t>(blue) * alpha / 255U);
        };
        auto set_pixel = [&](const int x, const int y, const std::uint32_t value) noexcept {
            if (x < graph_x || y < graph_y || x >= graph_x + graph_width || y >= graph_y + graph_height) return;
            pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = value;
        };
        const auto grid_pixel = pixel_for(settings_.text_color_rgb, 42U);
        for (int x{}; x < graph_width; ++x) {
            set_pixel(graph_x + x, graph_y, grid_pixel);
            set_pixel(graph_x + x, graph_y + graph_height - 1, grid_pixel);
        }
        for (int y{}; y < graph_height; ++y) {
            set_pixel(graph_x, graph_y + y, grid_pixel);
            set_pixel(graph_x + graph_width - 1, graph_y + y, grid_pixel);
        }
        if (settings_.osd_graph_grid && plot.right > plot.left && plot.bottom > plot.top) {
            for (int division = 1; division < 4; ++division) {
                const auto y = plot.top + (plot.bottom - plot.top) * division / 4;
                for (int x = plot.left; x <= plot.right; x += 2) set_pixel(x, y, grid_pixel);
            }
            for (int division = 1; division < 4; ++division) {
                const auto x = plot.left + (plot.right - plot.left) * division / 4;
                for (int y = plot.top; y <= plot.bottom; y += 2) set_pixel(x, y, grid_pixel);
            }
        }

        const auto plot_width = std::max(1L, plot.right - plot.left);
        const auto plot_height = std::max(1L, plot.bottom - plot.top);
        const auto view_milliseconds = static_cast<std::uint64_t>(settings_.osd_graph_history_seconds) * 1'000ULL;
        for (std::size_t series_index{}; series_index < graph_history_.SeriesCount(); ++series_index) {
            const auto& series = graph_history_.Series(series_index);
            if (series.count < 2U) continue;
            const auto line_pixel = pixel_for(settings_.osd_graph_colors_rgb[series_index], 235U);
            const auto newest_tick = graph_history_.LatestTick();
            auto point = [&](const std::size_t index) noexcept {
                const auto sample_tick = series.Timestamp(index);
                const auto age = newest_tick >= sample_tick ? newest_tick - sample_tick : 0U;
                const auto x = plot.right - static_cast<int>(std::min(age, view_milliseconds) * static_cast<std::uint64_t>(plot_width) / std::max<std::uint64_t>(1U, view_milliseconds));
                const auto normalized = std::clamp((series.Sample(index) - range.minimum) / range_span, 0.0, 1.0);
                const auto y = plot.bottom - static_cast<int>(std::llround(normalized * static_cast<double>(plot_height)));
                return POINT{x, y};
            };
            std::size_t first_visible{};
            while (first_visible + 1U < series.count && newest_tick - series.Timestamp(first_visible) > view_milliseconds) ++first_visible;
            auto previous = point(first_visible);
            for (std::size_t index = first_visible + 1U; index < series.count; ++index) {
                const auto next = point(index);
                if (series.BreakBefore(index)) { previous = next; continue; }
                auto x0 = previous.x;
                auto y0 = previous.y;
                const auto dx = std::abs(next.x - x0);
                const auto sx = x0 < next.x ? 1 : -1;
                const auto dy = -std::abs(next.y - y0);
                const auto sy = y0 < next.y ? 1 : -1;
                auto error = dx + dy;
                while (true) {
                    for (std::uint32_t thickness{}; thickness < settings_.osd_graph_line_thickness_px; ++thickness) {
                        set_pixel(x0, y0 + static_cast<int>(thickness), line_pixel);
                    }
                    if (x0 == next.x && y0 == next.y) break;
                    const auto doubled = error * 2;
                    if (doubled >= dy) { error += dy; x0 += sx; }
                    if (doubled <= dx) { error += dx; y0 += sy; }
                }
                previous = next;
            }
        }

        if (settings_.osd_graph_labels) {
            auto draw_label = [&](const std::wstring& text, const RECT bounds, const std::uint32_t color, const UINT alignment) noexcept {
                for (int y = std::max(graph_y, static_cast<int>(bounds.top)); y < std::min(graph_y + graph_height, static_cast<int>(bounds.bottom)); ++y) {
                    for (int x = std::max(graph_x, static_cast<int>(bounds.left)); x < std::min(graph_x + graph_width, static_cast<int>(bounds.right)); ++x) {
                        pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = 0U;
                    }
                }
                const auto previous_font = SelectObject(memory_dc_, sensor_font_);
                const auto previous_mode = SetBkMode(memory_dc_, TRANSPARENT);
                SetTextColor(memory_dc_, RGB(255, 255, 255));
                auto target = bounds;
                DrawTextW(memory_dc_, text.c_str(), static_cast<int>(text.size()), &target, DT_SINGLELINE | DT_VCENTER | alignment | DT_END_ELLIPSIS);
                SetBkMode(memory_dc_, previous_mode);
                SelectObject(memory_dc_, previous_font);
                for (int y = std::max(graph_y, static_cast<int>(bounds.top)); y < std::min(graph_y + graph_height, static_cast<int>(bounds.bottom)); ++y) {
                    for (int x = std::max(graph_x, static_cast<int>(bounds.left)); x < std::min(graph_x + graph_width, static_cast<int>(bounds.right)); ++x) {
                        auto& pixel = pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
                        const auto coverage = static_cast<std::uint8_t>(std::max({pixel & 0xFFU, (pixel >> 8U) & 0xFFU, (pixel >> 16U) & 0xFFU}));
                        if (coverage == 0U) continue;
                        const auto alpha = static_cast<std::uint8_t>(static_cast<unsigned>(coverage) * opacity / 100U);
                        const auto red = static_cast<std::uint8_t>((color >> 16U) & 0xFFU);
                        const auto green = static_cast<std::uint8_t>((color >> 8U) & 0xFFU);
                        const auto blue = static_cast<std::uint8_t>(color & 0xFFU);
                        pixel = (static_cast<std::uint32_t>(alpha) << 24U)
                            | (static_cast<std::uint32_t>(red) * alpha / 255U << 16U)
                            | (static_cast<std::uint32_t>(green) * alpha / 255U << 8U)
                            | (static_cast<std::uint32_t>(blue) * alpha / 255U);
                    }
                }
            };
            wchar_t scale_text[32]{};
            static_cast<void>(swprintf_s(scale_text, L"%.1f", range.maximum));
            draw_label(scale_text, RECT{plot.right + 3, plot.top - 8, graph_x + graph_width - 2, plot.top + 12}, settings_.text_color_rgb, DT_RIGHT);
            static_cast<void>(swprintf_s(scale_text, L"%.1f", range.minimum));
            draw_label(scale_text, RECT{plot.right + 3, plot.bottom - 10, graph_x + graph_width - 2, plot.bottom + 10}, settings_.text_color_rgb, DT_RIGHT);
            wchar_t time_text[32]{};
            static_cast<void>(swprintf_s(time_text, L"%us", settings_.osd_graph_history_seconds));
            draw_label(time_text, RECT{plot.left, plot.bottom, plot.left + ScaleForDpi(48), graph_y + graph_height - 1}, settings_.text_color_rgb, DT_LEFT);
            const auto value_width = std::max(1L, (plot.right - plot.left) / static_cast<LONG>(std::max<std::size_t>(1U, graph_history_.SeriesCount())));
            for (std::size_t index{}; index < graph_history_.SeriesCount(); ++index) {
                const auto& series = graph_history_.Series(index);
                if (!series.available) continue;
                wchar_t value[64]{};
                static_cast<void>(swprintf_s(value, L"%.1f %s", series.current, GraphUnitSuffix(series.unit)));
                const auto left = plot.left + static_cast<LONG>(index) * value_width;
                draw_label(value, RECT{left, graph_y, left + value_width - 2, graph_y + legend_height}, settings_.osd_graph_colors_rgb[index], DT_LEFT);
            }
        }
    }

    if (settings_.osd_background) {
        auto* pixels = static_cast<std::uint32_t*>(pixels_);
        const auto background_alpha = static_cast<std::uint8_t>((145U * settings_.osd_opacity_percent) / 100U);
        for (std::size_t index = 0; index < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++index) {
            if ((pixels[index] >> 24U) == 0U) pixels[index] = static_cast<std::uint32_t>(background_alpha) << 24U;
        }
    }

    Position(width, height);
    POINT source{};
    SIZE size{width, height};
    POINT destination{};
    RECT bounds{};
    GetWindowRect(window_, &bounds);
    destination.x = bounds.left;
    destination.y = bounds.top;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    static_cast<void>(UpdateLayeredWindow(window_, nullptr, &destination, &size, memory_dc_, &source, 0, &blend, ULW_ALPHA));
    ShowWindow(window_, SW_SHOWNOACTIVATE);
}

void OsdWindow::Position(const int width, const int height) noexcept {
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    const auto monitor = MonitorFromWindow(monitor_anchor_, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(monitor, &information)) return;
    const auto margin = ScaleForDpi(12);
    int x = information.rcWork.left + margin;
    int y = information.rcWork.top + margin;
    const auto position = role_ == OsdWindowRole::fps ? settings_.fps_osd_position : settings_.osd_position;
    if (position == OsdPosition::top_right || position == OsdPosition::bottom_right) x = information.rcWork.right - width - margin;
    if (position == OsdPosition::bottom_left || position == OsdPosition::bottom_right) y = information.rcWork.bottom - height - margin;
    static_cast<void>(SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER));
}

} // namespace hardwarescope
