#pragma once

#include "hardwarescope/app_settings.hpp"
#include "hardwarescope/graph_window.hpp"
#include "hardwarescope/osd_window.hpp"
#include "hardwarescope/sensor_snapshot.hpp"
#include "hardwarescope/sensor_explanations.hpp"
#include "hardwarescope/sensor_view_model.hpp"
#include "hardwarescope/sensor_worker.hpp"
#include "hardwarescope/tray_panel_window.hpp"
#include "hardwarescope/ui_palette.hpp"
#include "hardwarescope/update_coordinator.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace hardwarescope {

class NativeWindow final {
public:
    explicit NativeWindow(HINSTANCE instance) noexcept;
    ~NativeWindow();

    NativeWindow(const NativeWindow&) = delete;
    NativeWindow& operator=(const NativeWindow&) = delete;

    int Run(int show_command);
    [[nodiscard]] HWND Handle() const noexcept { return window_; }

    static constexpr wchar_t kWindowClass[] = L"HardwareScope.Native.MainWindow";

private:
    static constexpr UINT kSnapshotMessage = WM_APP + 17U;
    static constexpr UINT kTrayMessage = WM_APP + 18U;
    static constexpr UINT_PTR kStartupUpdateTimer = 2U;
    static constexpr UINT_PTR kTooltipTimer = 3U;
    static constexpr int kHeaderHeight = 70;
    static constexpr int kWindowButtonWidth = 40;
    static constexpr int kWindowButtonCount = 4;

    static LRESULT CALLBACK StaticWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    LRESULT WindowProcedure(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    static void SnapshotPublished(void* context, std::uint64_t sequence) noexcept;
    [[nodiscard]] std::uint64_t LatestSnapshotSequence() const noexcept;
    void HandleSnapshotMessage() noexcept;

    bool RegisterWindowClass();
    bool CreateNativeWindow(int show_command);
    void RefreshWindowIcons() noexcept;
    bool CreateDeviceResources();
    void DiscardDeviceResources() noexcept;
    void ClearTextLayoutCache() noexcept;
    void Render();
    void DrawHeader(const D2D1_SIZE_F& size);
    void DrawSearch(const D2D1_SIZE_F& size);
    void DrawSensorTable(const D2D1_SIZE_F& size, const SensorSnapshot& snapshot);
    void DrawTooltip(const D2D1_SIZE_F& size, const SensorSnapshot& snapshot);
    void DrawTextLine(const wchar_t* text, const D2D1_RECT_F& rectangle, ID2D1Brush* brush, IDWriteTextFormat* format);
    void ResizeRenderTarget(UINT width, UINT height);
    LRESULT HitTest(POINT screen_point) const noexcept;
    void HandleWindowButton(POINT client_point);
    void HandleContentClick(POINT client_point);
    void HandleCharacter(wchar_t character);
    void UpdateHover(POINT client_point);
    void ClearHover() noexcept;
    void ApplyDwmAppearance() noexcept;
    bool AddTrayIcon() noexcept;
    void RemoveTrayIcon() noexcept;
    void MinimizeToTray() noexcept;
    void RestoreFromTray() noexcept;
    void ShowTrayMenu() noexcept;
    void HandleCommand(int command) noexcept;
    void ShowSettings() noexcept;
    void ShowFirstRunSetup() noexcept;
    void ScheduleAutomaticUpdateCheck(std::uint32_t default_delay_ms = 5'000U) noexcept;
    bool ShowUpdateNotification(const UpdateCompletion& completion) noexcept;
    void PromptForUpdate(const UpdateCompletion& completion) noexcept;
    void PromptForPendingUpdate() noexcept;
    void HandleUpdateCompletion(const UpdateCompletion& completion) noexcept;
    [[nodiscard]] std::uint64_t PaintP95Microseconds() const noexcept;

    HINSTANCE instance_{};
    HWND window_{};
    UINT dpi_{96U};
    float scroll_offset_{};
    std::uint32_t collapsed_sections_{};
    std::wstring search_text_{};
    bool search_active_{};
    enum class HoverKind : std::uint8_t { none, column, sensor };
    HoverKind hover_kind_{HoverKind::none};
    TableColumn hover_column_{TableColumn::sensor};
    std::uint64_t hover_sensor_id_{};
    D2D1_POINT_2F hover_point_{};
    bool tooltip_visible_{};
    bool tracking_mouse_leave_{};
    int hover_window_button_{-1};
    std::atomic<bool> in_size_move_{};
    std::uint64_t last_paint_microseconds_{};
    std::array<std::uint64_t, 128U> paint_samples_{};
    std::size_t paint_sample_count_{};
    std::size_t paint_sample_index_{};
    UINT taskbar_created_message_{};
    bool tray_icon_added_{};
    bool suspended_{};
    bool first_run_{};
    bool resume_waiting_for_snapshot_{};
    std::optional<UpdateCompletion> pending_update_{};
    SnapshotStore snapshots_{};
    SensorSnapshot ui_snapshot_{};
    SensorWorker sensor_worker_;
    AppSettings settings_{};
    UiPalette palette_{};
    SettingsStore settings_store_;
    AsyncSettingsWriter settings_writer_;
    void SaveSettings() noexcept;
    void InstallVerifiedUpdate(const UpdateCompletion& completion) noexcept;
    OsdWindow osd_window_;
    OsdWindow fps_osd_window_;
    GraphWindow graph_window_;
    TrayPanelWindow tray_panel_;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_{};
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_{};
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_{};
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target_{};
    Microsoft::WRL::ComPtr<ID2D1Bitmap> header_logo_bitmap_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> muted_brush_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent_brush_{};
    std::array<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>, static_cast<std::size_t>(SensorSection::count)> section_brushes_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> line_brush_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> header_brush_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surface_brush_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surface_alternate_brush_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> close_hover_brush_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> close_icon_brush_{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_format_{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> subtitle_format_{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> body_format_{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> value_format_{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tooltip_format_{};

    struct SensorTextLayouts final {
        std::uint64_t id{};
        std::array<wchar_t, 48U> current{};
        std::array<wchar_t, 48U> minimum{};
        std::array<wchar_t, 48U> maximum{};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> name_layout{};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> current_layout{};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> minimum_layout{};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> maximum_layout{};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> hardware_layout{};
    };
    std::array<SensorTextLayouts, kMaxSensors> sensor_text_layouts_{};
    float sensor_layout_table_width_{};
};

} // namespace hardwarescope
