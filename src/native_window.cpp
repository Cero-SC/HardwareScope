#include "hardwarescope/native_window.hpp"

#include "hardwarescope/app_commands.hpp"
#include "hardwarescope/legacy_settings_migration.hpp"
#include "hardwarescope/osd_model.hpp"
#include "hardwarescope/settings_window.hpp"
#include "hardwarescope/startup_registration.hpp"
#include "hardwarescope/update_prompt_window.hpp"
#include "hardwarescope/window_regions.hpp"

#include <d2d1helper.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <string>

namespace hardwarescope {
namespace {

constexpr float kContentInset = 12.0F;
constexpr UINT_PTR kSettingsWriteTimer = 0x48535057U;

bool IsIsolatedTest() noexcept {
#if HARDWARESCOPE_INTERNAL_TEST_HOOKS
    return GetEnvironmentVariableW(L"HARDWARESCOPE_TEST_SETTINGS", nullptr, 0U) > 1U;
#else
    return false;
#endif
}

std::filesystem::path WindowSettingsPath() {
#if HARDWARESCOPE_INTERNAL_TEST_HOOKS
    std::array<wchar_t, 32'768U> path{};
    const auto length = GetEnvironmentVariableW(L"HARDWARESCOPE_TEST_SETTINGS", path.data(), static_cast<DWORD>(path.size()));
    if (length > 0U && length < path.size()) return std::filesystem::path{path.data()};
#endif
    return SettingsStore::DefaultPath();
}
constexpr float kSearchTop = 78.0F;
constexpr float kSearchHeight = 32.0F;
constexpr float kFavoritesButtonWidth = 126.0F;
constexpr float kToolbarGap = 10.0F;
constexpr float kFavoriteSlotWidth = 25.0F;
constexpr float kTableTop = 118.0F;
constexpr float kColumnHeaderHeight = 29.0F;
constexpr float kSensorRowHeight = 31.0F;
#if HARDWARESCOPE_INTERNAL_TEST_HOOKS
const GUID kTrayIconGuid{0xC937D2E1, 0x3FC1, 0x4F21, {0xA9, 0x77, 0x48, 0x46, 0x26, 0x19, 0x7D, 0x91}};
#else
const GUID kTrayIconGuid{0xE58BB907, 0x9AEF, 0x4E50, {0x9D, 0x2F, 0x5A, 0x65, 0xB4, 0xB4, 0x2D, 0x40}};
#endif

D2D1_RECT_F FavoritesButtonBounds(const D2D1_SIZE_F size) noexcept {
    const auto right = std::max(kContentInset + kFavoritesButtonWidth, size.width - kContentInset);
    return D2D1::RectF(right - kFavoritesButtonWidth, kSearchTop, right, kSearchTop + kSearchHeight);
}

float SearchRightEdge(const D2D1_SIZE_F size) noexcept {
    const auto favorites = FavoritesButtonBounds(size);
    return std::max(kContentInset + 80.0F, std::min(500.0F, favorites.left - kToolbarGap));
}

HICON LoadHardwareScopeIcon(
    const HINSTANCE instance,
    const UINT dpi,
    const int width_metric,
    const int height_metric) noexcept {
    const auto effective_dpi = dpi == 0U ? 96U : dpi;
    const auto width = std::max(1, GetSystemMetricsForDpi(width_metric, effective_dpi));
    const auto height = std::max(1, GetSystemMetricsForDpi(height_metric, effective_dpi));
    return static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(101),
        IMAGE_ICON,
        width,
        height,
        LR_DEFAULTCOLOR | LR_SHARED));
}

using TableColumns = std::array<float, 7>;

TableColumns CalculateTableColumns(const float left, const float table_width) noexcept {
    // These proportions preserve useful sensor and hardware widths in the compact
    // layout while keeping all six columns visible at the minimum window width.
    return TableColumns{
        left + 6.0F,
        left + 46.0F,
        left + table_width * 0.34F,
        left + table_width * 0.49F,
        left + table_width * 0.61F,
        left + table_width * 0.73F,
        left + table_width - 8.0F};
}

std::uint64_t CurrentUnixSeconds() noexcept {
    const auto count = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return count > 0 ? static_cast<std::uint64_t>(count) : 0U;
}

bool SameVersion(const SemanticVersion& left, const SemanticVersion& right) noexcept {
    return left.major == right.major && left.minor == right.minor && left.patch == right.patch;
}

std::optional<SemanticVersion> SkippedUpdateVersion(const AppSettings& settings) noexcept {
    if (settings.skipped_update_major == 0U
        && settings.skipped_update_minor == 0U
        && settings.skipped_update_patch == 0U) return std::nullopt;
    return SemanticVersion{
        settings.skipped_update_major,
        settings.skipped_update_minor,
        settings.skipped_update_patch};
}

const wchar_t* UnitSuffix(const SensorUnit unit) noexcept {
    switch (unit) {
    case SensorUnit::celsius: return L" °C";
    case SensorUnit::percent: return L" %";
    case SensorUnit::megahertz: return L" MHz";
    case SensorUnit::revolutions_per_minute: return L" RPM";
    case SensorUnit::watts: return L" W";
    case SensorUnit::volts: return L" V";
    case SensorUnit::megabytes: return L" MB";
    case SensorUnit::frames_per_second: return L" FPS";
    case SensorUnit::milliseconds: return L" ms";
    }
    return L"";
}

void FormatValue(const SensorValue& sensor, const double value, wchar_t* destination, const std::size_t destination_size) noexcept {
    if (!sensor.available) {
        static_cast<void>(wcscpy_s(destination, destination_size, L"—"));
        return;
    }
    const auto decimals = sensor.unit == SensorUnit::volts ? 3 : sensor.unit == SensorUnit::megahertz || sensor.unit == SensorUnit::revolutions_per_minute ? 0 : 1;
    if (decimals == 3) static_cast<void>(swprintf_s(destination, destination_size, L"%.3f%s", value, UnitSuffix(sensor.unit)));
    else if (decimals == 0) static_cast<void>(swprintf_s(destination, destination_size, L"%.0f%s", value, UnitSuffix(sensor.unit)));
    else static_cast<void>(swprintf_s(destination, destination_size, L"%.1f%s", value, UnitSuffix(sensor.unit)));
}

D2D1_COLOR_F Color(const std::uint32_t rgb) noexcept {
    return D2D1::ColorF(rgb & 0xFFFFFFU);
}

} // namespace

NativeWindow::NativeWindow(const HINSTANCE instance) noexcept
    : instance_(instance),
      sensor_worker_(snapshots_, &NativeWindow::SnapshotPublished, this,
          IsIsolatedTest() ? SensorWorkerMode::synthetic : SensorWorkerMode::native, instance),
      settings_store_(WindowSettingsPath()),
      settings_writer_(WindowSettingsPath()),
      osd_window_(instance, OsdWindowRole::primary),
      fps_osd_window_(instance, OsdWindowRole::fps),
      graph_window_(instance),
      tray_panel_(instance) {
    const auto settings_loaded = settings_store_.Load(settings_);
    if (!settings_loaded) {
        auto legacy_path = settings_store_.Path().parent_path();
        legacy_path /= L"settings.json";
        if (MigrateLegacySettingsFile(legacy_path, settings_)) {
            settings_.onboarding_completed = true;
            SaveSettings();
        }
    }
    first_run_ = !settings_.onboarding_completed;
#if HARDWARESCOPE_INTERNAL_TEST_HOOKS
    settings_.start_minimized = false;
    first_run_ = false;
    if (IsIsolatedTest()) {
        settings_.automatic_updates = false;
        settings_.start_with_windows = false;
    }
#endif
    collapsed_sections_ = settings_.collapsed_sections;
    palette_ = PaletteFor(settings_.theme, settings_.text_color_rgb, settings_.high_contrast);
    sensor_worker_.ConfigureFps(
        settings_.fps_enabled,
        settings_.fps_game_only,
        settings_.fps_refresh_interval_ms,
        settings_.fps_smoothing_interval_ms);
    sensor_worker_.ConfigureMinMaxReset(
        settings_.reset_min_max_on_game_launch,
        settings_.reset_min_max_interval_minutes);
}

