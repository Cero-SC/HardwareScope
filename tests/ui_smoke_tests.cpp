#include <windows.h>
#include <commctrl.h>

#include "hardwarescope/app_commands.hpp"
#include "hardwarescope/tray_panel_window.hpp"
#include "hardwarescope/update_prompt_window.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

constexpr wchar_t kWindowClass[] = L"HardwareScope.Native.MainWindow";
constexpr wchar_t kOsdWindowClass[] = L"HardwareScope.Native.OsdWindow";
constexpr wchar_t kSettingsWindowClass[] = L"HardwareScope.Native.SettingsWindow";
constexpr wchar_t kGraphWindowClass[] = L"HardwareScope.Native.GraphWindow";

struct MonitorCollection final {
    std::array<RECT, 8U> work_areas{};
    std::size_t count{};
};

BOOL CALLBACK CollectMonitor(const HMONITOR monitor, HDC, LPRECT, const LPARAM context) noexcept {
    auto& collection = *reinterpret_cast<MonitorCollection*>(context);
    if (collection.count >= collection.work_areas.size()) return TRUE;
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (GetMonitorInfoW(monitor, &information)) collection.work_areas[collection.count++] = information.rcWork;
    return TRUE;
}

LPARAM ScreenPointParameter(const int x, const int y) noexcept {
    return MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
}

struct WindowSearch final {
    const wchar_t* class_name{};
    const wchar_t* title{};
    DWORD process_id{};
    HWND result{};
};

BOOL CALLBACK FindProcessWindow(const HWND window, const LPARAM context) noexcept {
    auto& search = *reinterpret_cast<WindowSearch*>(context);
    std::array<wchar_t, 128U> class_name{};
    std::array<wchar_t, 256U> title{};
    DWORD process_id{};
    static_cast<void>(GetWindowThreadProcessId(window, &process_id));
    static_cast<void>(GetClassNameW(window, class_name.data(), static_cast<int>(class_name.size())));
    static_cast<void>(GetWindowTextW(window, title.data(), static_cast<int>(title.size())));
    if (process_id == search.process_id
        && (search.class_name == nullptr || wcscmp(class_name.data(), search.class_name) == 0)
        && (search.title == nullptr || wcscmp(title.data(), search.title) == 0)) {
        search.result = window;
        return FALSE;
    }
    return TRUE;
}

HWND ProcessWindow(const wchar_t* class_name, const wchar_t* title, const DWORD process_id) noexcept {
    WindowSearch search{class_name, title, process_id, nullptr};
    static_cast<void>(EnumWindows(&FindProcessWindow, reinterpret_cast<LPARAM>(&search)));
    return search.result;
}

BOOL CALLBACK FindInstrumentedWindow(const HWND window, const LPARAM context) noexcept {
    std::array<wchar_t, 128U> class_name{};
    static_cast<void>(GetClassNameW(window, class_name.data(), static_cast<int>(class_name.size())));
    if (wcscmp(class_name.data(), kWindowClass) == 0
        && SendMessageW(window, hardwarescope::kDisableAutomaticUpdateTestMessage, 0U, 0U) == 1) {
        *reinterpret_cast<HWND*>(context) = window;
        return FALSE;
    }
    return TRUE;
}

} // namespace