void NativeWindow::SaveSettings() noexcept {
    settings_writer_.Request(settings_);
    if (window_ != nullptr) SetTimer(window_, kSettingsWriteTimer, 1'000U, nullptr);
}

NativeWindow::~NativeWindow() {
    sensor_worker_.Stop();
    DiscardDeviceResources();
}

int NativeWindow::Run(const int show_command) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.ReleaseAndGetAddressOf()))) return 10;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_factory_.ReleaseAndGetAddressOf())))) return 11;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(wic_factory_.ReleaseAndGetAddressOf())))) return 12;
    if (!RegisterWindowClass()) return 12;
    if (!CreateNativeWindow(show_command)) return 13;

    if (settings_.reset_min_max_on_startup) sensor_worker_.RequestMinMaxReset();
    sensor_worker_.Start(std::chrono::milliseconds{settings_.refresh_interval_ms});
    if (first_run_) ShowFirstRunSetup();
    MSG message{};
    int result = 0;
    for (;;) {
        const auto status = GetMessageW(&message, nullptr, 0, 0);
        if (status == 0) break;
        if (status == -1) {
            result = 14;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    sensor_worker_.Stop();
    return result;
}

bool NativeWindow::RegisterWindowClass() {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    window_class.lpfnWndProc = &NativeWindow::StaticWindowProcedure;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    const auto system_dpi = GetDpiForSystem();
    window_class.hIcon = LoadHardwareScopeIcon(instance_, system_dpi, SM_CXICON, SM_CYICON);
    window_class.hIconSm = LoadHardwareScopeIcon(instance_, system_dpi, SM_CXSMICON, SM_CYSMICON);
    window_class.lpszClassName = kWindowClass;
    return RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool NativeWindow::CreateNativeWindow(const int show_command) {
    const auto reported_dpi = GetDpiForSystem();
    const auto initial_dpi = reported_dpi == 0U ? 96U : reported_dpi;
    const auto desired_width = MulDiv(760, static_cast<int>(initial_dpi), 96);
    const auto desired_height = MulDiv(820, static_cast<int>(initial_dpi), 96);
    RECT work_area{};
    static_cast<void>(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0));
    const auto width = std::min(desired_width, static_cast<int>(work_area.right - work_area.left));
    const auto height = std::min(desired_height, static_cast<int>(work_area.bottom - work_area.top));
    const auto x = work_area.left + std::max(0L, (work_area.right - work_area.left - width) / 2L);
    const auto y = work_area.top + std::max(0L, (work_area.bottom - work_area.top - height) / 2L);

    window_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        kWindowClass,
        L"HardwareScope",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr) return false;

    dpi_ = GetDpiForWindow(window_);
    RefreshWindowIcons();
    ApplyDwmAppearance();
    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
    static_cast<void>(AddTrayIcon());
    if (!osd_window_.Initialize(window_, settings_)) return false;
    if (!fps_osd_window_.Initialize(window_, settings_)) return false;
    if (!graph_window_.Initialize(window_, settings_)) return false;
    if (!tray_panel_.Initialize(window_, settings_)) return false;
    if (!first_run_) ScheduleAutomaticUpdateCheck();
    if (settings_.start_minimized) {
        ShowWindow(window_, SW_HIDE);
    } else {
        ShowWindow(window_, show_command == 0 ? SW_SHOWNORMAL : show_command);
        UpdateWindow(window_);
    }
    return true;
}

void NativeWindow::RefreshWindowIcons() noexcept {
    if (window_ == nullptr) return;
    const auto large_icon = LoadHardwareScopeIcon(instance_, dpi_, SM_CXICON, SM_CYICON);
    const auto small_icon = LoadHardwareScopeIcon(instance_, dpi_, SM_CXSMICON, SM_CYSMICON);
    if (large_icon != nullptr) static_cast<void>(SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon)));
    if (small_icon != nullptr) static_cast<void>(SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon)));

    if (tray_icon_added_ && small_icon != nullptr) {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window_;
        icon.uID = 1U;
        icon.uFlags = NIF_ICON | NIF_GUID;
        icon.hIcon = small_icon;
        icon.guidItem = kTrayIconGuid;
        static_cast<void>(Shell_NotifyIconW(NIM_MODIFY, &icon));
    }
}

LRESULT CALLBACK NativeWindow::StaticWindowProcedure(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) noexcept {
    NativeWindow* instance = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        instance = static_cast<NativeWindow*>(create->lpCreateParams);
        instance->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
    } else {
        instance = reinterpret_cast<NativeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return instance != nullptr ? instance->WindowProcedure(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT NativeWindow::WindowProcedure(const UINT message, const WPARAM wparam, const LPARAM lparam) noexcept {
    if (taskbar_created_message_ != 0U && message == taskbar_created_message_) {
        tray_icon_added_ = false;
        static_cast<void>(AddTrayIcon());
        return 0;
    }
    switch (message) {
    case WM_NCCALCSIZE:
        if (wparam != 0U) return 0;
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        return HitTest(point);
    }
    case WM_GETMINMAXINFO: {
        auto* information = reinterpret_cast<MINMAXINFO*>(lparam);
        MONITORINFO monitor_information{};
        monitor_information.cbSize = sizeof(monitor_information);
        const auto monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        static_cast<void>(GetMonitorInfoW(monitor, &monitor_information));
        const auto available_width = std::max(1L, monitor_information.rcWork.right - monitor_information.rcWork.left);
        const auto available_height = std::max(1L, monitor_information.rcWork.bottom - monitor_information.rcWork.top);
        information->ptMinTrackSize = POINT{
            std::min(MulDiv(660, static_cast<int>(dpi_), 96), static_cast<int>(available_width)),
            std::min(MulDiv(480, static_cast<int>(dpi_), 96), static_cast<int>(available_height))};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOACTIVATE | SWP_NOZORDER);
        RefreshWindowIcons();
        DiscardDeviceResources();
        osd_window_.DisplayChanged();
        fps_osd_window_.DisplayChanged();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_SIZE:
        ResizeRenderTarget(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_DISPLAYCHANGE:
        osd_window_.DisplayChanged();
        fps_osd_window_.DisplayChanged();
        graph_window_.DisplayChanged();
        tray_panel_.DisplayChanged();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND) {
            if (!suspended_) {
                suspended_ = true;
                resume_waiting_for_snapshot_ = false;
                sensor_worker_.SetSuspended(true);
                MSG pending{};
                while (PeekMessageW(&pending, window_, kSnapshotMessage, kSnapshotMessage, PM_REMOVE)) {}
                osd_window_.SetVisible(false);
                fps_osd_window_.SetVisible(false);
                tray_panel_.Hide();
            }
            return TRUE;
        }
        if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND || wparam == PBT_APMRESUMECRITICAL) {
            if (suspended_ || !sensor_worker_.Running()) {
                suspended_ = false;
                MSG pending{};
                while (PeekMessageW(&pending, window_, kSnapshotMessage, kSnapshotMessage, PM_REMOVE)) {}
                resume_waiting_for_snapshot_ = true;
                osd_window_.SetVisible(false);
                fps_osd_window_.SetVisible(false);
                sensor_worker_.SetSuspended(false);
                sensor_worker_.Start(std::chrono::milliseconds{settings_.refresh_interval_ms});
            }
            return TRUE;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0U) == SC_MINIMIZE) {
            MinimizeToTray();
            return 0;
        }
        break;
    case WM_ENTERSIZEMOVE:
        in_size_move_.store(true, std::memory_order_release);
        return 0;
    case WM_EXITSIZEMOVE:
        in_size_move_.store(false, std::memory_order_release);
        ClearTextLayoutCache();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        ClearHover();
        const auto delta = GET_WHEEL_DELTA_WPARAM(wparam);
        scroll_offset_ = std::max(0.0F, scroll_offset_ - static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA) * kSensorRowHeight * 3.0F);
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        ClearHover();
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        HandleWindowButton(point);
        HandleContentClick(point);
        return 0;
    }
    case WM_CHAR:
        HandleCharacter(static_cast<wchar_t>(wparam));
        return 0;
    case WM_KEYDOWN:
#if HARDWARESCOPE_INTERNAL_TEST_HOOKS
        if (wparam == VK_F8) {
            RECT work{};
            static_cast<void>(SystemParametersInfoW(SPI_GETWORKAREA, 0U, &work, 0U));
            snapshots_.ReadLatest(ui_snapshot_);
            tray_panel_.Toggle(POINT{work.right - 8, work.bottom - 8}, ui_snapshot_, settings_);
            return 0;
        }
        if (wparam == VK_F9) {
            UpdateManifest manifest{};
            manifest.version = SemanticVersion{9U, 9U, 9U};
            static_cast<void>(ShowUpdatePromptWindow(window_, instance_, manifest, settings_));
            return 0;
        }
#endif
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wparam == 'F') {
            search_active_ = true;
            SetFocus(window_);
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wparam == VK_OEM_COMMA) {
            ShowSettings();
            return 0;
        }
        if (wparam == VK_ESCAPE && (search_active_ || !search_text_.empty() || tooltip_visible_)) {
            search_active_ = false;
            search_text_.clear();
            ClearHover();
            scroll_offset_ = 0.0F;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        UpdateHover(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
    case WM_MOUSELEAVE:
        tracking_mouse_leave_ = false;
        ClearHover();
        return 0;
    case WM_COMMAND:
        HandleCommand(LOWORD(wparam));
        return 0;
    case WM_TIMER:
        if (wparam == kSettingsWriteTimer) {
            if (!settings_writer_.Pending()) KillTimer(window_, kSettingsWriteTimer);
            if (settings_writer_.TakeFailure()) {
                ShowUpdateNoticeWindow(window_, instance_, settings_, L"Settings could not be saved",
                    L"Your changes are active in this session, but could not be saved to disk. Check free space and folder permissions, then save again.", true);
            }
            return 0;
        }
        if (wparam == kStartupUpdateTimer) {
            KillTimer(window_, kStartupUpdateTimer);
            if (!BeginNativeUpdateCheck(window_, true, SkippedUpdateVersion(settings_))) {
                ScheduleAutomaticUpdateCheck(60'000U);
            }
            return 0;
        }
        if (wparam == kTooltipTimer) {
            KillTimer(window_, kTooltipTimer);
            if (hover_kind_ != HoverKind::none) {
                tooltip_visible_ = true;
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case kManualUpdateRequestMessage:
        if (!BeginNativeUpdateCheck(window_, false)) {
            ShowUpdateNoticeWindow(
                GetLastActivePopup(window_), instance_, settings_,
                L"Update check already running",
                L"HardwareScope is already checking GitHub for a verified stable release.");
        }
        return 0;
    case kUpdateCompletedMessage:
        if (const auto completion = TakeNativeUpdateCompletion()) HandleUpdateCompletion(*completion);
        return 0;
#if HARDWARESCOPE_INTERNAL_TEST_HOOKS
    case kQueryTooltipVisibleMessage:
        return tooltip_visible_ ? 1 : 0;
    case kArmTooltipTestMessage:
        ClearHover();
        hover_kind_ = HoverKind::column;
        hover_column_ = TableColumn::current;
        hover_point_ = D2D1::Point2F(700.0F, 145.0F);
        static_cast<void>(SetTimer(window_, kTooltipTimer, 1'000U, nullptr));
        return 1;
    case kQueryPaintP95Message:
        return static_cast<LRESULT>(PaintP95Microseconds());
    case kDisableAutomaticUpdateTestMessage:
        KillTimer(window_, kStartupUpdateTimer);
        return 1;
    case kQuerySensorWorkerRunningMessage:
        return sensor_worker_.Running() ? 1 : 0;
    case kQueryResumeWaitingMessage:
        return resume_waiting_for_snapshot_ ? 1 : 0;
    case kQuerySnapshotSequenceMessage:
        return static_cast<LRESULT>(LatestSnapshotSequence());
    case kRestoreTrayIconTestMessage:
        RemoveTrayIcon();
        return AddTrayIcon() ? 1 : 0;
    case kQueryTrayIconAddedMessage:
        return tray_icon_added_ ? 1 : 0;
    case kToggleTrayPanelTestMessage: {
        RECT work{};
        static_cast<void>(SystemParametersInfoW(SPI_GETWORKAREA, 0U, &work, 0U));
        snapshots_.ReadLatest(ui_snapshot_);
        tray_panel_.Toggle(POINT{work.right - 8, work.bottom - 8}, ui_snapshot_, settings_);
        return tray_panel_.Visible() ? 1 : 0;
    }
    case kQueryTrayPanelVisibleMessage:
        return tray_panel_.Visible() ? 1 : 0;
    case kApplyMainDpiTestMessage: {
        const auto target_dpi = std::clamp(static_cast<UINT>(wparam), 96U, 384U);
        RECT target{};
        GetWindowRect(window_, &target);
        static_cast<void>(SendMessageW(
            window_,
            WM_DPICHANGED,
            MAKEWPARAM(target_dpi, target_dpi),
            reinterpret_cast<LPARAM>(&target)));
        return static_cast<LRESULT>(dpi_);
    }
    case kShowUpdatePromptTestMessage: {
        UpdateManifest manifest{};
        manifest.version = SemanticVersion{9U, 9U, 9U};
        static_cast<void>(ShowUpdatePromptWindow(window_, instance_, manifest, settings_));
        return 1;
    }
    case kQueueAutomaticUpdateNotificationTestMessage: {
        UpdateCompletion completion{};
        completion.status = UpdateCompletionStatus::available;
        completion.automatic = true;
        completion.manifest.version = SemanticVersion{9U, 9U, 9U};
        pending_update_ = completion;
        return ShowUpdateNotification(completion) ? 1 : 0;
    }
    case kConfigureOsdGraphTestMessage: {
        settings_.osd_graph_enabled = wparam != 0U;
        if (settings_.osd_graph_enabled) {
            const auto end = ui_snapshot_.sensors.begin() + ui_snapshot_.count;
            const auto source = std::find_if(ui_snapshot_.sensors.begin(), end, [](const SensorValue& sensor) {
                return sensor.available && sensor.kind != SensorKind::frame_rate;
            });
            if (source == end) return 0;
            settings_.osd_graph_sensor_id = source->id;
            settings_.osd_graph_sensor_ids = {source->id, 0U, 0U, 0U};
            settings_.osd_graph_sensor_count = 1U;
            settings_.osd_graph_history_seconds = 5U;
            settings_.osd_graph_refresh_interval_ms = 100U;
            settings_.osd_graph_width_px = 240U;
            settings_.osd_graph_height_px = 64U;
        }
        osd_window_.ApplySettings(settings_);
        fps_osd_window_.ApplySettings(settings_);
        osd_window_.Update(ui_snapshot_);
        fps_osd_window_.Update(ui_snapshot_);
        return 1;
    }
    case kConfigureFloatingGraphTestMessage: {
        settings_.floating_graph_enabled = wparam != 0U;
        if (settings_.floating_graph_enabled) {
            const auto end = ui_snapshot_.sensors.begin() + ui_snapshot_.count;
            const auto source = std::find_if(ui_snapshot_.sensors.begin(), end, [](const SensorValue& sensor) {
                return sensor.available && sensor.kind != SensorKind::frame_rate;
            });
            if (source == end) return 0;
            settings_.osd_graph_sensor_id = source->id;
            settings_.osd_graph_sensor_ids = {source->id, 0U, 0U, 0U};
            settings_.osd_graph_sensor_count = 1U;
            settings_.osd_graph_history_seconds = 5U;
            settings_.osd_graph_refresh_interval_ms = 100U;
            settings_.floating_graph_width_px = 560U;
            settings_.floating_graph_height_px = 300U;
        }
        graph_window_.ApplySettings(settings_);
        graph_window_.Update(ui_snapshot_);
        return 1;
    }
#endif
    case kGraphWindowClosedMessage:
        settings_.floating_graph_enabled = false;
        SaveSettings();
        return 0;
    case kGraphWindowPlacementChangedMessage:
        graph_window_.CapturePlacement(settings_);
        SaveSettings();
        return 0;
    case kTrayMessage: {
        const auto tray_event = static_cast<UINT>(LOWORD(lparam));
        if (tray_event == NIN_BALLOONUSERCLICK) PromptForPendingUpdate();
        else if (tray_event == WM_LBUTTONDBLCLK) RestoreFromTray();
        else if (tray_event == WM_LBUTTONUP || tray_event == NIN_SELECT || tray_event == NIN_KEYSELECT) {
            POINT cursor{};
            static_cast<void>(GetCursorPos(&cursor));
            snapshots_.ReadLatest(ui_snapshot_);
            tray_panel_.Toggle(cursor, ui_snapshot_, settings_);
        }
        else if (tray_event == WM_RBUTTONUP || tray_event == WM_CONTEXTMENU) ShowTrayMenu();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window_, &paint);
        Render();
        EndPaint(window_, &paint);
        return 0;
    }
    case kSnapshotMessage:
        HandleSnapshotMessage();
        return 0;
    case WM_CLOSE:
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        sensor_worker_.RequestStop();
        graph_window_.CapturePlacement(settings_);
        SaveSettings();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void NativeWindow::SnapshotPublished(void* const context, const std::uint64_t sequence) noexcept {
    auto* instance = static_cast<NativeWindow*>(context);
    if (instance != nullptr && instance->window_ != nullptr) {
        static_cast<void>(PostMessageW(instance->window_, kSnapshotMessage, static_cast<WPARAM>(sequence), 0));
    }
}

std::uint64_t NativeWindow::LatestSnapshotSequence() const noexcept {
    return snapshots_.LatestSequence();
}

void NativeWindow::HandleSnapshotMessage() noexcept {
    if (suspended_) return;
    snapshots_.ReadLatest(ui_snapshot_);
    if (InitializeDefaultFavorites(ui_snapshot_, settings_)) SaveSettings();
    osd_window_.Update(ui_snapshot_);
    fps_osd_window_.Update(ui_snapshot_);
    graph_window_.Update(ui_snapshot_);
    tray_panel_.Update(ui_snapshot_, settings_);
    if (resume_waiting_for_snapshot_) {
        resume_waiting_for_snapshot_ = false;
        osd_window_.SetVisible(settings_.show_osd);
        fps_osd_window_.SetVisible(settings_.show_osd);
    }
    if (!in_size_move_.load(std::memory_order_acquire)) InvalidateRect(window_, nullptr, FALSE);
}

bool NativeWindow::CreateDeviceResources() {
    if (render_target_ != nullptr) return true;

    RECT bounds{};
    GetClientRect(window_, &bounds);
    const auto size = D2D1::SizeU(
        static_cast<UINT>(std::max(1L, bounds.right - bounds.left)),
        static_cast<UINT>(std::max(1L, bounds.bottom - bounds.top)));
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE),
        static_cast<float>(dpi_),
        static_cast<float>(dpi_));
    const auto hwnd_properties = D2D1::HwndRenderTargetProperties(
        window_,
        size,
        D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    if (FAILED(d2d_factory_->CreateHwndRenderTarget(properties, hwnd_properties, render_target_.ReleaseAndGetAddressOf()))) return false;
    render_target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    if (FAILED(render_target_->CreateSolidColorBrush(Color(palette_.text), text_brush_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(render_target_->CreateSolidColorBrush(Color(palette_.muted), muted_brush_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(render_target_->CreateSolidColorBrush(Color(palette_.accent), accent_brush_.ReleaseAndGetAddressOf()))) return false;
    for (std::size_t index = 0U; index < section_brushes_.size(); ++index) {
        if (FAILED(render_target_->CreateSolidColorBrush(
                Color(SensorSectionColor(static_cast<SensorSection>(index), settings_)),
                section_brushes_[index].ReleaseAndGetAddressOf()))) return false;
    }
    if (FAILED(render_target_->CreateSolidColorBrush(Color(palette_.line), line_brush_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(render_target_->CreateSolidColorBrush(Color(palette_.header), header_brush_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(render_target_->CreateSolidColorBrush(Color(palette_.surface), surface_brush_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(render_target_->CreateSolidColorBrush(Color(palette_.surface_alternate), surface_alternate_brush_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(render_target_->CreateSolidColorBrush(Color(0xE81123U), close_hover_brush_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(render_target_->CreateSolidColorBrush(Color(0xFFFFFFU), close_icon_brush_.ReleaseAndGetAddressOf()))) return false;

    const auto icon_size = MulDiv(48, static_cast<int>(dpi_), 96);
    const auto icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON, icon_size, icon_size, LR_DEFAULTCOLOR));
    if (icon != nullptr) {
        Microsoft::WRL::ComPtr<IWICBitmap> wic_bitmap;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(wic_factory_->CreateBitmapFromHICON(icon, wic_bitmap.ReleaseAndGetAddressOf()))
            && SUCCEEDED(wic_factory_->CreateFormatConverter(converter.ReleaseAndGetAddressOf()))
            && SUCCEEDED(converter->Initialize(
                wic_bitmap.Get(),
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom))) {
            static_cast<void>(render_target_->CreateBitmapFromWicBitmap(converter.Get(), header_logo_bitmap_.ReleaseAndGetAddressOf()));
        }
        DestroyIcon(icon);
    }

    const auto text_scale = static_cast<float>(settings_.interface_text_scale_percent) / 100.0F;
    if (FAILED(dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 22.0F * text_scale, L"en-US", title_format_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0F * text_scale, L"en-US", subtitle_format_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0F * text_scale, L"en-US", body_format_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0F * text_scale, L"en-US", value_format_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5F * text_scale, L"en-US", tooltip_format_.ReleaseAndGetAddressOf()))) return false;

    for (const auto format : {subtitle_format_.Get(), body_format_.Get(), value_format_.Get()}) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    title_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    title_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tooltip_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    tooltip_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    tooltip_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return true;
}

void NativeWindow::DiscardDeviceResources() noexcept {
    ClearTextLayoutCache();
    tooltip_format_.Reset();
    value_format_.Reset();
    body_format_.Reset();
    subtitle_format_.Reset();
    title_format_.Reset();
    header_logo_bitmap_.Reset();
    close_icon_brush_.Reset();
    close_hover_brush_.Reset();
    surface_alternate_brush_.Reset();
    surface_brush_.Reset();
    header_brush_.Reset();
    line_brush_.Reset();
    for (auto& brush : section_brushes_) brush.Reset();
    accent_brush_.Reset();
    muted_brush_.Reset();
    text_brush_.Reset();
    render_target_.Reset();
}

void NativeWindow::ClearTextLayoutCache() noexcept {
    for (auto& layouts : sensor_text_layouts_) layouts = SensorTextLayouts{};
    sensor_layout_table_width_ = 0.0F;
}

void NativeWindow::Render() {
    if (!CreateDeviceResources()) return;
    LARGE_INTEGER frequency{};
    LARGE_INTEGER started{};
    LARGE_INTEGER finished{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);

    snapshots_.ReadLatest(ui_snapshot_);
    const auto size = render_target_->GetSize();
    render_target_->BeginDraw();
    render_target_->Clear(Color(palette_.background));
    DrawHeader(size);
    DrawSearch(size);
    DrawSensorTable(size, ui_snapshot_);
    DrawTooltip(size, ui_snapshot_);
    const auto result = render_target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) DiscardDeviceResources();

    QueryPerformanceCounter(&finished);
    if (frequency.QuadPart > 0) {
        const auto ticks = static_cast<std::uint64_t>(finished.QuadPart - started.QuadPart);
        last_paint_microseconds_ = (ticks * 1'000'000ULL) / static_cast<std::uint64_t>(frequency.QuadPart);
        paint_samples_[paint_sample_index_] = last_paint_microseconds_;
        paint_sample_index_ = (paint_sample_index_ + 1U) % paint_samples_.size();
        paint_sample_count_ = std::min(paint_sample_count_ + 1U, paint_samples_.size());
    }
}

std::uint64_t NativeWindow::PaintP95Microseconds() const noexcept {
    if (paint_sample_count_ == 0U) return 0U;
    auto samples = paint_samples_;
    std::sort(samples.begin(), samples.begin() + paint_sample_count_);
    const auto index = std::min(paint_sample_count_ - 1U, (paint_sample_count_ * 95U + 99U) / 100U - 1U);
    return samples[index];
}

void NativeWindow::DrawSearch(const D2D1_SIZE_F& size) {
    const auto right = SearchRightEdge(size);
    const auto bounds = D2D1::RectF(kContentInset, kSearchTop, right, kSearchTop + kSearchHeight);
    render_target_->FillRectangle(bounds, surface_brush_.Get());
    render_target_->DrawRectangle(bounds, search_active_ ? accent_brush_.Get() : line_brush_.Get(), search_active_ ? 2.0F : 1.0F);
    const auto* text = search_text_.empty() ? L"Search sensors or hardware..." : search_text_.c_str();
    DrawTextLine(text, D2D1::RectF(kContentInset + 12.0F, kSearchTop, right - 10.0F, kSearchTop + kSearchHeight), search_text_.empty() ? muted_brush_.Get() : text_brush_.Get(), body_format_.Get());

    const auto favorites = FavoritesButtonBounds(size);
    render_target_->FillRectangle(favorites, settings_.favorites_only ? surface_alternate_brush_.Get() : surface_brush_.Get());
    render_target_->DrawRectangle(favorites, settings_.favorites_only ? accent_brush_.Get() : line_brush_.Get(), settings_.favorites_only ? 1.5F : 1.0F);
    DrawTextLine(
        settings_.favorites_only ? L"\u2605  Favorites" : L"\u2606  Favorites",
        D2D1::RectF(favorites.left + 12.0F, favorites.top, favorites.right - 8.0F, favorites.bottom),
        settings_.favorites_only ? accent_brush_.Get() : text_brush_.Get(),
        body_format_.Get());
}

void NativeWindow::DrawHeader(const D2D1_SIZE_F& size) {
    render_target_->FillRectangle(D2D1::RectF(0.0F, 0.0F, size.width, static_cast<float>(kHeaderHeight)), accent_brush_.Get());
    render_target_->FillRectangle(D2D1::RectF(1.0F, 1.0F, size.width - 1.0F, static_cast<float>(kHeaderHeight)), header_brush_.Get());

    if (header_logo_bitmap_ != nullptr) {
        render_target_->DrawBitmap(
            header_logo_bitmap_.Get(),
            D2D1::RectF(10.0F, 10.0F, 58.0F, 58.0F),
            1.0F,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    DrawTextLine(L"HardwareScope", D2D1::RectF(66.0F, 7.0F, 286.0F, 43.0F), accent_brush_.Get(), title_format_.Get());
    DrawTextLine(L"Sensors", D2D1::RectF(68.0F, 38.0F, 286.0F, 62.0F), muted_brush_.Get(), subtitle_format_.Get());

    const auto controls_left = size.width - static_cast<float>(kWindowButtonWidth * kWindowButtonCount);
    render_target_->DrawLine(D2D1::Point2F(controls_left, 0.0F), D2D1::Point2F(controls_left, static_cast<float>(kHeaderHeight)), line_brush_.Get(), 1.0F);
    for (int index = 0; index < kWindowButtonCount; ++index) {
        const auto left = controls_left + static_cast<float>(index * kWindowButtonWidth);
        const auto bounds = D2D1::RectF(left, 0.0F, left + static_cast<float>(kWindowButtonWidth), static_cast<float>(kHeaderHeight));
        if (hover_window_button_ == index) {
            render_target_->FillRectangle(bounds, index == 3 ? close_hover_brush_.Get() : surface_alternate_brush_.Get());
        }
        if (index > 0) render_target_->DrawLine(D2D1::Point2F(left, 0.0F), D2D1::Point2F(left, static_cast<float>(kHeaderHeight)), line_brush_.Get(), 1.0F);
        const auto center_x = left + static_cast<float>(kWindowButtonWidth) * 0.5F;
        constexpr auto center_y = static_cast<float>(kHeaderHeight) * 0.5F;
        auto* const icon_brush = index == 3 && hover_window_button_ == 3 ? close_icon_brush_.Get() : index == 0 ? accent_brush_.Get() : text_brush_.Get();
        if (index == 0) {
            render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), 7.0F, 7.0F), icon_brush, 1.7F);
            render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), 2.4F, 2.4F), icon_brush, 1.7F);
            for (int spoke = 0; spoke < 8; ++spoke) {
                const auto angle = static_cast<float>(spoke) * 3.14159265F / 4.0F;
                render_target_->DrawLine(
                    D2D1::Point2F(center_x + std::cos(angle) * 8.5F, center_y + std::sin(angle) * 8.5F),
                    D2D1::Point2F(center_x + std::cos(angle) * 11.0F, center_y + std::sin(angle) * 11.0F),
                    icon_brush,
                    1.7F);
            }
        } else if (index == 1) {
            render_target_->DrawLine(D2D1::Point2F(center_x - 8.0F, center_y + 5.0F), D2D1::Point2F(center_x + 8.0F, center_y + 5.0F), icon_brush, 1.7F);
        } else if (index == 2) {
            if (IsZoomed(window_)) {
                render_target_->DrawRectangle(D2D1::RectF(center_x - 6.0F, center_y - 7.0F, center_x + 7.0F, center_y + 6.0F), icon_brush, 1.5F);
                render_target_->DrawRectangle(D2D1::RectF(center_x - 9.0F, center_y - 4.0F, center_x + 4.0F, center_y + 9.0F), icon_brush, 1.5F);
            } else {
                render_target_->DrawRectangle(D2D1::RectF(center_x - 8.0F, center_y - 8.0F, center_x + 8.0F, center_y + 8.0F), icon_brush, 1.5F);
            }
        } else {
            render_target_->DrawLine(D2D1::Point2F(center_x - 7.0F, center_y - 7.0F), D2D1::Point2F(center_x + 7.0F, center_y + 7.0F), icon_brush, 1.8F);
            render_target_->DrawLine(D2D1::Point2F(center_x + 7.0F, center_y - 7.0F), D2D1::Point2F(center_x - 7.0F, center_y + 7.0F), icon_brush, 1.8F);
        }
    }
    render_target_->DrawLine(D2D1::Point2F(0.0F, static_cast<float>(kHeaderHeight)), D2D1::Point2F(size.width, static_cast<float>(kHeaderHeight)), line_brush_.Get(), 1.0F);
}

void NativeWindow::DrawSensorTable(const D2D1_SIZE_F& size, const SensorSnapshot& snapshot) {
    const auto table_width = std::max(0.0F, size.width - kContentInset * 2.0F);
    if (sensor_layout_table_width_ == 0.0F) sensor_layout_table_width_ = table_width;
    else if (!in_size_move_.load(std::memory_order_acquire) && std::abs(sensor_layout_table_width_ - table_width) > 0.5F) {
        ClearTextLayoutCache();
        sensor_layout_table_width_ = table_width;
    }
    const auto left = kContentInset;
    render_target_->FillRectangle(D2D1::RectF(left, kTableTop, left + table_width, kTableTop + kColumnHeaderHeight), line_brush_.Get());

    const std::array<const wchar_t*, 6> headers{L"OSD", L"Sensor", L"Current", L"Minimum", L"Maximum", L"Hardware"};
    const auto columns = CalculateTableColumns(left, table_width);
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const auto text_left = index == 1U ? columns[index] + kFavoriteSlotWidth : columns[index];
        DrawTextLine(headers[index], D2D1::RectF(text_left, kTableTop, columns[index + 1], kTableTop + kColumnHeaderHeight), muted_brush_.Get(), value_format_.Get());
    }
    DrawTextLine(L"\u2606", D2D1::RectF(columns[1] + 2.0F, kTableTop, columns[1] + kFavoriteSlotWidth, kTableTop + kColumnHeaderHeight), muted_brush_.Get(), value_format_.Get());

    const auto viewport_top = kTableTop + kColumnHeaderHeight;
    const auto viewport_bottom = std::max(viewport_top, size.height);
    const auto viewport_height = viewport_bottom - viewport_top;
    const auto view = BuildSensorView(snapshot, collapsed_sections_, search_text_, true, &settings_);
    const auto content_height = static_cast<float>(view.count) * kSensorRowHeight;
    scroll_offset_ = std::clamp(scroll_offset_, 0.0F, std::max(0.0F, content_height - viewport_height));
    const auto first = static_cast<std::uint32_t>(scroll_offset_ / kSensorRowHeight);
    auto y = viewport_top - std::fmod(scroll_offset_, kSensorRowHeight);

    if (view.count == 0U && settings_.favorites_only) {
        DrawTextLine(
            search_text_.empty()
                ? L"No favorites yet. Turn off Favorites, then click a star beside any sensor."
                : L"No favorites match your search.",
            D2D1::RectF(left + 18.0F, viewport_top + 18.0F, left + table_width - 18.0F, viewport_top + 62.0F),
            muted_brush_.Get(),
            body_format_.Get());
    }

    for (auto row_index = first; row_index < view.count && y < viewport_bottom; ++row_index, y += kSensorRowHeight) {
        const auto& row = view.rows[row_index];
        auto* const section_brush = section_brushes_[static_cast<std::size_t>(row.section)].Get();
        if (row.is_section) {
            render_target_->FillRectangle(D2D1::RectF(left, y, left + table_width, std::min(y + kSensorRowHeight, viewport_bottom)), header_brush_.Get());
            const auto section_bit = 1U << static_cast<std::uint32_t>(row.section);
            const auto collapsed = (collapsed_sections_ & section_bit) != 0U && search_text_.empty();
            DrawTextLine(collapsed ? L"›" : L"⌄", D2D1::RectF(left + 10.0F, y, left + 38.0F, y + kSensorRowHeight), section_brush, value_format_.Get());
            DrawTextLine(SensorSectionName(row.section), D2D1::RectF(left + 42.0F, y, columns[2], y + kSensorRowHeight), section_brush, value_format_.Get());
            wchar_t count_text[32]{};
            if (row.section == SensorSection::cpu_temperatures && row.matching_sensor_count == 0U) {
                static_cast<void>(wcscpy_s(count_text, L"waiting for reading"));
            } else {
                static_cast<void>(swprintf_s(count_text, L"%u sensors", static_cast<unsigned>(row.matching_sensor_count)));
            }
            DrawTextLine(count_text, D2D1::RectF(columns[2], y, columns[3], y + kSensorRowHeight), muted_brush_.Get(), body_format_.Get());
            render_target_->DrawLine(D2D1::Point2F(left, y + kSensorRowHeight), D2D1::Point2F(left + table_width, y + kSensorRowHeight), line_brush_.Get(), 1.0F);
            continue;
        }

        if (row.is_placeholder) {
            render_target_->FillRectangle(D2D1::RectF(left, y, left + table_width, std::min(y + kSensorRowHeight, viewport_bottom)), surface_brush_.Get());
            render_target_->FillRectangle(D2D1::RectF(left, y, left + 3.0F, std::min(y + kSensorRowHeight, viewport_bottom)), section_brush);
            render_target_->DrawLine(D2D1::Point2F(left, y + kSensorRowHeight), D2D1::Point2F(left + table_width, y + kSensorRowHeight), line_brush_.Get(), 1.0F);
            DrawTextLine(L"CPU package temperature", D2D1::RectF(columns[1], y, columns[2], y + kSensorRowHeight), text_brush_.Get(), body_format_.Get());
            DrawTextLine(L"—", D2D1::RectF(columns[2], y, columns[3], y + kSensorRowHeight), muted_brush_.Get(), value_format_.Get());
            const auto status = sensor_worker_.PrivilegedStatus();
            const auto* explanation = status == PrivilegedSensorStatus::starting
                ? L"Sensor service is starting"
                : status == PrivilegedSensorStatus::unavailable
                    ? L"Sensor service unavailable"
                    : L"Waiting for a supported CPU reading";
            DrawTextLine(explanation, D2D1::RectF(columns[5], y, columns[6], y + kSensorRowHeight), muted_brush_.Get(), body_format_.Get());
            continue;
        }

        const auto row_brush = row_index % 2U == 0U ? surface_brush_.Get() : surface_alternate_brush_.Get();
        render_target_->FillRectangle(D2D1::RectF(left, y, left + table_width, std::min(y + kSensorRowHeight, viewport_bottom)), row_brush);
        render_target_->FillRectangle(D2D1::RectF(left, y, left + 3.0F, std::min(y + kSensorRowHeight, viewport_bottom)), section_brush);
        render_target_->DrawLine(D2D1::Point2F(left, y + kSensorRowHeight), D2D1::Point2F(left + table_width, y + kSensorRowHeight), line_brush_.Get(), 1.0F);

        const auto& sensor = snapshot.sensors[row.sensor_index];
        const auto checkbox = D2D1::RectF(columns[0] + 8.0F, y + 7.0F, columns[0] + 23.0F, y + 22.0F);
        render_target_->DrawRectangle(checkbox, IsSensorSelectedForOsd(sensor, settings_) ? section_brush : muted_brush_.Get(), 1.5F);
        if (IsSensorSelectedForOsd(sensor, settings_)) {
            render_target_->DrawLine(D2D1::Point2F(checkbox.left + 3.0F, checkbox.top + 8.0F), D2D1::Point2F(checkbox.left + 7.0F, checkbox.bottom - 3.0F), section_brush, 2.0F);
            render_target_->DrawLine(D2D1::Point2F(checkbox.left + 7.0F, checkbox.bottom - 3.0F), D2D1::Point2F(checkbox.right - 3.0F, checkbox.top + 3.0F), section_brush, 2.0F);
        }
        DrawTextLine(
            settings_.IsFavorite(sensor.id) ? L"\u2605" : L"\u2606",
            D2D1::RectF(columns[1] + 2.0F, y, columns[1] + kFavoriteSlotWidth, y + kSensorRowHeight),
            settings_.IsFavorite(sensor.id) ? section_brush : muted_brush_.Get(),
            value_format_.Get());
        wchar_t current[48]{};
        wchar_t minimum[48]{};
        wchar_t maximum[48]{};
        FormatValue(sensor, sensor.current, current, std::size(current));
        FormatValue(sensor, sensor.minimum, minimum, std::size(minimum));
        FormatValue(sensor, sensor.maximum, maximum, std::size(maximum));
        auto& layouts = sensor_text_layouts_[row.sensor_index];
        if (layouts.id != sensor.id) {
            layouts = SensorTextLayouts{};
            layouts.id = sensor.id;
        }
        const auto update_dynamic = [](auto& stored, const wchar_t* const value, auto& layout) {
            if (wcscmp(stored.data(), value) == 0) return;
            static_cast<void>(wcscpy_s(stored.data(), stored.size(), value));
            layout.Reset();
        };
        update_dynamic(layouts.current, current, layouts.current_layout);
        update_dynamic(layouts.minimum, minimum, layouts.minimum_layout);
        update_dynamic(layouts.maximum, maximum, layouts.maximum_layout);
        const auto ensure_layout = [&](auto& layout, const wchar_t* const value, IDWriteTextFormat* const format, const float width) {
            if (layout == nullptr) {
                static_cast<void>(dwrite_factory_->CreateTextLayout(
                    value,
                    static_cast<UINT32>(wcslen(value)),
                    format,
                    std::max(1.0F, width),
                    kSensorRowHeight,
                    layout.ReleaseAndGetAddressOf()));
            }
            return layout.Get();
        };
        const auto name_layout = ensure_layout(layouts.name_layout, sensor.name.data(), body_format_.Get(), columns[2] - columns[1] - kFavoriteSlotWidth);
        const auto current_layout = ensure_layout(layouts.current_layout, layouts.current.data(), value_format_.Get(), columns[3] - columns[2]);
        const auto minimum_layout = ensure_layout(layouts.minimum_layout, layouts.minimum.data(), body_format_.Get(), columns[4] - columns[3]);
        const auto maximum_layout = ensure_layout(layouts.maximum_layout, layouts.maximum.data(), body_format_.Get(), columns[5] - columns[4]);
        const auto hardware_layout = ensure_layout(layouts.hardware_layout, sensor.hardware.data(), body_format_.Get(), columns[6] - columns[5]);
        if (name_layout != nullptr) render_target_->DrawTextLayout(D2D1::Point2F(columns[1] + kFavoriteSlotWidth, y), name_layout, text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (current_layout != nullptr) render_target_->DrawTextLayout(D2D1::Point2F(columns[2], y), current_layout, section_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (minimum_layout != nullptr) render_target_->DrawTextLayout(D2D1::Point2F(columns[3], y), minimum_layout, muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (maximum_layout != nullptr) render_target_->DrawTextLayout(D2D1::Point2F(columns[4], y), maximum_layout, muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (hardware_layout != nullptr) render_target_->DrawTextLayout(D2D1::Point2F(columns[5], y), hardware_layout, muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void NativeWindow::DrawTooltip(const D2D1_SIZE_F& size, const SensorSnapshot& snapshot) {
    if (!tooltip_visible_ || hover_kind_ == HoverKind::none) return;
    std::wstring text;
    if (hover_kind_ == HoverKind::column) {
        text = ColumnExplanation(hover_column_);
    } else {
        for (std::uint32_t index = 0U; index < snapshot.count; ++index) {
            if (snapshot.sensors[index].id == hover_sensor_id_) {
                text = SensorExplanation(snapshot.sensors[index]);
                break;
            }
        }
    }
    if (text.empty()) return;
    constexpr float tooltip_width = 420.0F;
    constexpr float tooltip_height = 72.0F;
    auto left = hover_point_.x + 16.0F;
    auto top = hover_point_.y + 18.0F;
    if (left + tooltip_width > size.width - 8.0F) left = std::max(8.0F, hover_point_.x - tooltip_width - 16.0F);
    if (top + tooltip_height > size.height - 8.0F) top = std::max(8.0F, hover_point_.y - tooltip_height - 18.0F);
    const auto bounds = D2D1::RectF(left, top, left + tooltip_width, top + tooltip_height);
    render_target_->FillRectangle(bounds, surface_alternate_brush_.Get());
    render_target_->DrawRectangle(bounds, accent_brush_.Get(), 1.0F);
    DrawTextLine(text.c_str(), D2D1::RectF(left + 12.0F, top + 7.0F, left + tooltip_width - 12.0F, top + tooltip_height - 7.0F), text_brush_.Get(), tooltip_format_.Get());
}

void NativeWindow::DrawTextLine(const wchar_t* const text, const D2D1_RECT_F& rectangle, ID2D1Brush* const brush, IDWriteTextFormat* const format) {
    if (text == nullptr || brush == nullptr || format == nullptr) return;
    render_target_->DrawTextW(text, static_cast<UINT32>(wcslen(text)), format, rectangle, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void NativeWindow::ResizeRenderTarget(const UINT width, const UINT height) {
    if (render_target_ != nullptr && width > 0U && height > 0U) {
        const auto result = render_target_->Resize(D2D1::SizeU(width, height));
        if (FAILED(result)) DiscardDeviceResources();
    }
}

LRESULT NativeWindow::HitTest(const POINT screen_point) const noexcept {
    POINT client_point = screen_point;
    ScreenToClient(window_, &client_point);
    RECT client{};
    GetClientRect(window_, &client);
    const auto border = IsZoomed(window_) ? 0 : MulDiv(7, static_cast<int>(dpi_), 96);
    const auto header = MulDiv(kHeaderHeight, static_cast<int>(dpi_), 96);
    const auto controls = MulDiv(kWindowButtonWidth * kWindowButtonCount, static_cast<int>(dpi_), 96);
    return HitTestWindowRegion(client.right, client.bottom, client_point.x, client_point.y, border, header, controls);
}

void NativeWindow::HandleWindowButton(const POINT client_point) {
    RECT bounds{};
    GetClientRect(window_, &bounds);
    if (client_point.y < 0 || client_point.y >= MulDiv(kHeaderHeight, static_cast<int>(dpi_), 96)) return;
    const auto button_width = MulDiv(kWindowButtonWidth, static_cast<int>(dpi_), 96);
    const auto controls_left = bounds.right - button_width * kWindowButtonCount;
    if (client_point.x < controls_left) return;
    const auto index = (client_point.x - controls_left) / std::max(1, button_width);
    if (index == 0) {
        ShowSettings();
    }
    else if (index == 1) {
        MinimizeToTray();
    }
    else if (index == 2) ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
    else if (index == 3) PostMessageW(window_, WM_CLOSE, 0, 0);
}

void NativeWindow::HandleContentClick(const POINT client_point) {
    const auto scale = dpi_ == 0U ? 1.0F : 96.0F / static_cast<float>(dpi_);
    const auto x = static_cast<float>(client_point.x) * scale;
    const auto y = static_cast<float>(client_point.y) * scale;
    RECT client{};
    GetClientRect(window_, &client);
    const auto client_width = static_cast<float>(client.right) * scale;
    const auto size = D2D1::SizeF(client_width, static_cast<float>(client.bottom) * scale);
    const auto search_right = SearchRightEdge(size);
    if (x >= kContentInset && x <= search_right && y >= kSearchTop && y <= kSearchTop + kSearchHeight) {
        search_active_ = true;
        SetFocus(window_);
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    const auto favorites = FavoritesButtonBounds(size);
    if (x >= favorites.left && x <= favorites.right && y >= favorites.top && y <= favorites.bottom) {
        search_active_ = false;
        settings_.favorites_only = !settings_.favorites_only;
        scroll_offset_ = 0.0F;
        SaveSettings();
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    const auto was_active = search_active_;
    search_active_ = false;
    const auto viewport_top = kTableTop + kColumnHeaderHeight;
    const auto viewport_bottom = static_cast<float>(client.bottom) * scale;
    if (y >= viewport_top && y < viewport_bottom) {
        snapshots_.ReadLatest(ui_snapshot_);
        const auto view = BuildSensorView(ui_snapshot_, collapsed_sections_, search_text_, true, &settings_);
        const auto row_index = static_cast<std::uint32_t>((y - viewport_top + scroll_offset_) / kSensorRowHeight);
        if (row_index < view.count && view.rows[row_index].is_section) {
            collapsed_sections_ ^= 1U << static_cast<std::uint32_t>(view.rows[row_index].section);
            settings_.collapsed_sections = collapsed_sections_;
            SaveSettings();
            scroll_offset_ = 0.0F;
        } else if (row_index < view.count && !view.rows[row_index].is_placeholder) {
            const auto table_width = std::max(0.0F, client_width - kContentInset * 2.0F);
            const auto columns = CalculateTableColumns(kContentInset, table_width);
            const auto& sensor = ui_snapshot_.sensors[view.rows[row_index].sensor_index];
            if (x >= columns[1] && x <= columns[1] + kFavoriteSlotWidth) {
                if (settings_.IsFavorite(sensor.id)) static_cast<void>(settings_.RemoveFavorite(sensor.id));
                else static_cast<void>(settings_.AddFavorite(sensor.id));
                settings_.favorites_initialized = true;
                SaveSettings();
                scroll_offset_ = std::max(0.0F, scroll_offset_ - (settings_.favorites_only ? kSensorRowHeight : 0.0F));
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
            if (x < columns[0] || x > columns[1]) {
                if (was_active) InvalidateRect(window_, nullptr, FALSE);
                return;
            }
            const auto selected = IsSensorSelectedForOsd(sensor, settings_);
            SetSensorSelectedForOsd(sensor, settings_, !selected);
            SaveSettings();
            osd_window_.ApplySettings(settings_);
            fps_osd_window_.ApplySettings(settings_);
            osd_window_.Update(ui_snapshot_);
            fps_osd_window_.Update(ui_snapshot_);
            if (sensor.kind == SensorKind::frame_rate) {
                sensor_worker_.ConfigureFps(
                    settings_.fps_enabled,
                    settings_.fps_game_only,
                    settings_.fps_refresh_interval_ms,
                    settings_.fps_smoothing_interval_ms);
                if (!suspended_) sensor_worker_.Start(std::chrono::milliseconds{settings_.refresh_interval_ms});
            }
        }
    }
    if (was_active || y >= viewport_top) InvalidateRect(window_, nullptr, FALSE);
}

void NativeWindow::HandleCharacter(const wchar_t character) {
    if (!search_active_) return;
    if (character == L'\b') {
        if (!search_text_.empty()) search_text_.pop_back();
    } else if (character == 27) {
        search_text_.clear();
        search_active_ = false;
    } else if (std::iswprint(character) != 0 && search_text_.size() < 96U) {
        search_text_.push_back(character);
    } else {
        return;
    }
    scroll_offset_ = 0.0F;
    InvalidateRect(window_, nullptr, FALSE);
}

void NativeWindow::UpdateHover(const POINT client_point) {
    if (!tracking_mouse_leave_) {
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = window_;
        tracking_mouse_leave_ = TrackMouseEvent(&tracking) != FALSE;
    }
    const auto scale = dpi_ == 0U ? 1.0F : 96.0F / static_cast<float>(dpi_);
    const auto x = static_cast<float>(client_point.x) * scale;
    const auto y = static_cast<float>(client_point.y) * scale;
    RECT client{};
    GetClientRect(window_, &client);
    const auto width = static_cast<float>(client.right) * scale;
    const auto height = static_cast<float>(client.bottom) * scale;
    int next_window_button = -1;
    const auto controls_left = width - static_cast<float>(kWindowButtonWidth * kWindowButtonCount);
    if (y >= 0.0F && y < static_cast<float>(kHeaderHeight) && x >= controls_left && x < width) {
        next_window_button = std::clamp(static_cast<int>((x - controls_left) / static_cast<float>(kWindowButtonWidth)), 0, kWindowButtonCount - 1);
    }
    if (next_window_button != hover_window_button_) {
        hover_window_button_ = next_window_button;
        InvalidateRect(window_, nullptr, FALSE);
    }
    HoverKind next_kind{HoverKind::none};
    TableColumn next_column{TableColumn::sensor};
    std::uint64_t next_sensor{};
    const auto table_width = std::max(0.0F, width - kContentInset * 2.0F);
    const auto left = kContentInset;
    const auto columns = CalculateTableColumns(left, table_width);
    if (next_window_button >= 0) {
        // Caption buttons never show sensor help.
    } else if (y >= kTableTop && y < kTableTop + kColumnHeaderHeight) {
        for (std::size_t column = 0U; column + 1U < columns.size(); ++column) {
            if (x >= columns[column] && x < columns[column + 1U]) {
                next_kind = HoverKind::column;
                next_column = static_cast<TableColumn>(column);
                break;
            }
        }
    } else {
        const auto viewport_top = kTableTop + kColumnHeaderHeight;
        const auto viewport_bottom = height;
        if (y >= viewport_top && y < viewport_bottom) {
            snapshots_.ReadLatest(ui_snapshot_);
            const auto view = BuildSensorView(ui_snapshot_, collapsed_sections_, search_text_, true, &settings_);
            const auto row_index = static_cast<std::uint32_t>((y - viewport_top + scroll_offset_) / kSensorRowHeight);
            if (row_index < view.count && !view.rows[row_index].is_section && !view.rows[row_index].is_placeholder) {
                next_kind = HoverKind::sensor;
                next_sensor = ui_snapshot_.sensors[view.rows[row_index].sensor_index].id;
            }
        }
    }
    const auto unchanged = next_kind == hover_kind_
        && (next_kind != HoverKind::column || next_column == hover_column_)
        && (next_kind != HoverKind::sensor || next_sensor == hover_sensor_id_);
    hover_point_ = D2D1::Point2F(x, y);
    if (unchanged) {
        if (tooltip_visible_) InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    KillTimer(window_, kTooltipTimer);
    const auto was_visible = tooltip_visible_;
    tooltip_visible_ = false;
    hover_kind_ = next_kind;
    hover_column_ = next_column;
    hover_sensor_id_ = next_sensor;
    if (hover_kind_ != HoverKind::none) static_cast<void>(SetTimer(window_, kTooltipTimer, 1'000U, nullptr));
    if (was_visible) InvalidateRect(window_, nullptr, FALSE);
}

void NativeWindow::ClearHover() noexcept {
    KillTimer(window_, kTooltipTimer);
    const auto was_visible = tooltip_visible_;
    const auto had_window_button = hover_window_button_ >= 0;
    hover_window_button_ = -1;
    hover_kind_ = HoverKind::none;
    hover_sensor_id_ = 0U;
    tooltip_visible_ = false;
    if ((was_visible || had_window_button) && window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
}

bool NativeWindow::AddTrayIcon() noexcept {
    if (tray_icon_added_ || window_ == nullptr) return tray_icon_added_;
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window_;
    icon.uID = 1U;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID | NIF_SHOWTIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadHardwareScopeIcon(instance_, dpi_, SM_CXSMICON, SM_CYSMICON);
    if (icon.hIcon == nullptr) icon.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
    icon.guidItem = kTrayIconGuid;
    static_cast<void>(wcscpy_s(icon.szTip, L"HardwareScope"));
    if (!Shell_NotifyIconW(NIM_ADD, &icon)) return false;
    icon.uVersion = NOTIFYICON_VERSION_4;
    static_cast<void>(Shell_NotifyIconW(NIM_SETVERSION, &icon));
    tray_icon_added_ = true;
    return true;
}

void NativeWindow::RemoveTrayIcon() noexcept {
    tray_panel_.Hide();
    if (!tray_icon_added_) return;
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window_;
    icon.uID = 1U;
    icon.uFlags = NIF_GUID;
    icon.guidItem = kTrayIconGuid;
    static_cast<void>(Shell_NotifyIconW(NIM_DELETE, &icon));
    tray_icon_added_ = false;
}

void NativeWindow::MinimizeToTray() noexcept {
    static_cast<void>(AddTrayIcon());
    tray_panel_.Hide();
    ShowWindow(window_, SW_HIDE);
}

void NativeWindow::RestoreFromTray() noexcept {
    tray_panel_.Hide();
    if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
    else ShowWindow(window_, SW_SHOW);
    static_cast<void>(SetForegroundWindow(window_));
}

void NativeWindow::ShowTrayMenu() noexcept {
    tray_panel_.Hide();
    const auto menu = CreatePopupMenu();
    if (menu == nullptr) return;
    static_cast<void>(AppendMenuW(menu, MF_STRING, kCommandOpen, L"Open HardwareScope"));
    if (pending_update_) {
        wchar_t update_text[96]{};
        static_cast<void>(swprintf_s(
            update_text,
            L"Install update %u.%u.%u…",
            pending_update_->manifest.version.major,
            pending_update_->manifest.version.minor,
            pending_update_->manifest.version.patch));
        static_cast<void>(AppendMenuW(menu, MF_STRING, kCommandInstallUpdate, update_text));
    }
    static_cast<void>(AppendMenuW(menu, MF_STRING | (settings_.show_osd ? MF_CHECKED : MF_UNCHECKED), kCommandToggleOsd, L"Show on-screen display"));
    static_cast<void>(AppendMenuW(menu, MF_STRING, kCommandResetMinMax, L"Reset Min/Max"));
    static_cast<void>(AppendMenuW(menu, MF_STRING, kCommandSettings, L"Settings…"));
    static_cast<void>(AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr));
    static_cast<void>(AppendMenuW(menu, MF_STRING, kCommandExit, L"Exit"));
    POINT cursor{};
    static_cast<void>(GetCursorPos(&cursor));
    static_cast<void>(SetForegroundWindow(window_));
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, cursor.x, cursor.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (command != 0) HandleCommand(command);
    static_cast<void>(PostMessageW(window_, WM_NULL, 0, 0));
}

void NativeWindow::HandleCommand(const int command) noexcept {
    if (command == kCommandOpen) RestoreFromTray();
    else if (command == kCommandInstallUpdate) PromptForPendingUpdate();
    else if (command == kCommandSettings) ShowSettings();
    else if (command == kCommandResetMinMax) {
        sensor_worker_.RequestMinMaxReset();
    }
    else if (command == kCommandToggleOsd) {
        settings_.show_osd = !settings_.show_osd;
        osd_window_.SetVisible(settings_.show_osd);
        fps_osd_window_.SetVisible(settings_.show_osd);
        SaveSettings();
    } else if (command == kCommandExit) {
        static_cast<void>(PostMessageW(window_, WM_CLOSE, 0, 0));
    }
}

void NativeWindow::ShowSettings() noexcept {
    auto updated = settings_;
    graph_window_.CapturePlacement(updated);
    snapshots_.ReadLatest(ui_snapshot_);
    if (!ShowSettingsWindow(window_, updated, ui_snapshot_)) return;
    settings_ = updated;
    palette_ = PaletteFor(settings_.theme, settings_.text_color_rgb, settings_.high_contrast);
    SaveSettings();
    if (!IsIsolatedTest()) static_cast<void>(ApplyStartupRegistration(settings_.start_with_windows, settings_.start_minimized));
    osd_window_.ApplySettings(settings_);
    fps_osd_window_.ApplySettings(settings_);
    graph_window_.ApplySettings(settings_);
    tray_panel_.ApplySettings(settings_);
    sensor_worker_.ConfigureInterval(std::chrono::milliseconds{settings_.refresh_interval_ms});
    sensor_worker_.ConfigureFps(
        settings_.fps_enabled,
        settings_.fps_game_only,
        settings_.fps_refresh_interval_ms,
        settings_.fps_smoothing_interval_ms);
    sensor_worker_.ConfigureMinMaxReset(
        settings_.reset_min_max_on_game_launch,
        settings_.reset_min_max_interval_minutes);
    if (!suspended_) sensor_worker_.Start(std::chrono::milliseconds{settings_.refresh_interval_ms});
    ScheduleAutomaticUpdateCheck();
    DiscardDeviceResources();
    ApplyDwmAppearance();
    InvalidateRect(window_, nullptr, FALSE);
}

void NativeWindow::ShowFirstRunSetup() noexcept {
    constexpr int use_recommended = 4'001;
    constexpr int customize = 4'002;
    constexpr TASKDIALOG_BUTTON buttons[]{
        {use_recommended, L"Use recommended settings\nFavorites, stable updates, game-only FPS, and a balanced 750 ms hardware refresh."},
        {customize, L"Customize settings\nChoose appearance, accessibility, monitoring, and on-screen-display options."},
    };
    TASKDIALOGCONFIG configuration{};
    configuration.cbSize = sizeof(configuration);
    configuration.hwndParent = window_;
    configuration.dwFlags = TDF_USE_COMMAND_LINKS | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    configuration.pszWindowTitle = L"Welcome to HardwareScope";
    configuration.pszMainIcon = TD_INFORMATION_ICON;
    configuration.pszMainInstruction = L"Set up HardwareScope";
    configuration.pszContent = L"Start with the recommended lightweight configuration or review every option now. You can change these choices later from Settings.";
    configuration.cButtons = static_cast<UINT>(std::size(buttons));
    configuration.pButtons = buttons;
    configuration.nDefaultButton = use_recommended;

    int selected = use_recommended;
    using TaskDialogIndirectFunction = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    auto controls = GetModuleHandleW(L"comctl32.dll");
    if (controls == nullptr) controls = LoadLibraryW(L"comctl32.dll");
    const auto task_dialog = controls == nullptr
        ? nullptr
        : reinterpret_cast<TaskDialogIndirectFunction>(GetProcAddress(controls, "TaskDialogIndirect"));
    if (task_dialog == nullptr || FAILED(task_dialog(&configuration, &selected, nullptr, nullptr))) {
        selected = MessageBoxW(
            window_,
            L"Use HardwareScope's recommended lightweight settings?\n\nChoose No to open Settings and customize the app.",
            L"Welcome to HardwareScope",
            MB_YESNO | MB_ICONINFORMATION) == IDNO
            ? customize
            : use_recommended;
    }

    settings_.onboarding_completed = true;
    first_run_ = false;
    SaveSettings();
    if (selected == customize) ShowSettings();
    else ScheduleAutomaticUpdateCheck();
}

void NativeWindow::ScheduleAutomaticUpdateCheck(const std::uint32_t default_delay_ms) noexcept {
    KillTimer(window_, kStartupUpdateTimer);
    if (!settings_.automatic_updates) return;
    const auto now = CurrentUnixSeconds();
    auto delay = static_cast<std::uint64_t>(default_delay_ms);
    if (settings_.update_snooze_until_unix_seconds > now) {
        const auto seconds = settings_.update_snooze_until_unix_seconds - now;
        delay = std::clamp<std::uint64_t>(seconds * 1'000ULL, 1'000ULL, static_cast<std::uint64_t>(UINT_MAX));
    }
    static_cast<void>(SetTimer(window_, kStartupUpdateTimer, static_cast<UINT>(delay), nullptr));
}

bool NativeWindow::ShowUpdateNotification(const UpdateCompletion& completion) noexcept {
    if (!AddTrayIcon()) return false;
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window_;
    icon.uID = 1U;
    icon.uFlags = NIF_INFO | NIF_GUID;
    icon.guidItem = kTrayIconGuid;
    static_cast<void>(wcscpy_s(icon.szInfoTitle, L"HardwareScope update available"));
    static_cast<void>(swprintf_s(
        icon.szInfo,
        L"Version %u.%u.%u is available. Click here to update or choose a reminder time.",
        completion.manifest.version.major,
        completion.manifest.version.minor,
        completion.manifest.version.patch));
    icon.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND | NIIF_RESPECT_QUIET_TIME;
    return Shell_NotifyIconW(NIM_MODIFY, &icon) != FALSE;
}

void NativeWindow::PromptForPendingUpdate() noexcept {
    if (!pending_update_) return;
    const auto completion = *pending_update_;
    pending_update_.reset();
    PromptForUpdate(completion);
}

void NativeWindow::PromptForUpdate(const UpdateCompletion& completion) noexcept {
    const auto popup = GetLastActivePopup(window_);
    const auto choice = ShowUpdatePromptWindow(popup, instance_, completion.manifest, settings_);
    if (choice.action == UpdatePromptAction::remind_later) {
        settings_.update_snooze_until_unix_seconds = CurrentUnixSeconds()
            + static_cast<std::uint64_t>(choice.delay.count());
        settings_.skipped_update_major = 0U;
        settings_.skipped_update_minor = 0U;
        settings_.skipped_update_patch = 0U;
        SaveSettings();
        ScheduleAutomaticUpdateCheck();
        return;
    }
    if (choice.action == UpdatePromptAction::skip_version) {
        settings_.update_snooze_until_unix_seconds = CurrentUnixSeconds() + 24ULL * 60ULL * 60ULL;
        settings_.skipped_update_major = completion.manifest.version.major;
        settings_.skipped_update_minor = completion.manifest.version.minor;
        settings_.skipped_update_patch = completion.manifest.version.patch;
        SaveSettings();
        ScheduleAutomaticUpdateCheck();
        return;
    }
    settings_.update_snooze_until_unix_seconds = 0U;
    settings_.skipped_update_major = 0U;
    settings_.skipped_update_minor = 0U;
    settings_.skipped_update_patch = 0U;
    SaveSettings();

    if (!BeginNativeUpdateDownload(window_, completion.manifest)) {
        ShowUpdateNoticeWindow(popup, instance_, settings_, L"Update busy",
            L"Another update operation is running. Please try again shortly.", true);
    } else if (popup != nullptr && popup != window_) {
        if (const auto button = GetDlgItem(popup, kSettingsCheckUpdatesCommand); button != nullptr) {
            EnableWindow(button, FALSE);
            SetWindowTextW(button, L"Downloading update...");
        }
    }
}

void NativeWindow::InstallVerifiedUpdate(const UpdateCompletion& completion) noexcept {
    const auto popup = GetLastActivePopup(window_);
    if (completion.installer.empty()) return;
    std::array<wchar_t, 32'768U> module_path{};
    const auto length = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0U || length >= module_path.size()) {
        ShowUpdateNoticeWindow(
            popup, instance_, settings_,
            L"Update could not start",
            L"The verified update could not be handed to Setup. Nothing was changed.",
            true);
        return;
    }
    const std::filesystem::path application{module_path.data()};
    auto updater = application.parent_path() / L"HardwareScopeUpdater.exe";
    if (!std::filesystem::exists(updater)) updater = application.parent_path() / L"HardwareScopeNativeUpdater.exe";
    if (!LaunchUpdateHandoff(updater, completion.installer, completion.manifest, GetCurrentProcessId(), application)) {
        ShowUpdateNoticeWindow(
            popup, instance_, settings_,
            L"Update could not start",
            L"The verified update could not be handed to Setup. Nothing was changed.",
            true);
        return;
    }
    if (popup != nullptr && popup != window_) static_cast<void>(PostMessageW(popup, WM_CLOSE, 0U, 0));
    static_cast<void>(PostMessageW(window_, WM_CLOSE, 0U, 0));
}

void NativeWindow::HandleUpdateCompletion(const UpdateCompletion& completion) noexcept {
    const auto popup = GetLastActivePopup(window_);
    if (popup != nullptr && popup != window_) {
        if (const auto button = GetDlgItem(popup, kSettingsCheckUpdatesCommand); button != nullptr) {
            EnableWindow(button, TRUE);
            SetWindowTextW(button, L"Check for updates");
        }
    }
    if (completion.status == UpdateCompletionStatus::ready) {
        InstallVerifiedUpdate(completion);
        return;
    }
    if (completion.status == UpdateCompletionStatus::failed) {
        if (!completion.automatic) {
            wchar_t message[256]{};
            static_cast<void>(swprintf_s(message, L"HardwareScope could not verify an update. Nothing was installed.\n\nWindows error: %u", completion.system_error));
            ShowUpdateNoticeWindow(
                popup, instance_, settings_,
                L"Update check failed",
                message,
                true);
        }
        return;
    }
    if (completion.status == UpdateCompletionStatus::current) {
        if (completion.automatic) {
            if (const auto skipped = SkippedUpdateVersion(settings_); skipped && SameVersion(*skipped, completion.manifest.version)) {
                settings_.update_snooze_until_unix_seconds = CurrentUnixSeconds() + 24ULL * 60ULL * 60ULL;
                SaveSettings();
                ScheduleAutomaticUpdateCheck();
            }
        }
        if (!completion.automatic) {
            ShowUpdateNoticeWindow(
                popup, instance_, settings_,
                L"HardwareScope is up to date",
                L"You already have the newest verified stable release.");
        }
        return;
    }

    if (completion.automatic) {
        pending_update_ = completion;
        settings_.update_snooze_until_unix_seconds = CurrentUnixSeconds() + 24ULL * 60ULL * 60ULL;
        SaveSettings();
        ScheduleAutomaticUpdateCheck();
        static_cast<void>(ShowUpdateNotification(completion));
        return;
    }
    pending_update_.reset();
    PromptForUpdate(completion);
}

void NativeWindow::ApplyDwmAppearance() noexcept {
    constexpr DWORD immersive_dark_mode_attribute = 20;
    constexpr DWORD corner_preference_attribute = 33;
    constexpr DWORD rounded_corner_preference = 2;
    const BOOL dark = settings_.theme != Theme::light ? TRUE : FALSE;
    static_cast<void>(DwmSetWindowAttribute(window_, immersive_dark_mode_attribute, &dark, sizeof(dark)));
    static_cast<void>(DwmSetWindowAttribute(window_, corner_preference_attribute, &rounded_corner_preference, sizeof(rounded_corner_preference)));
}

} // namespace hardwarescope