int wmain(const int argument_count, wchar_t** arguments) {
    const auto has_argument = [&](const std::wstring_view expected) noexcept {
        for (int index = 1; index < argument_count; ++index)
            if (std::wstring_view{arguments[index]} == expected) return true;
        return false;
    };
    const auto expect_no_hooks = has_argument(L"--expect-no-hooks");
    DWORD requested_pid{};
    for (int index = 1; index + 1 < argument_count; ++index) {
        if (std::wstring_view{arguments[index]} == L"--pid") requested_pid = wcstoul(arguments[index + 1], nullptr, 10);
    }
    static_cast<void>(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    std::ofstream result_log(
        std::filesystem::temp_directory_path() / "HardwareScopeNativeUiSmokeTests.log",
        std::ios::out | std::ios::trunc);
    if (!result_log) return 2;
    struct StreamRestore final {
        std::streambuf* output;
        std::streambuf* error;
        ~StreamRestore() {
            std::cout.rdbuf(output);
            std::cerr.rdbuf(error);
        }
    } restore{std::cout.rdbuf(), std::cerr.rdbuf()};
    std::cout.rdbuf(result_log.rdbuf());
    std::cerr.rdbuf(result_log.rdbuf());

    const auto progress_window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC",
        L"HardwareScope native UI regression in progress",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        40,
        40,
        420,
        90,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (progress_window == nullptr) {
        std::cerr << "FAIL: could not create interactive smoke progress window\n";
        return 1;
    }
    ShowWindow(progress_window, SW_SHOWNOACTIVATE);
    struct WindowDestroy final {
        HWND window;
        ~WindowDestroy() {
            if (window != nullptr) DestroyWindow(window);
        }
    } destroy_progress{progress_window};

    HWND window{};
    if (expect_no_hooks) {
        if (requested_pid == 0U) { std::cerr << "FAIL: --expect-no-hooks requires --pid <target process>\n"; return 2; }
        window = ProcessWindow(kWindowClass, nullptr, requested_pid);
    } else {
        static_cast<void>(EnumWindows(&FindInstrumentedWindow, reinterpret_cast<LPARAM>(&window)));
    }
    if (window == nullptr) {
        std::cerr << "FAIL: HardwareScope native window is not running\n";
        return 1;
    }
    if (!IsWindowVisible(window)) {
        static_cast<void>(SendMessageW(window, WM_COMMAND, hardwarescope::kCommandOpen, 0));
        Sleep(250U);
    }
    const auto skip_hover = has_argument(L"--skip-hover");
    const auto skip_update_prompt = has_argument(L"--skip-update-prompt");
    if (expect_no_hooks) {
        constexpr std::array hook_messages{
            hardwarescope::kQueryTooltipVisibleMessage,
            hardwarescope::kArmTooltipTestMessage,
            hardwarescope::kQueryPaintP95Message,
            hardwarescope::kDisableAutomaticUpdateTestMessage,
            hardwarescope::kQuerySensorWorkerRunningMessage,
            hardwarescope::kQueryResumeWaitingMessage,
            hardwarescope::kQuerySnapshotSequenceMessage,
            hardwarescope::kRestoreTrayIconTestMessage,
            hardwarescope::kQueryTrayIconAddedMessage,
            hardwarescope::kApplyMainDpiTestMessage,
            hardwarescope::kShowUpdatePromptTestMessage,
            hardwarescope::kQueueAutomaticUpdateNotificationTestMessage,
            hardwarescope::kSelectSettingsTabTestMessage,
            hardwarescope::kConfigureOsdGraphTestMessage,
            hardwarescope::kConfigureFloatingGraphTestMessage,
            hardwarescope::kToggleTrayPanelTestMessage,
            hardwarescope::kQueryTrayPanelVisibleMessage,
        };
        for (const auto message : hook_messages) {
            if (SendMessageW(window, message, 144U, 0U) != 0) {
                std::cerr << "FAIL: packaged app responded to internal message 0x"
                          << std::hex << message << std::dec << '\n';
                return 1;
            }
        }

        static_cast<void>(PostMessageW(window, WM_COMMAND, hardwarescope::kCommandSettings, 0U));
        HWND settings{};
        for (int attempt = 0; attempt < 120 && settings == nullptr; ++attempt) {
            Sleep(25U);
            DWORD process_id{};
            static_cast<void>(GetWindowThreadProcessId(window, &process_id));
            settings = ProcessWindow(kSettingsWindowClass, nullptr, process_id);
        }
        if (settings == nullptr) {
            std::cerr << "FAIL: packaged Settings window did not open for hook-boundary validation\n";
            return 1;
        }
        if (SendMessageW(settings, hardwarescope::kApplySettingsDpiTestMessage, 144U, 0U) != 0) {
            std::cerr << "FAIL: packaged Settings responded to its internal DPI message\n";
            return 1;
        }
        static_cast<void>(PostMessageW(settings, WM_COMMAND, 2U, 0U));
        for (int attempt = 0; attempt < 120 && IsWindow(settings); ++attempt) Sleep(25U);
        if (IsWindow(settings)) {
            std::cerr << "FAIL: packaged Settings window did not close after hook-boundary validation\n";
            return 1;
        }
        std::cout << "OK: packaged app and Settings expose no internal UI test messages\n";
        return 0;
    }
    if (SendMessageW(window, hardwarescope::kDisableAutomaticUpdateTestMessage, 0U, 0) != 1) {
        std::cerr << "FAIL: internal update-isolation test hook is unavailable\n";
        return 1;
    }

    RECT before{};
    if (!GetWindowRect(window, &before)) {
        std::cerr << "FAIL: could not read native window bounds\n";
        return 1;
    }

    const auto start_x = before.left + 300;
    const auto start_y = before.top + 42;
    const auto caption_result = SendMessageW(window, WM_NCHITTEST, 0, ScreenPointParameter(start_x, start_y));
    const auto controls_result = SendMessageW(window, WM_NCHITTEST, 0, ScreenPointParameter(before.right - 20, start_y));
    const auto lower_controls_result = SendMessageW(window, WM_NCHITTEST, 0, ScreenPointParameter(before.right - 20, before.top + 62));
    const auto content_result = SendMessageW(window, WM_NCHITTEST, 0, ScreenPointParameter(start_x, before.top + 200));
    if (caption_result != HTCAPTION || controls_result != HTCLIENT || lower_controls_result != HTCLIENT || content_result != HTCLIENT) {
        std::cerr << "FAIL: hit regions were caption=" << caption_result
                  << ", controls=" << controls_result
                  << ", content=" << content_result << '\n';
        return 1;
    }

    std::cout << "OK: full-header hit region is delegated to the native DWM caption path\n";

    const auto original_dpi = (std::max)(96U, GetDpiForWindow(window));
    const auto test_dpi = (std::min)(384U, original_dpi + 48U);
    if (SendMessageW(window, hardwarescope::kApplyMainDpiTestMessage, test_dpi, 0U) != static_cast<LRESULT>(test_dpi)) {
        std::cerr << "FAIL: main window did not apply an internal per-monitor DPI transition\n";
        return 1;
    }
    static_cast<void>(SendMessageW(window, WM_PAINT, 0U, 0));
    if (SendMessageW(window, hardwarescope::kApplyMainDpiTestMessage, original_dpi, 0U) != static_cast<LRESULT>(original_dpi)) {
        std::cerr << "FAIL: main window did not restore its original DPI\n";
        return 1;
    }
    static_cast<void>(SendMessageW(window, WM_DISPLAYCHANGE, 32U, 0U));
    std::cout << "OK: main render resources survive DPI and display topology changes\n";

    MonitorCollection monitors{};
    static_cast<void>(EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor, reinterpret_cast<LPARAM>(&monitors)));
    if (monitors.count >= 2U) {
        UINT minimum_dpi = UINT_MAX;
        UINT maximum_dpi{};
        for (std::size_t index = 0U; index < monitors.count; ++index) {
            const auto& work = monitors.work_areas[index];
            if (!SetWindowPos(window, nullptr, work.left + 48, work.top + 48, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
                std::cerr << "FAIL: could not move the app to real monitor " << index + 1U << '\n';
                return 1;
            }
            Sleep(300U);
            DWORD_PTR response{};
            if (!SendMessageTimeoutW(window, WM_NULL, 0U, 0, SMTO_ABORTIFHUNG, 1'000U, &response)) {
                std::cerr << "FAIL: app stopped responding after a real monitor transition\n";
                return 1;
            }
            const auto monitor_dpi = GetDpiForWindow(window);
            minimum_dpi = (std::min)(minimum_dpi, monitor_dpi);
            maximum_dpi = (std::max)(maximum_dpi, monitor_dpi);
            static_cast<void>(SendMessageW(window, WM_PAINT, 0U, 0));
        }
        static_cast<void>(SetWindowPos(
            window,
            nullptr,
            before.left,
            before.top,
            before.right - before.left,
            before.bottom - before.top,
            SWP_NOZORDER | SWP_NOACTIVATE));
        Sleep(300U);
        if (minimum_dpi < maximum_dpi) {
            std::cout << "OK: real mixed-DPI monitor transitions stayed responsive (" << minimum_dpi
                      << " -> " << maximum_dpi << " DPI)\n";
        } else {
            std::cout << "SKIP: multiple real monitors use the same " << minimum_dpi << " DPI\n";
        }
    } else {
        std::cout << "SKIP: real multi-monitor transition requires at least two active displays\n";
    }

    if (!skip_hover) {
        if (SendMessageW(window, hardwarescope::kArmTooltipTestMessage, 0U, 0) != 1) {
            std::cerr << "FAIL: internal tooltip test hook is unavailable\n";
            return 1;
        }
        if (SendMessageW(window, hardwarescope::kQueryTooltipVisibleMessage, 0U, 0) != 0) {
            std::cerr << "FAIL: column hover help appeared without the one-second delay\n";
            return 1;
        }
        Sleep(900U);
        if (SendMessageW(window, hardwarescope::kQueryTooltipVisibleMessage, 0U, 0) != 0) {
            std::cerr << "FAIL: column hover help appeared before the one-second delay\n";
            return 1;
        }
        bool tooltip_visible{};
        for (int attempt = 0; attempt < 40 && !tooltip_visible; ++attempt) {
            Sleep(50U);
            tooltip_visible = SendMessageW(window, hardwarescope::kQueryTooltipVisibleMessage, 0U, 0) == 1;
        }
        if (!tooltip_visible) {
            std::cerr << "FAIL: one-second column hover did not show native help\n";
            return 1;
        }
        static_cast<void>(SendMessageW(window, WM_MOUSELEAVE, 0U, 0));
        if (SendMessageW(window, hardwarescope::kQueryTooltipVisibleMessage, 0U, 0) != 0) {
            std::cerr << "FAIL: native hover help did not clear when the pointer left\n";
            return 1;
        }
        std::cout << "OK: native one-second hover explanations show and clear without child controls\n";
    } else {
        std::cout << "SKIP: physical pointer activity makes the hover-delay check nondeterministic\n";
    }

    if (SendMessageW(window, hardwarescope::kQueueAutomaticUpdateNotificationTestMessage, 0U, 0U) != 1) {
        std::cerr << "FAIL: automatic update notification could not be queued through the tray\n";
        return 1;
    }
    Sleep(150U);
    if (FindWindowW(L"#32770", L"HardwareScope update") != nullptr) {
        std::cerr << "FAIL: automatic update notification opened an intrusive modal dialog\n";
        return 1;
    }
    std::cout << "OK: automatic updates use a quiet tray notification without stealing focus\n";

    if (!skip_update_prompt) {
        static_cast<void>(PostMessageW(window, hardwarescope::kShowUpdatePromptTestMessage, 0U, 0U));
        HWND update_prompt{};
        for (int attempt = 0; attempt < 100 && update_prompt == nullptr; ++attempt) {
            Sleep(25U);
            update_prompt = FindWindowW(hardwarescope::kUpdatePromptWindowClass, L"HardwareScope update");
        }
        if (update_prompt == nullptr
            || SendMessageW(update_prompt, hardwarescope::kQueryUpdatePromptChoicesMessage, 0U, 0U) != 0x3F) {
            std::cerr << "FAIL: themed update prompt is missing its install or reminder choices\n";
            return 1;
        }
        static_cast<void>(SendMessageW(update_prompt, WM_COMMAND, hardwarescope::kUpdateIn1WeekRadio, 0U));
        if (SendMessageW(update_prompt, hardwarescope::kQueryUpdatePromptSelectionMessage, 0U, 0U)
            != hardwarescope::kUpdateIn1WeekRadio) {
            std::cerr << "FAIL: themed update prompt did not select the one-week reminder\n";
            return 1;
        }
        static_cast<void>(SendMessageW(update_prompt, WM_COMMAND, hardwarescope::kUpdateLaterButton, 0U));
        for (int attempt = 0; attempt < 100 && IsWindow(update_prompt); ++attempt) Sleep(25U);
        if (IsWindow(update_prompt)) {
            std::cerr << "FAIL: update reminder choice did not close the prompt\n";
            return 1;
        }
        std::cout << "OK: themed in-app updater offers update now, 24 hours, 3 days, 1 week, or never for that version\n";
    } else {
        std::cout << "SKIP: update prompt inspection is unavailable with this Windows dialog host\n";
    }

    for (int paint = 0; paint < 160; ++paint) static_cast<void>(SendMessageW(window, WM_PAINT, 0U, 0));
    const auto paint_p95 = static_cast<unsigned long long>(SendMessageW(window, hardwarescope::kQueryPaintP95Message, 0U, 0));
    if (paint_p95 == 0U || paint_p95 > 4'000U) {
        std::cerr << "FAIL: native UI p95 paint time is outside the 4 ms budget: " << paint_p95 << " us\n";
        return 1;
    }
    std::cout << "OK: native UI p95 paint time is " << paint_p95 << " us\n";

    static_cast<void>(SendMessageW(window, WM_SYSCOMMAND, SC_MINIMIZE, 0));
    Sleep(100);
    if (IsWindowVisible(window)) {
        std::cerr << "FAIL: minimize-to-tray left the main window visible\n";
        return 1;
    }
    static_cast<void>(SendMessageW(window, WM_COMMAND, hardwarescope::kCommandOpen, 0));
    Sleep(100);
    if (!IsWindowVisible(window)) {
        std::cerr << "FAIL: tray open command did not restore the main window\n";
        return 1;
    }
    std::cout << "OK: minimize-to-tray hides the taskbar window and tray open restores it\n";

    DWORD application_process_id{};
    static_cast<void>(GetWindowThreadProcessId(window, &application_process_id));
    const auto osd = ProcessWindow(kOsdWindowClass, nullptr, application_process_id);
    if (osd == nullptr || !IsWindowVisible(osd)) {
        std::cerr << "FAIL: default native OSD is not visible after a sensor snapshot\n";
        return 1;
    }
    RECT osd_bounds{};
    static_cast<void>(GetWindowRect(osd, &osd_bounds));
    const auto extended_style = static_cast<DWORD>(GetWindowLongPtrW(osd, GWL_EXSTYLE));
    const auto osd_hit = SendMessageW(osd, WM_NCHITTEST, 0, ScreenPointParameter(osd_bounds.left + 2, osd_bounds.top + 2));
    if (osd_bounds.right - osd_bounds.left <= 20 || osd_bounds.bottom - osd_bounds.top <= 10
        || (extended_style & (WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != (WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)
        || (extended_style & WS_EX_APPWINDOW) != 0U || osd_hit != HTTRANSPARENT) {
        std::cerr << "FAIL: native OSD bounds or click-through window styles are incorrect\n";
        return 1;
    }
    std::cout << "OK: native OSD is compact, taskbar-free, non-activating, and click-through\n";

    if (SendMessageW(window, hardwarescope::kConfigureOsdGraphTestMessage, 1U, 0U) != 1) {
        std::cerr << "FAIL: internal live-graph test hook could not select a sensor\n";
        return 1;
    }
    Sleep(2'600U);
    RECT graph_bounds{};
    static_cast<void>(GetWindowRect(osd, &graph_bounds));
    if (!IsWindowVisible(osd) || graph_bounds.bottom - graph_bounds.top < osd_bounds.bottom - osd_bounds.top + 30) {
        std::cerr << "FAIL: live OSD graph did not create a visible fixed-history plot\n";
        return 1;
    }
    static_cast<void>(SendMessageW(window, hardwarescope::kConfigureOsdGraphTestMessage, 0U, 0U));
    std::cout << "OK: live OSD graph renders from a fixed sensor-history buffer\n";

    if (SendMessageW(window, hardwarescope::kConfigureFloatingGraphTestMessage, 1U, 0U) != 1) {
        std::cerr << "FAIL: internal floating-graph test hook could not select a sensor\n";
        return 1;
    }
    Sleep(250U);
    const auto graph_window = ProcessWindow(kGraphWindowClass, nullptr, application_process_id);
    RECT floating_bounds{};
    if (graph_window == nullptr || !IsWindowVisible(graph_window)
        || !GetWindowRect(graph_window, &floating_bounds)
        || floating_bounds.right - floating_bounds.left < 360
        || floating_bounds.bottom - floating_bounds.top < 220) {
        std::cerr << "FAIL: detachable graph window was not visible or correctly sized\n";
        return 1;
    }
    InvalidateRect(graph_window, nullptr, FALSE);
    UpdateWindow(graph_window);
    static_cast<void>(SendMessageW(window, hardwarescope::kConfigureFloatingGraphTestMessage, 0U, 0U));
    if (IsWindowVisible(graph_window)) {
        std::cerr << "FAIL: disabling the detachable graph window did not hide it\n";
        return 1;
    }
    std::cout << "OK: detachable native graph window opens, paints, sizes, and hides\n";

    if (SendMessageW(window, hardwarescope::kRestoreTrayIconTestMessage, 0U, 0U) != 1
        || SendMessageW(window, hardwarescope::kQueryTrayIconAddedMessage, 0U, 0U) != 1) {
        std::cerr << "FAIL: notification icon could not be restored after a simulated Explorer restart\n";
        return 1;
    }
    std::cout << "OK: tray icon restoration path re-adds the notification icon\n";
    const auto foreground_before_panel = GetForegroundWindow();
    if (SendMessageW(window, hardwarescope::kToggleTrayPanelTestMessage, 0U, 0U) != 1) {
        std::cerr << "FAIL: tray quick panel did not open\n";
        return 1;
    }
    const auto tray_panel = FindWindowW(hardwarescope::TrayPanelWindow::kWindowClass, L"HardwareScope quick view");
    if (tray_panel == nullptr || !IsWindowVisible(tray_panel)
        || SendMessageW(window, hardwarescope::kQueryTrayPanelVisibleMessage, 0U, 0U) != 1
        || GetForegroundWindow() != foreground_before_panel) {
        std::cerr << "FAIL: tray quick panel is missing, hidden, or stole focus\n";
        return 1;
    }
    if (SendMessageW(window, hardwarescope::kToggleTrayPanelTestMessage, 0U, 0U) != 0
        || IsWindowVisible(tray_panel)) {
        std::cerr << "FAIL: tray quick panel did not close on the second toggle\n";
        return 1;
    }
    std::cout << "OK: tray quick panel toggles without activating or opening the full app\n";

    if (SendMessageW(window, WM_POWERBROADCAST, PBT_APMSUSPEND, 0U) != TRUE
        || IsWindowVisible(osd)) {
        std::cerr << "FAIL: suspend did not acknowledge the asynchronous pause and hide the OSD\n";
        return 1;
    }
    if (SendMessageW(window, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0U) != TRUE) {
        std::cerr << "FAIL: automatic resume message was rejected\n";
        return 1;
    }
    bool resumed{};
    for (int attempt = 0; attempt < 120 && !resumed; ++attempt) {
        Sleep(50U);
        resumed = SendMessageW(window, hardwarescope::kQuerySensorWorkerRunningMessage, 0U, 0U) == 1
            && SendMessageW(window, hardwarescope::kQueryResumeWaitingMessage, 0U, 0U) == 0
            && IsWindowVisible(osd);
    }
    if (!resumed) {
        std::cerr << "FAIL: resume did not publish a fresh snapshot and restore the OSD\n";
        return 1;
    }
    std::cout << "OK: suspend requests a nonblocking pause; resume waits for fresh telemetry before restoring OSD\n";

    static_cast<void>(PostMessageW(window, WM_COMMAND, hardwarescope::kCommandSettings, 0));
    HWND settings{};
    bool modal_ready{};
    for (int attempt = 0; attempt < 100 && !modal_ready; ++attempt) {
        Sleep(50U);
        settings = ProcessWindow(kSettingsWindowClass, nullptr, application_process_id);
        modal_ready = settings != nullptr && IsWindowVisible(settings) && !IsWindowEnabled(window);
    }
    if (!modal_ready) {
        std::cerr << "FAIL: cogwheel did not open a modal native settings window\n";
        return 1;
    }
    const auto tabs = FindWindowExW(settings, nullptr, WC_TABCONTROLW, nullptr);
    const auto first_combo = FindWindowExW(settings, nullptr, WC_COMBOBOXW, nullptr);
    const auto sensor_list = GetDlgItem(settings, hardwarescope::kSettingsSensorListControl);
    const auto update_button = GetDlgItem(settings, hardwarescope::kSettingsCheckUpdatesCommand);
    const auto export_button = GetDlgItem(settings, hardwarescope::kSettingsExportCommand);
    const auto import_button = GetDlgItem(settings, hardwarescope::kSettingsImportCommand);
    if (tabs == nullptr || first_combo == nullptr || sensor_list == nullptr || update_button == nullptr
        || export_button == nullptr || import_button == nullptr || TabCtrl_GetItemCount(tabs) != 5
        || (GetWindowLongPtrW(settings, GWL_STYLE) & WS_CLIPCHILDREN) == 0
        || (GetWindowLongPtrW(tabs, GWL_STYLE) & TCS_OWNERDRAWFIXED) == 0
        || (GetWindowLongPtrW(first_combo, GWL_STYLE) & CBS_OWNERDRAWFIXED) == 0
        || (GetWindowLongPtrW(sensor_list, GWL_STYLE) & LBS_OWNERDRAWFIXED) == 0
        || (GetWindowLongPtrW(update_button, GWL_STYLE) & BS_OWNERDRAW) == 0
        || (GetWindowLongPtrW(export_button, GWL_STYLE) & BS_OWNERDRAW) == 0
        || (GetWindowLongPtrW(import_button, GWL_STYLE) & BS_OWNERDRAW) == 0) {
        std::cerr << "FAIL: settings pages, import/export, or configurable OSD sensor list are missing\n";
        return 1;
    }
    RECT tab_client{};
    if (!GetClientRect(tabs, &tab_client) || tab_client.right <= tab_client.left || tab_client.bottom <= tab_client.top) {
        std::cerr << "FAIL: Settings tab geometry is unavailable\n";
        return 1;
    }
    if (SendMessageW(settings, hardwarescope::kSelectSettingsTabTestMessage, 2U, 0U) != 3) {
        std::cerr << "FAIL: internal Settings tab selection hook is unavailable\n";
        return 1;
    }
    if (TabCtrl_GetCurSel(tabs) != 2 || !IsWindowVisible(sensor_list)) {
        std::cerr << "FAIL: Monitoring tab did not expose the Additional OSD Sensors list (tab="
                  << TabCtrl_GetCurSel(tabs) << ", list=" << sensor_list
                  << ", visible=" << (sensor_list != nullptr && IsWindowVisible(sensor_list)) << ")\n";
        return 1;
    }
    RECT default_client{};
    SCROLLINFO default_vertical{sizeof(default_vertical), SIF_ALL};
    static_cast<void>(GetClientRect(settings, &default_client));
    static_cast<void>(GetScrollInfo(settings, SB_VERT, &default_vertical));
    const auto default_content_height = MulDiv(650, static_cast<int>((std::max)(96U, GetDpiForWindow(settings))), 96);
    if (default_client.bottom - default_client.top >= default_content_height
        && default_vertical.nMax - default_vertical.nMin + 1 > static_cast<int>(default_vertical.nPage)) {
        std::cerr << "FAIL: normally sized Settings exposes unnecessary outer-page scrolling\n";
        return 1;
    }
    RECT settings_before{};
    RECT tabs_before{};
    static_cast<void>(GetWindowRect(settings, &settings_before));
    static_cast<void>(GetWindowRect(tabs, &tabs_before));
    const auto settings_dpi = (std::max)(96U, GetDpiForWindow(settings));
    const auto settings_test_dpi = (std::min)(384U, settings_dpi + 48U);
    if (SendMessageW(settings, hardwarescope::kApplySettingsDpiTestMessage, settings_test_dpi, 0U) != static_cast<LRESULT>(settings_test_dpi)) {
        std::cerr << "FAIL: Settings did not apply an internal per-monitor DPI transition\n";
        return 1;
    }
    RECT tabs_scaled{};
    static_cast<void>(GetWindowRect(tabs, &tabs_scaled));
    const auto expected_tab_width = MulDiv(tabs_before.right - tabs_before.left, static_cast<int>(settings_test_dpi), static_cast<int>(settings_dpi));
    if (std::abs((tabs_scaled.right - tabs_scaled.left) - expected_tab_width) > 3) {
        std::cerr << "FAIL: Settings controls were not rescaled with the monitor DPI\n";
        return 1;
    }
    static_cast<void>(SetWindowPos(settings, nullptr, settings_before.left, settings_before.top, 600, 450, SWP_NOACTIVATE | SWP_NOZORDER));
    const auto sensor_top_before = SendMessageW(sensor_list, LB_GETTOPINDEX, 0U, 0U);
    const auto monitoring_scroll_started = GetTickCount64();
    for (int step = 0; step < 12; ++step) {
        static_cast<void>(SendMessageW(settings, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0U));
    }
    if (GetTickCount64() - monitoring_scroll_started > 1'000U || !IsWindowVisible(sensor_list)
        || SendMessageW(sensor_list, LB_GETTOPINDEX, 0U, 0U) != sensor_top_before) {
        std::cerr << "FAIL: Monitoring page scrolling stalled or scrolled inside its OSD sensor list\n";
        return 1;
    }
    SCROLLINFO vertical{sizeof(vertical), SIF_ALL};
    SCROLLINFO horizontal{sizeof(horizontal), SIF_ALL};
    if (!GetScrollInfo(settings, SB_VERT, &vertical) || !GetScrollInfo(settings, SB_HORZ, &horizontal)
        || vertical.nMax - vertical.nMin + 1 <= static_cast<int>(vertical.nPage)
        || horizontal.nMax - horizontal.nMin + 1 <= static_cast<int>(horizontal.nPage)) {
        std::cerr << "FAIL: compact Settings window did not expose both scrollable dimensions\n";
        return 1;
    }
    static_cast<void>(SendMessageW(settings, WM_VSCROLL, MAKEWPARAM(SB_PAGEDOWN, 0), 0U));
    vertical.fMask = SIF_POS;
    static_cast<void>(GetScrollInfo(settings, SB_VERT, &vertical));
    if (vertical.nPos == 0) {
        std::cerr << "FAIL: high-DPI Settings content did not scroll\n";
        return 1;
    }
    if (SendMessageW(settings, hardwarescope::kApplySettingsDpiTestMessage, settings_dpi, 0U) != static_cast<LRESULT>(settings_dpi)) {
        std::cerr << "FAIL: Settings did not restore its original DPI\n";
        return 1;
    }
    static_cast<void>(SetWindowPos(
        settings,
        nullptr,
        settings_before.left,
        settings_before.top,
        settings_before.right - settings_before.left,
        settings_before.bottom - settings_before.top,
        SWP_NOACTIVATE | SWP_NOZORDER));
    std::cout << "OK: Settings rescales fonts/controls and scrolls on constrained high-DPI displays\n";
    static_cast<void>(PostMessageW(settings, WM_COMMAND, 2U, 0));
    for (int attempt = 0; attempt < 30 && IsWindow(settings); ++attempt) Sleep(50U);
    if (IsWindow(settings) || !IsWindowEnabled(window)) {
        std::cerr << "FAIL: cancel did not close settings and restore the main window\n";
        return 1;
    }
    std::cout << "OK: cogwheel opens native tabbed settings with configurable OSD sensors\n";

    DWORD process_id{};
    static_cast<void>(GetWindowThreadProcessId(window, &process_id));
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        std::cerr << "FAIL: could not inspect native UI resources\n";
        return 1;
    }
    const auto initial_gdi = GetGuiResources(process, GR_GDIOBJECTS);
    const auto initial_user = GetGuiResources(process, GR_USEROBJECTS);
    for (int repetition = 0; repetition < 5; ++repetition) {
        const auto open_started = GetTickCount64();
        static_cast<void>(PostMessageW(window, WM_COMMAND, hardwarescope::kCommandSettings, 0));
        settings = nullptr;
        for (int attempt = 0; attempt < 400 && (settings == nullptr || !IsWindowVisible(settings)); ++attempt) {
            Sleep(25U);
            settings = ProcessWindow(kSettingsWindowClass, nullptr, application_process_id);
        }
        if (settings == nullptr || !IsWindowVisible(settings)) {
            CloseHandle(process);
            std::cerr << "FAIL: repeated settings open failed\n";
            return 1;
        }
        std::cout << "Settings repetition " << repetition + 1 << " opened in " << GetTickCount64() - open_started << " ms\n";
        static_cast<void>(PostMessageW(settings, WM_COMMAND, 2U, 0));
        for (int attempt = 0; attempt < 100 && (IsWindow(settings) || !IsWindowEnabled(window)); ++attempt) Sleep(25U);
        if (IsWindow(settings) || !IsWindowEnabled(window)) {
            CloseHandle(process);
            std::cerr << "FAIL: repeated settings close did not restore the owner\n";
            return 1;
        }
    }
    auto final_gdi = GetGuiResources(process, GR_GDIOBJECTS);
    auto final_user = GetGuiResources(process, GR_USEROBJECTS);
    for (int attempt = 0; attempt < 60 && (final_gdi > initial_gdi + 2U || final_user > initial_user + 2U); ++attempt) {
        Sleep(50U);
        final_gdi = GetGuiResources(process, GR_GDIOBJECTS);
        final_user = GetGuiResources(process, GR_USEROBJECTS);
    }
    CloseHandle(process);
    std::cout << "Settings resource counts GDI " << initial_gdi << " -> " << final_gdi
              << ", USER " << initial_user << " -> " << final_user << '\n';
    if (final_gdi > initial_gdi + 2U || final_user > initial_user + 2U) {
        std::cerr << "FAIL: repeated settings windows leaked GDI or USER objects\n";
        return 1;
    }
    std::cout << "OK: repeated settings windows have stable GDI and USER object counts\n";
    std::cout << "PASS: HardwareScope native UI smoke suite\n";
    return 0;
}
