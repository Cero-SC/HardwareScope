#include "hardwarescope/settings_window.hpp"
#include "hardwarescope/app_commands.hpp"
#include "hardwarescope/osd_model.hpp"
#include "hardwarescope/ui_palette.hpp"
#include "hardwarescope/update_coordinator.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <atomic>
#include <memory>
#include <thread>

namespace hardwarescope {
namespace {

constexpr wchar_t kClassName[] = L"HardwareScope.Native.SettingsWindow";
constexpr int kSave = 1;
constexpr int kCancel = 2;
constexpr int kTabs = 10;
constexpr int kLogicalDialogWidth = 710;
constexpr int kLogicalDialogHeight = 690;
constexpr int kLogicalContentWidth = 650;
constexpr int kLogicalContentHeight = 650;
constexpr int kOsdPreviewControl = 21;
constexpr int kOsdOrderListControl = 22;
constexpr UINT_PTR kSettingsTransferTimer = 0x48535452U;

struct SettingsTransfer final {
    AppSettings settings;
    bool exporting{};
    bool success{};
    std::atomic<bool> done{};
};

COLORREF WinColor(const std::uint32_t rgb) noexcept {
    return RGB((rgb >> 16U) & 0xFFU, (rgb >> 8U) & 0xFFU, rgb & 0xFFU);
}

struct DialogState final {
    struct ControlRecord final {
        HWND window{};
        int page{};
        int x{};
        int y{};
        int width{};
        int height{};
    };

    HWND owner{};
    HWND window{};
    AppSettings draft{};
    const SensorSnapshot* snapshot{};
    HFONT font{};
    HBRUSH background_brush{};
    HBRUSH field_brush{};
    HBRUSH surface_brush{};
    HBRUSH hover_brush{};
    HBRUSH selection_brush{};
    HBRUSH line_brush{};
    HBRUSH accent_brush{};
    UiPalette palette{};
    bool done{};
    bool saved{};
    UINT dpi{96U};
    int scroll_x{};
    int scroll_y{};
    bool relayout_active{};
    std::vector<ControlRecord> paged_controls;
    std::vector<HWND> push_buttons;

    HWND tabs{};
    HWND theme{};
    HWND color{};
    HWND refresh{};
    HWND text_scale{};
    HWND high_contrast{};
    HWND start_windows{};
    HWND start_minimized{};
    HWND reset_on_startup{};
    HWND reset_on_game_launch{};
    HWND reset_interval{};
    HWND updates{};
    HWND check_updates{};
    HWND export_settings{};
    HWND import_settings{};
    std::shared_ptr<SettingsTransfer> transfer;
    HWND show_osd{};
    HWND position{};
    HWND layout{};
    HWND opacity{};
    HWND scale{};
    HWND spacing{};
    HWND separators{};
    HWND background{};
    HWND fps_enabled{};
    HWND fps_game_only{};
    HWND fps_separate_position{};
    HWND fps_position{};
    HWND fps_refresh{};
    HWND fps_smoothing{};
    HWND fps_color{};
    HWND fps_scale{};
    HWND fps_one_percent_low{};
    HWND graph_enabled{};
    HWND graph_sources{};
    HWND graph_scale_mode{};
    HWND graph_custom_minimum{};
    HWND graph_custom_maximum{};
    HWND graph_history{};
    HWND graph_refresh{};
    HWND graph_width{};
    HWND graph_height{};
    HWND graph_line_thickness{};
    HWND graph_grid{};
    HWND graph_labels{};
    HWND floating_graph{};
    HWND floating_graph_topmost{};
    HWND sensor_list{};
    HWND osd_preview{};
    HWND osd_order_list{};
    int osd_drag_index{-1};
    std::array<HWND, 8U> section_colors{};
    std::array<HWND, AppSettings::kMaximumGraphSensors> graph_colors{};
};

void SyncOsdOrderFromList(DialogState& state) noexcept;

LRESULT CALLBACK OsdOrderListWindowProcedure(
    const HWND control,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR subclass_id,
    const DWORD_PTR reference) noexcept {
    auto& state = *reinterpret_cast<DialogState*>(reference);
    if (message == WM_LBUTTONDOWN) {
        const auto hit = static_cast<DWORD>(SendMessageW(control, LB_ITEMFROMPOINT, 0, lparam));
        if (HIWORD(hit) == 0U) {
            state.osd_drag_index = LOWORD(hit);
            SetCapture(control);
        }
    } else if (message == WM_MOUSEMOVE && GetCapture() == control && state.osd_drag_index >= 0) {
        const auto hit = static_cast<DWORD>(SendMessageW(control, LB_ITEMFROMPOINT, 0, lparam));
        const auto target = HIWORD(hit) == 0U ? static_cast<int>(LOWORD(hit)) : state.osd_drag_index;
        if (target != state.osd_drag_index) {
            std::array<wchar_t, 256U> text{};
            SendMessageW(control, LB_GETTEXT, state.osd_drag_index, reinterpret_cast<LPARAM>(text.data()));
            const auto id = SendMessageW(control, LB_GETITEMDATA, state.osd_drag_index, 0);
            SendMessageW(control, LB_DELETESTRING, state.osd_drag_index, 0);
            const auto inserted = static_cast<int>(SendMessageW(control, LB_INSERTSTRING, target, reinterpret_cast<LPARAM>(text.data())));
            SendMessageW(control, LB_SETITEMDATA, inserted, id);
            SendMessageW(control, LB_SETCURSEL, inserted, 0);
            state.osd_drag_index = inserted;
            SyncOsdOrderFromList(state);
            if (state.osd_preview != nullptr) InvalidateRect(state.osd_preview, nullptr, TRUE);
        }
        return 0;
    } else if (message == WM_LBUTTONUP && GetCapture() == control) {
        ReleaseCapture();
        state.osd_drag_index = -1;
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, &OsdOrderListWindowProcedure, subclass_id);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

int Scale(const DialogState& state, const int value) noexcept {
    return MulDiv(value, static_cast<int>(state.dpi), 96);
}

void StyleControl(const DialogState& state, const HWND control) noexcept {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    static_cast<void>(SetWindowTheme(control, state.draft.theme != Theme::light ? L"DarkMode_Explorer" : L"Explorer", nullptr));
}

LRESULT CALLBACK CheckboxWindowProcedure(
    const HWND control,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR subclass_id,
    const DWORD_PTR reference_data) noexcept {
    auto* const state = reinterpret_cast<DialogState*>(reference_data);
    if (state == nullptr) return DefSubclassProc(control, message, wparam, lparam);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        const auto dc = message == WM_PAINT ? BeginPaint(control, &paint) : reinterpret_cast<HDC>(wparam);
        if (dc != nullptr) {
            RECT bounds{};
            GetClientRect(control, &bounds);
            FillRect(dc, &bounds, state->background_brush);

            const auto box_size = Scale(*state, 16);
            RECT box{0, (bounds.bottom - box_size) / 2, box_size, (bounds.bottom + box_size) / 2};
            FillRect(dc, &box, state->field_brush);
            FrameRect(dc, &box, GetFocus() == control ? state->accent_brush : state->line_brush);
            if (SendMessageW(control, BM_GETCHECK, 0U, 0U) == BST_CHECKED) {
                const auto pen = CreatePen(PS_SOLID, std::max(1, Scale(*state, 2)), WinColor(state->palette.accent));
                const auto previous_pen = SelectObject(dc, pen);
                MoveToEx(dc, box.left + Scale(*state, 3), box.top + Scale(*state, 8), nullptr);
                LineTo(dc, box.left + Scale(*state, 7), box.bottom - Scale(*state, 3));
                LineTo(dc, box.right - Scale(*state, 2), box.top + Scale(*state, 3));
                SelectObject(dc, previous_pen);
                DeleteObject(pen);
            }

            std::array<wchar_t, 256U> text{};
            GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
            auto text_bounds = bounds;
            text_bounds.left = box.right + Scale(*state, 10);
            const auto previous_font = SelectObject(dc, state->font);
            const auto previous_mode = SetBkMode(dc, TRANSPARENT);
            const auto previous_color = SetTextColor(dc, WinColor(IsWindowEnabled(control) ? state->palette.text : state->palette.disabled));
            DrawTextW(dc, text.data(), -1, &text_bounds, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            SetTextColor(dc, previous_color);
            SetBkMode(dc, previous_mode);
            SelectObject(dc, previous_font);
            if (GetFocus() == control) {
                auto focus = text_bounds;
                focus.right = std::min(focus.right, focus.left + Scale(*state, 560));
                DrawFocusRect(dc, &focus);
            }
        }
        if (message == WM_PAINT) EndPaint(control, &paint);
        return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE || message == BM_SETCHECK) {
        const auto result = DefSubclassProc(control, message, wparam, lparam);
        InvalidateRect(control, nullptr, FALSE);
        return result;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, &CheckboxWindowProcedure, subclass_id);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK TabWindowProcedure(
    const HWND control,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR subclass_id,
    const DWORD_PTR reference_data) noexcept {
    auto* const state = reinterpret_cast<DialogState*>(reference_data);
    if (state == nullptr) return DefSubclassProc(control, message, wparam, lparam);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        const auto dc = message == WM_PAINT ? BeginPaint(control, &paint) : reinterpret_cast<HDC>(wparam);
        if (dc != nullptr) {
            RECT bounds{};
            GetClientRect(control, &bounds);
            FillRect(dc, &bounds, state->background_brush);
            const auto previous_font = SelectObject(dc, state->font);
            const auto previous_mode = SetBkMode(dc, TRANSPARENT);
            const auto selected = TabCtrl_GetCurSel(control);
            const auto count = TabCtrl_GetItemCount(control);
            for (int index = 0; index < count; ++index) {
                RECT item{};
                if (!TabCtrl_GetItemRect(control, index, &item)) continue;
                FillRect(dc, &item, index == selected ? state->surface_brush : state->field_brush);
                FrameRect(dc, &item, state->line_brush);
                if (index == selected) {
                    auto accent = item;
                    accent.top = accent.bottom - Scale(*state, 3);
                    FillRect(dc, &accent, state->accent_brush);
                }
                std::array<wchar_t, 128U> text{};
                TCITEMW tab{};
                tab.mask = TCIF_TEXT;
                tab.pszText = text.data();
                tab.cchTextMax = static_cast<int>(text.size());
                static_cast<void>(TabCtrl_GetItem(control, index, &tab));
                SetTextColor(dc, WinColor(index == selected ? state->palette.accent : state->palette.muted));
                DrawTextW(dc, text.data(), -1, &item, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
            }
            SetBkMode(dc, previous_mode);
            SelectObject(dc, previous_font);
        }
        if (message == WM_PAINT) EndPaint(control, &paint);
        return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE) {
        const auto result = DefSubclassProc(control, message, wparam, lparam);
        InvalidateRect(control, nullptr, FALSE);
        return result;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(control, &TabWindowProcedure, subclass_id);
    return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK ComboWindowProcedure(
    const HWND control,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR subclass_id,
    const DWORD_PTR reference_data) noexcept {
    auto* const state = reinterpret_cast<DialogState*>(reference_data);
    if (state == nullptr) return DefSubclassProc(control, message, wparam, lparam);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        const auto dc = message == WM_PAINT ? BeginPaint(control, &paint) : reinterpret_cast<HDC>(wparam);
        if (dc != nullptr) {
            RECT bounds{};
            GetClientRect(control, &bounds);
            FillRect(dc, &bounds, state->field_brush);
            FrameRect(dc, &bounds, GetFocus() == control ? state->accent_brush : state->line_brush);

            const auto arrow_width = Scale(*state, 30);
            RECT arrow{std::max(bounds.left, bounds.right - arrow_width), bounds.top + 1, bounds.right - 1, bounds.bottom - 1};
            FillRect(dc, &arrow, SendMessageW(control, CB_GETDROPPEDSTATE, 0U, 0U) != 0 ? state->selection_brush : state->surface_brush);
            RECT separator{arrow.left, arrow.top, arrow.left + 1, arrow.bottom};
            FillRect(dc, &separator, state->line_brush);

            const auto center_x = (arrow.left + arrow.right) / 2;
            const auto center_y = (arrow.top + arrow.bottom) / 2;
            const auto pen = CreatePen(PS_SOLID, std::max(1, Scale(*state, 2)), WinColor(state->palette.accent));
            const auto previous_pen = SelectObject(dc, pen);
            MoveToEx(dc, center_x - Scale(*state, 5), center_y - Scale(*state, 2), nullptr);
            LineTo(dc, center_x, center_y + Scale(*state, 3));
            LineTo(dc, center_x + Scale(*state, 5), center_y - Scale(*state, 2));
            SelectObject(dc, previous_pen);
            DeleteObject(pen);

            std::array<wchar_t, 256U> text{};
            GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
            auto text_bounds = bounds;
            text_bounds.left += Scale(*state, 10);
            text_bounds.right = arrow.left - Scale(*state, 6);
            const auto previous_font = SelectObject(dc, state->font);
            const auto previous_mode = SetBkMode(dc, TRANSPARENT);
            const auto previous_color = SetTextColor(dc, WinColor(IsWindowEnabled(control) ? state->palette.text : state->palette.disabled));
            DrawTextW(dc, text.data(), -1, &text_bounds, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            SetTextColor(dc, previous_color);
            SetBkMode(dc, previous_mode);
            SelectObject(dc, previous_font);
        }
        if (message == WM_PAINT) EndPaint(control, &paint);
        return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE || message == CB_SETCURSEL) {
        const auto result = DefSubclassProc(control, message, wparam, lparam);
        InvalidateRect(control, nullptr, FALSE);
        return result;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(control, &ComboWindowProcedure, subclass_id);
    return DefSubclassProc(control, message, wparam, lparam);
}

HWND AddControl(DialogState& state, const wchar_t* const type, const wchar_t* const text, const DWORD style, const int x, const int y, const int width, const int height, const int id, const int page) {
    const auto control = CreateWindowExW(
        0U,
        type,
        text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
        Scale(state, x) - state.scroll_x,
        Scale(state, y) - state.scroll_y,
        Scale(state, width),
        Scale(state, height),
        state.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (control != nullptr) {
        StyleControl(state, control);
        state.paged_controls.push_back(DialogState::ControlRecord{control, page, x, y, width, height});
    }
    return control;
}

HWND Label(DialogState& state, const wchar_t* const text, const int y, const int page, const int x = 42, const int width = 215) {
    return AddControl(state, L"STATIC", text, SS_LEFT | SS_CENTERIMAGE, x, y, width, 28, 0, page);
}

HWND Check(DialogState& state, const wchar_t* const text, const bool checked, const int y, const int page, const int x = 42, const int width = 590) {
    const auto control = AddControl(state, L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, width, 28, 0, page);
    if (control != nullptr) static_cast<void>(SetWindowSubclass(control, &CheckboxWindowProcedure, 1U, reinterpret_cast<DWORD_PTR>(&state)));
    SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return control;
}

HWND PushButton(DialogState& state, const wchar_t* const text, const int x, const int y, const int width, const int height, const int id, const int page) {
    const auto control = AddControl(state, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, width, height, id, page);
    if (control != nullptr) state.push_buttons.push_back(control);
    return control;
}

HWND ComboAt(DialogState& state, const int x, const int y, const int width, const int page, const std::vector<std::pair<const wchar_t*, std::uint32_t>>& items, const std::uint32_t selected) {
    const auto control = AddControl(state, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_BORDER | WS_TABSTOP | WS_VSCROLL, x, y, width, 220, 0, page);
    if (control != nullptr) static_cast<void>(SetWindowSubclass(control, &ComboWindowProcedure, 1U, reinterpret_cast<DWORD_PTR>(&state)));
    SendMessageW(control, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Scale(state, 28));
    SendMessageW(control, CB_SETITEMHEIGHT, 0U, Scale(state, 28));
    int selected_index{};
    for (std::size_t index = 0U; index < items.size(); ++index) {
        const auto item = static_cast<int>(SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[index].first)));
        SendMessageW(control, CB_SETITEMDATA, item, items[index].second);
        if (items[index].second == selected) selected_index = item;
    }
    SendMessageW(control, CB_SETCURSEL, selected_index, 0);
    InvalidateRect(control, nullptr, TRUE);
    return control;
}

HWND Combo(DialogState& state, const int y, const int page, const std::vector<std::pair<const wchar_t*, std::uint32_t>>& items, const std::uint32_t selected) {
    return ComboAt(state, 270, y, 350, page, items, selected);
}

HWND NumberEdit(DialogState& state, const double value, const int x, const int y, const int width, const int page) {
    wchar_t text[48]{};
    static_cast<void>(swprintf_s(text, L"%.2f", value));
    return AddControl(state, L"EDIT", text, ES_AUTOHSCROLL | ES_RIGHT | WS_BORDER | WS_TABSTOP, x, y, width, 30, 0, page);
}

std::uint32_t ComboValue(const HWND combo, const std::uint32_t fallback) noexcept {
    const auto selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) return fallback;
    const auto value = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
    return value == CB_ERR ? fallback : static_cast<std::uint32_t>(value);
}

void SelectComboValue(const HWND combo, const std::uint32_t value) noexcept {
    const auto count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int index = 0; index < count; ++index) {
        if (static_cast<std::uint32_t>(SendMessageW(combo, CB_GETITEMDATA, index, 0)) == value) {
            SendMessageW(combo, CB_SETCURSEL, index, 0);
            return;
        }
    }
}

HWND GraphSourceList(DialogState& state, const int y, const int page) {
    const auto control = AddControl(
        state,
        L"LISTBOX",
        L"",
        LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_BORDER | WS_VSCROLL | WS_TABSTOP,
        42,
        y,
        578,
        170,
        0,
        page);
    if (control == nullptr) return nullptr;
    SendMessageW(control, LB_SETITEMHEIGHT, 0U, Scale(state, 28));
    const auto is_selected = [&](const std::uint64_t id) noexcept {
        return std::find(
            state.draft.osd_graph_sensor_ids.begin(),
            state.draft.osd_graph_sensor_ids.begin() + state.draft.osd_graph_sensor_count,
            id) != state.draft.osd_graph_sensor_ids.begin() + state.draft.osd_graph_sensor_count;
    };
    // Keep active series at the top. A long hardware inventory should never hide
    // the sensors that are already feeding the graph.
    for (const bool selected_pass : {true, false}) {
        for (std::uint32_t index{}; state.snapshot != nullptr && index < state.snapshot->count; ++index) {
            const auto& sensor = state.snapshot->sensors[index];
            const auto selected = is_selected(sensor.id);
            if (!sensor.available || selected != selected_pass) continue;
            std::wstring label = selected ? L"★  " : L"   ";
            label += sensor.name.data();
            label += L"  —  ";
            label += sensor.hardware.data();
            const auto item = SendMessageW(control, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (item == LB_ERR || item == LB_ERRSPACE) continue;
            SendMessageW(control, LB_SETITEMDATA, static_cast<WPARAM>(item), static_cast<LPARAM>(sensor.id));
            SendMessageW(control, LB_SETSEL, selected ? TRUE : FALSE, item);
        }
    }
    return control;
}

double NumberValue(const HWND edit, const double fallback) noexcept {
    std::array<wchar_t, 64U> text{};
    GetWindowTextW(edit, text.data(), static_cast<int>(text.size()));
    wchar_t* end{};
    const auto value = std::wcstod(text.data(), &end);
    return end != text.data() && std::isfinite(value) ? value : fallback;
}

const SensorValue* SnapshotSensor(const DialogState& state, const std::uint64_t id) noexcept {
    for (std::uint32_t index{}; state.snapshot != nullptr && index < state.snapshot->count; ++index) {
        if (state.snapshot->sensors[index].id == id) return &state.snapshot->sensors[index];
    }
    return nullptr;
}

void UpdateGraphControlStates(DialogState& state) noexcept {
    const auto custom = ComboValue(state.graph_scale_mode, 0U) == static_cast<std::uint32_t>(GraphScaleMode::custom);
    EnableWindow(state.graph_custom_minimum, custom ? TRUE : FALSE);
    EnableWindow(state.graph_custom_maximum, custom ? TRUE : FALSE);
    EnableWindow(state.floating_graph_topmost, SendMessageW(state.floating_graph, BM_GETCHECK, 0, 0) == BST_CHECKED ? TRUE : FALSE);
}

void EnforceGraphSelection(DialogState& state) noexcept {
    const auto caret = static_cast<int>(SendMessageW(state.graph_sources, LB_GETCARETINDEX, 0, 0));
    if (caret < 0 || SendMessageW(state.graph_sources, LB_GETSEL, caret, 0) <= 0) return;
    const auto caret_id = static_cast<std::uint64_t>(SendMessageW(state.graph_sources, LB_GETITEMDATA, caret, 0));
    const auto* caret_sensor = SnapshotSensor(state, caret_id);
    const auto count = static_cast<int>(SendMessageW(state.graph_sources, LB_GETSELCOUNT, 0, 0));
    bool invalid = count > static_cast<int>(AppSettings::kMaximumGraphSensors);
    const auto items = static_cast<int>(SendMessageW(state.graph_sources, LB_GETCOUNT, 0, 0));
    for (int item{}; !invalid && item < items; ++item) {
        if (item == caret || SendMessageW(state.graph_sources, LB_GETSEL, item, 0) <= 0) continue;
        const auto id = static_cast<std::uint64_t>(SendMessageW(state.graph_sources, LB_GETITEMDATA, item, 0));
        const auto* sensor = SnapshotSensor(state, id);
        invalid = caret_sensor != nullptr && sensor != nullptr && caret_sensor->unit != sensor->unit;
    }
    if (!invalid) return;
    SendMessageW(state.graph_sources, LB_SETSEL, FALSE, caret);
    static_cast<void>(MessageBeep(MB_ICONINFORMATION));
}

bool Checked(const HWND control) noexcept { return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED; }

std::vector<std::pair<const wchar_t*, std::uint32_t>> ColorChoices(const bool include_match_accent) {
    std::vector<std::pair<const wchar_t*, std::uint32_t>> choices;
    if (include_match_accent) choices.emplace_back(L"Match accent", AppSettings::kMatchAccentColor);
    choices.insert(choices.end(), {
        {L"Teal", 0x52E0D4U},
        {L"Cyan", 0x20C7F2U},
        {L"Blue", 0x4D8DFFU},
        {L"Purple", 0xA970FFU},
        {L"Pink", 0xFF5CA8U},
        {L"Green", 0x5BE37DU},
        {L"White", 0xFFFFFFU},
        {L"Red", 0xFF5252U},
        {L"Orange", 0xFF9F43U},
        {L"Yellow", 0xFFD93DU},
    });
    return choices;
}

void DeleteDialogResources(DialogState& state) noexcept {
    if (state.font != nullptr) DeleteObject(std::exchange(state.font, nullptr));
    if (state.background_brush != nullptr) DeleteObject(std::exchange(state.background_brush, nullptr));
    if (state.field_brush != nullptr) DeleteObject(std::exchange(state.field_brush, nullptr));
    if (state.surface_brush != nullptr) DeleteObject(std::exchange(state.surface_brush, nullptr));
    if (state.hover_brush != nullptr) DeleteObject(std::exchange(state.hover_brush, nullptr));
    if (state.selection_brush != nullptr) DeleteObject(std::exchange(state.selection_brush, nullptr));
    if (state.line_brush != nullptr) DeleteObject(std::exchange(state.line_brush, nullptr));
    if (state.accent_brush != nullptr) DeleteObject(std::exchange(state.accent_brush, nullptr));
}

void RecreateBrushes(DialogState& state) noexcept {
    if (state.background_brush != nullptr) DeleteObject(std::exchange(state.background_brush, nullptr));
    if (state.field_brush != nullptr) DeleteObject(std::exchange(state.field_brush, nullptr));
    if (state.surface_brush != nullptr) DeleteObject(std::exchange(state.surface_brush, nullptr));
    if (state.hover_brush != nullptr) DeleteObject(std::exchange(state.hover_brush, nullptr));
    if (state.selection_brush != nullptr) DeleteObject(std::exchange(state.selection_brush, nullptr));
    if (state.line_brush != nullptr) DeleteObject(std::exchange(state.line_brush, nullptr));
    if (state.accent_brush != nullptr) DeleteObject(std::exchange(state.accent_brush, nullptr));
    state.background_brush = CreateSolidBrush(WinColor(state.palette.background));
    state.field_brush = CreateSolidBrush(WinColor(state.palette.surface_alternate));
    state.surface_brush = CreateSolidBrush(WinColor(state.palette.surface));
    state.hover_brush = CreateSolidBrush(WinColor(state.palette.hover));
    state.selection_brush = CreateSolidBrush(WinColor(state.palette.selection));
    state.line_brush = CreateSolidBrush(WinColor(state.palette.line));
    state.accent_brush = CreateSolidBrush(WinColor(state.palette.accent));
}

void ShowPage(DialogState& state, const int page) noexcept {
    for (const auto& record : state.paged_controls) ShowWindow(record.window, record.page < 0 || record.page == page ? SW_SHOW : SW_HIDE);
}

void RecreateFont(DialogState& state) noexcept {
    const auto logical_height = MulDiv(17, static_cast<int>(state.draft.interface_text_scale_percent), 100);
    const auto next = CreateFontW(
        -Scale(state, logical_height),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH,
        L"Segoe UI");
    if (next == nullptr) return;
    for (const auto& record : state.paged_controls) SendMessageW(record.window, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
    if (state.font != nullptr) DeleteObject(state.font);
    state.font = next;
}

void UpdateOwnerDrawMetrics(DialogState& state) noexcept {
    if (state.tabs != nullptr) TabCtrl_SetItemSize(state.tabs, Scale(state, 116), Scale(state, 36));
    for (const auto& record : state.paged_controls) {
        std::array<wchar_t, 32U> class_name{};
        if (GetClassNameW(record.window, class_name.data(), static_cast<int>(class_name.size())) == 0) continue;
        if (_wcsicmp(class_name.data(), WC_COMBOBOXW) == 0) {
            SendMessageW(record.window, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Scale(state, 28));
            SendMessageW(record.window, CB_SETITEMHEIGHT, 0U, Scale(state, 28));
        } else if (_wcsicmp(class_name.data(), L"ListBox") == 0) {
            SendMessageW(record.window, LB_SETITEMHEIGHT, 0U, Scale(state, 28));
        }
    }
}

void LayoutControls(DialogState& state) noexcept {
    auto defer = BeginDeferWindowPos(static_cast<int>(state.paged_controls.size()));
    for (const auto& record : state.paged_controls) {
        if (defer == nullptr) break;
        defer = DeferWindowPos(
            defer,
            record.window,
            nullptr,
            Scale(state, record.x) - state.scroll_x,
            Scale(state, record.y) - state.scroll_y,
            Scale(state, record.width),
            Scale(state, record.height),
            SWP_NOACTIVATE | SWP_NOZORDER);
    }
    if (defer != nullptr) static_cast<void>(EndDeferWindowPos(defer));
}

void UpdateScrollBars(DialogState& state) noexcept {
    RECT client{};
    GetClientRect(state.window, &client);
    const auto client_width = std::max(1L, client.right - client.left);
    const auto client_height = std::max(1L, client.bottom - client.top);
    const auto content_width = std::max(1, Scale(state, kLogicalContentWidth));
    const auto content_height = std::max(1, Scale(state, kLogicalContentHeight));
    state.scroll_x = std::clamp(state.scroll_x, 0, std::max(0, content_width - static_cast<int>(client_width)));
    state.scroll_y = std::clamp(state.scroll_y, 0, std::max(0, content_height - static_cast<int>(client_height)));

    SCROLLINFO horizontal{sizeof(horizontal), SIF_PAGE | SIF_POS | SIF_RANGE, 0, content_width - 1, static_cast<UINT>(client_width), state.scroll_x, 0};
    SCROLLINFO vertical{sizeof(vertical), SIF_PAGE | SIF_POS | SIF_RANGE, 0, content_height - 1, static_cast<UINT>(client_height), state.scroll_y, 0};
    static_cast<void>(SetScrollInfo(state.window, SB_HORZ, &horizontal, TRUE));
    static_cast<void>(SetScrollInfo(state.window, SB_VERT, &vertical, TRUE));
}

void Relayout(DialogState& state) noexcept {
    if (std::exchange(state.relayout_active, true)) return;
    UpdateScrollBars(state);
    LayoutControls(state);
    state.relayout_active = false;
}

void HandleScroll(DialogState& state, const int bar, const WPARAM wparam, const int line_count = 1) noexcept {
    SCROLLINFO information{sizeof(information), SIF_ALL};
    if (!GetScrollInfo(state.window, bar, &information)) return;
    auto position = information.nPos;
    switch (LOWORD(wparam)) {
    case SB_LINEUP: position -= Scale(state, 24) * line_count; break;
    case SB_LINEDOWN: position += Scale(state, 24) * line_count; break;
    case SB_PAGEUP: position -= static_cast<int>(information.nPage); break;
    case SB_PAGEDOWN: position += static_cast<int>(information.nPage); break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: position = information.nTrackPos; break;
    default: return;
    }
    const auto maximum = std::max(0, information.nMax - static_cast<int>(information.nPage) + 1);
    position = std::clamp(position, 0, maximum);
    const auto previous_x = state.scroll_x;
    const auto previous_y = state.scroll_y;
    if (bar == SB_HORZ) state.scroll_x = position;
    else state.scroll_y = position;
    if (state.scroll_x == previous_x && state.scroll_y == previous_y) return;
    information.fMask = SIF_POS;
    information.nPos = position;
    static_cast<void>(SetScrollInfo(state.window, bar, &information, TRUE));
    static_cast<void>(ScrollWindowEx(
        state.window,
        previous_x - state.scroll_x,
        previous_y - state.scroll_y,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        SW_SCROLLCHILDREN | SW_INVALIDATE));
}

void SyncOsdOrderFromList(DialogState& state) noexcept {
    if (state.osd_order_list == nullptr) return;
    state.draft.osd_sensor_order_ids = {};
    state.draft.osd_sensor_order_count = 0U;
    const auto count = static_cast<int>(SendMessageW(state.osd_order_list, LB_GETCOUNT, 0, 0));
    for (int item{}; item < count && state.draft.osd_sensor_order_count < state.draft.osd_sensor_order_ids.size(); ++item) {
        const auto id = static_cast<std::uint64_t>(SendMessageW(state.osd_order_list, LB_GETITEMDATA, item, 0));
        if (id != 0U) state.draft.osd_sensor_order_ids[state.draft.osd_sensor_order_count++] = id;
    }
}

void RebuildOsdOrderList(DialogState& state) noexcept {
    if (state.osd_order_list == nullptr || state.snapshot == nullptr) return;
    SendMessageW(state.osd_order_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(state.osd_order_list, LB_RESETCONTENT, 0, 0);
    for (const auto& item : BuildOsdDisplayItems(*state.snapshot, state.draft)) {
        if (item.fps) continue;
        const auto index = SendMessageW(state.osd_order_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.text.c_str()));
        if (index != LB_ERR && index != LB_ERRSPACE) SendMessageW(state.osd_order_list, LB_SETITEMDATA, index, static_cast<LPARAM>(item.sensor_id));
    }
    SendMessageW(state.osd_order_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.osd_order_list, nullptr, TRUE);
}

void BuildControls(DialogState& state) {
    state.tabs = AddControl(state, WC_TABCONTROLW, L"", TCS_OWNERDRAWFIXED | TCS_FIXEDWIDTH | WS_TABSTOP, 20, 18, 610, 38, kTabs, -1);
    if (state.tabs != nullptr) static_cast<void>(SetWindowSubclass(state.tabs, &TabWindowProcedure, 1U, reinterpret_cast<DWORD_PTR>(&state)));
    TabCtrl_SetItemSize(state.tabs, Scale(state, 116), Scale(state, 36));
    constexpr std::array<const wchar_t*, 5U> pages{L"General", L"OSD", L"Monitoring", L"Graphs", L"Colors"};
    for (std::size_t index = 0U; index < pages.size(); ++index) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(pages[index]);
        static_cast<void>(TabCtrl_InsertItem(state.tabs, static_cast<int>(index), &item));
    }

    Label(state, L"Application theme", 82, 0);
    state.theme = Combo(state, 82, 0, {{L"Dark", 0U}, {L"Light", 1U}, {L"Midnight", 2U}}, static_cast<std::uint32_t>(state.draft.theme));
    Label(state, L"Text and accent color", 122, 0);
    state.color = Combo(state, 122, 0, ColorChoices(false), state.draft.text_color_rgb);
    Label(state, L"Hardware polling interval", 162, 0);
    state.refresh = Combo(state, 162, 0, {
        {L"100 ms (fastest)", 100U},
        {L"125 ms", 125U},
        {L"200 ms", 200U},
        {L"250 ms", 250U},
        {L"333 ms", 333U},
        {L"500 ms", 500U},
        {L"750 ms (recommended)", 750U},
        {L"1000 ms", 1'000U},
        {L"1500 ms", 1'500U},
        {L"2000 ms", 2'000U},
        {L"3000 ms", 3'000U},
        {L"5000 ms", 5'000U},
        {L"10000 ms (lightest)", 10'000U}}, state.draft.refresh_interval_ms);
    state.start_windows = Check(state, L"Start HardwareScope with Windows", state.draft.start_with_windows, 218, 0);
    state.start_minimized = Check(state, L"Start minimized to the notification tray", state.draft.start_minimized, 254, 0);
    state.updates = Check(state, L"Automatically check for stable updates", state.draft.automatic_updates, 306, 0);
    state.check_updates = PushButton(state, L"Check for updates", 42, 358, 180, 36, kSettingsCheckUpdatesCommand, 0);
    Label(state, L"Interface text size", 414, 0);
    state.text_scale = Combo(state, 414, 0, {{L"Standard — 100%", 100U}, {L"Large — 115%", 115U}, {L"Extra large — 130%", 130U}}, state.draft.interface_text_scale_percent);
    state.high_contrast = Check(state, L"Use stronger contrast for text, borders, and selections", state.draft.high_contrast, 454, 0);
    Label(state, L"Minimum / Maximum", 494, 0, 42, 180);
    PushButton(state, L"Reset Min/Max now", 234, 494, 180, 34, kSettingsResetMinMaxCommand, 0);
    state.reset_on_startup = Check(state, L"Reset at startup", state.draft.reset_min_max_on_startup, 530, 0, 42, 270);
    state.reset_on_game_launch = Check(state, L"Reset when a game launches", state.draft.reset_min_max_on_game_launch, 530, 0, 330, 290);
    Label(state, L"Automatic reset interval", 566, 0, 42, 180);
    state.reset_interval = ComboAt(state, 234, 566, 180, 0, {
        {L"Never", 0U}, {L"15 minutes", 15U}, {L"30 minutes", 30U}, {L"1 hour", 60U},
        {L"4 hours", 240U}, {L"12 hours", 720U}, {L"24 hours", 1'440U}, {L"1 week", 10'080U}},
        state.draft.reset_min_max_interval_minutes);
    state.export_settings = PushButton(state, L"Export settings", 42, 605, 180, 38, kSettingsExportCommand, 0);
    state.import_settings = PushButton(state, L"Import settings", 234, 605, 162, 38, kSettingsImportCommand, 0);

    state.show_osd = Check(state, L"Show the on-screen display", state.draft.show_osd, 82, 1, 32, 270);
    Label(state, L"Telemetry corner", 122, 1, 32, 125);
    state.position = ComboAt(state, 155, 122, 140, 1, {{L"Top left", 0U}, {L"Top right", 1U}, {L"Bottom left", 2U}, {L"Bottom right", 3U}}, static_cast<std::uint32_t>(state.draft.osd_position));
    state.fps_separate_position = Check(state, L"Separate FPS position", state.draft.fps_separate_position, 162, 1, 32, 270);
    Label(state, L"FPS corner", 202, 1, 32, 125);
    state.fps_position = ComboAt(state, 155, 202, 140, 1, {{L"Top left", 0U}, {L"Top right", 1U}, {L"Bottom left", 2U}, {L"Bottom right", 3U}}, static_cast<std::uint32_t>(state.draft.fps_osd_position));
    Label(state, L"Layout", 242, 1, 32, 125);
    state.layout = ComboAt(state, 155, 242, 140, 1, {{L"Vertical", 0U}, {L"Horizontal", 1U}}, static_cast<std::uint32_t>(state.draft.osd_layout));
    Label(state, L"Opacity", 282, 1, 32, 125);
    state.opacity = ComboAt(state, 155, 282, 140, 1, {{L"25%", 25U}, {L"50%", 50U}, {L"75%", 75U}, {L"90%", 90U}, {L"100%", 100U}}, state.draft.osd_opacity_percent);
    Label(state, L"Scale", 322, 1, 32, 125);
    state.scale = ComboAt(state, 155, 322, 140, 1, {{L"50%", 50U}, {L"75%", 75U}, {L"100%", 100U}, {L"125%", 125U}, {L"150%", 150U}, {L"200%", 200U}, {L"250%", 250U}}, state.draft.osd_scale_percent);
    Label(state, L"Spacing", 362, 1, 32, 125);
    state.spacing = ComboAt(state, 155, 362, 140, 1, {{L"Tight — 2 px", 2U}, {L"Compact — 5 px", 5U}, {L"Normal — 8 px", 8U}, {L"Roomy — 12 px", 12U}, {L"Wide — 20 px", 20U}}, state.draft.osd_spacing_px);
    state.separators = Check(state, L"Group separators", state.draft.osd_group_separators, 410, 1, 32, 270);
    state.background = Check(state, L"Translucent background", state.draft.osd_background, 446, 1, 32, 270);
    Label(state, L"LIVE PREVIEW", 82, 1, 320, 290);
    state.osd_preview = AddControl(state, L"STATIC", L"", SS_OWNERDRAW | WS_BORDER, 320, 112, 310, 250, kOsdPreviewControl, 1);
    Label(state, L"SENSOR ORDER — drag to rearrange", 382, 1, 320, 310);
    Label(state, L"FPS stays first", 410, 1, 320, 310);
    state.osd_order_list = AddControl(state, L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 320, 442, 310, 144, kOsdOrderListControl, 1);
    SendMessageW(state.osd_order_list, LB_SETITEMHEIGHT, 0U, Scale(state, 28));
    static_cast<void>(SetWindowSubclass(state.osd_order_list, &OsdOrderListWindowProcedure, 1U, reinterpret_cast<DWORD_PTR>(&state)));
    RebuildOsdOrderList(state);

    state.fps_enabled = Check(state, L"Enable game FPS monitoring", state.draft.fps_enabled, 82, 2);
    state.fps_game_only = Check(state, L"Show FPS only while a game is running", state.draft.fps_game_only, 114, 2);
    state.fps_one_percent_low = Check(state, L"Show rolling 1% low FPS", state.draft.fps_one_percent_low_enabled, 146, 2);
    Label(state, L"FPS refresh", 186, 2);
    state.fps_refresh = Combo(state, 186, 2, {{L"50 ms", 50U}, {L"100 ms", 100U}, {L"200 ms", 200U}, {L"250 ms", 250U}, {L"500 ms", 500U}}, state.draft.fps_refresh_interval_ms);
    Label(state, L"FPS smoothing", 226, 2);
    state.fps_smoothing = Combo(state, 226, 2, {{L"250 ms", 250U}, {L"500 ms", 500U}, {L"750 ms", 750U}, {L"1 second", 1'000U}, {L"1.25 seconds", 1'250U}}, state.draft.fps_smoothing_interval_ms);
    Label(state, L"FPS scale", 266, 2);
    state.fps_scale = Combo(state, 266, 2, {{L"50%", 50U}, {L"75%", 75U}, {L"100%", 100U}, {L"125%", 125U}, {L"150%", 150U}, {L"200%", 200U}, {L"250%", 250U}, {L"300%", 300U}}, state.draft.fps_scale_percent);
    Label(state, L"Sensors shown in the OSD", 318, 2, 42, 220);
    state.sensor_list = AddControl(state, L"LISTBOX", L"", LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 270, 314, 350, 244, kSettingsSensorListControl, 2);
    SendMessageW(state.sensor_list, LB_SETITEMHEIGHT, 0U, Scale(state, 28));
    for (std::uint32_t index = 0U; state.snapshot != nullptr && index < state.snapshot->count; ++index) {
        const auto& sensor = state.snapshot->sensors[index];
        if (sensor.kind == SensorKind::frame_rate) continue;
        std::wstring label = sensor.name.data();
        label += L"  —  ";
        label += sensor.hardware.data();
        const auto item = SendMessageW(state.sensor_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(state.sensor_list, LB_SETITEMDATA, static_cast<WPARAM>(item), static_cast<LPARAM>(index));
        if (IsSensorSelectedForOsd(sensor, state.draft)) SendMessageW(state.sensor_list, LB_SETSEL, TRUE, item);
    }

    state.graph_enabled = Check(state, L"Show graph in the OSD", state.draft.osd_graph_enabled, 76, 3, 42, 260);
    state.floating_graph = Check(state, L"Open a floating graph window", state.draft.floating_graph_enabled, 108, 3, 42, 280);
    state.floating_graph_topmost = Check(state, L"Keep floating graph on top", state.draft.floating_graph_topmost, 108, 3, 340, 280);
    Label(state, L"Graph sensors — select up to four sensors of the same unit", 146, 3, 42, 578);
    state.graph_sources = GraphSourceList(state, 176, 3);

    Label(state, L"Scale mode", 360, 3, 42, 112);
    state.graph_scale_mode = ComboAt(state, 158, 360, 152, 3, {
        {L"Fixed", 0U}, {L"Adaptive", 1U}, {L"Custom", 2U}}, static_cast<std::uint32_t>(state.draft.osd_graph_scale_mode));
    Label(state, L"Line thickness", 360, 3, 330, 130);
    state.graph_line_thickness = ComboAt(state, 468, 360, 152, 3, {
        {L"1 px", 1U}, {L"2 px", 2U}, {L"3 px", 3U}, {L"4 px", 4U}}, state.draft.osd_graph_line_thickness_px);

    Label(state, L"Custom minimum", 400, 3, 42, 112);
    state.graph_custom_minimum = NumberEdit(state, state.draft.osd_graph_custom_minimum, 158, 400, 152, 3);
    Label(state, L"Custom maximum", 400, 3, 330, 130);
    state.graph_custom_maximum = NumberEdit(state, state.draft.osd_graph_custom_maximum, 468, 400, 152, 3);

    Label(state, L"History", 440, 3, 42, 112);
    state.graph_history = ComboAt(state, 158, 440, 152, 3, {
        {L"5 seconds", 5U}, {L"10 seconds", 10U}, {L"15 seconds", 15U}, {L"30 seconds", 30U},
        {L"60 seconds", 60U}, {L"2 minutes", 120U}, {L"5 minutes", 300U}}, state.draft.osd_graph_history_seconds);
    Label(state, L"Refresh", 440, 3, 330, 130);
    state.graph_refresh = ComboAt(state, 468, 440, 152, 3, {
        {L"50 ms", 50U}, {L"100 ms", 100U}, {L"200 ms", 200U}, {L"250 ms", 250U},
        {L"500 ms", 500U}, {L"1 second", 1'000U}}, state.draft.osd_graph_refresh_interval_ms);

    Label(state, L"OSD width", 480, 3, 42, 112);
    state.graph_width = ComboAt(state, 158, 480, 152, 3, {
        {L"160 px", 160U}, {L"240 px", 240U}, {L"320 px", 320U}, {L"480 px", 480U},
        {L"640 px", 640U}, {L"960 px", 960U}}, state.draft.osd_graph_width_px);
    Label(state, L"OSD height", 480, 3, 330, 130);
    state.graph_height = ComboAt(state, 468, 480, 152, 3, {
        {L"64 px", 64U}, {L"88 px", 88U}, {L"120 px", 120U}, {L"160 px", 160U},
        {L"240 px", 240U}, {L"480 px", 480U}}, state.draft.osd_graph_height_px);

    state.graph_grid = Check(state, L"Grid lines", state.draft.osd_graph_grid, 526, 3, 42, 220);
    state.graph_labels = Check(state, L"Scale, time, and live-value labels", state.draft.osd_graph_labels, 526, 3, 300, 320);

    const auto category_choices = ColorChoices(true);
    constexpr std::array<const wchar_t*, 8U> category_labels{
        L"CPU temperatures", L"CPU usage", L"CPU clock speeds", L"CPU power & voltage",
        L"Graphics", L"Storage & drives", L"Memory", L"System & other"};
    const std::array<std::uint32_t, 8U> category_values{
        state.draft.cpu_temperature_color_rgb, state.draft.cpu_usage_color_rgb,
        state.draft.cpu_clock_color_rgb, state.draft.cpu_power_color_rgb,
        state.draft.graphics_color_rgb, state.draft.storage_color_rgb,
        state.draft.memory_color_rgb, state.draft.system_color_rgb};
    for (std::size_t index = 0U; index < category_labels.size(); ++index) {
        const auto y = 82 + static_cast<int>(index) * 40;
        Label(state, category_labels[index], y, 4);
        state.section_colors[index] = Combo(state, y, 4, category_choices, category_values[index]);
    }
    Label(state, L"FPS", 402, 4);
    state.fps_color = Combo(state, 402, 4, category_choices, state.draft.fps_color_rgb);
    for (std::size_t index{}; index < state.graph_colors.size(); ++index) {
        wchar_t label[32]{};
        static_cast<void>(swprintf_s(label, L"Graph line %zu", index + 1U));
        const auto y = 442 + static_cast<int>(index) * 40;
        Label(state, label, y, 4);
        state.graph_colors[index] = Combo(state, y, 4, ColorChoices(false), state.draft.osd_graph_colors_rgb[index]);
    }

    PushButton(state, L"Cancel", 408, 605, 100, 38, kCancel, -1);
    PushButton(state, L"Save settings", 518, 605, 120, 38, kSave, -1);
    UpdateGraphControlStates(state);
    ShowPage(state, 0);
}

void ReadControls(DialogState& state) noexcept {
    state.draft.theme = static_cast<Theme>(ComboValue(state.theme, 0U));
    state.draft.text_color_rgb = ComboValue(state.color, state.draft.text_color_rgb);
    state.draft.refresh_interval_ms = ComboValue(state.refresh, state.draft.refresh_interval_ms);
    state.draft.interface_text_scale_percent = ComboValue(state.text_scale, state.draft.interface_text_scale_percent);
    state.draft.high_contrast = Checked(state.high_contrast);
    state.draft.onboarding_completed = true;
    state.draft.start_with_windows = Checked(state.start_windows);
    state.draft.start_minimized = Checked(state.start_minimized);
    state.draft.reset_min_max_on_startup = Checked(state.reset_on_startup);
    state.draft.reset_min_max_on_game_launch = Checked(state.reset_on_game_launch);
    state.draft.reset_min_max_interval_minutes = ComboValue(state.reset_interval, state.draft.reset_min_max_interval_minutes);
    state.draft.automatic_updates = Checked(state.updates);
    state.draft.show_osd = Checked(state.show_osd);
    state.draft.osd_position = static_cast<OsdPosition>(ComboValue(state.position, 0U));
    state.draft.osd_layout = static_cast<OsdLayout>(ComboValue(state.layout, 0U));
    state.draft.osd_opacity_percent = ComboValue(state.opacity, state.draft.osd_opacity_percent);
    state.draft.osd_scale_percent = ComboValue(state.scale, state.draft.osd_scale_percent);
    state.draft.osd_spacing_px = ComboValue(state.spacing, state.draft.osd_spacing_px);
    state.draft.osd_group_separators = Checked(state.separators);
    state.draft.osd_background = Checked(state.background);
    state.draft.easy_temperature_enabled = false;
    state.draft.easy_temperature_mask = 0U;
    state.draft.fps_enabled = Checked(state.fps_enabled);
    state.draft.fps_game_only = Checked(state.fps_game_only);
    state.draft.fps_separate_position = Checked(state.fps_separate_position);
    state.draft.fps_osd_position = static_cast<OsdPosition>(ComboValue(state.fps_position, 1U));
    state.draft.fps_refresh_interval_ms = ComboValue(state.fps_refresh, state.draft.fps_refresh_interval_ms);
    state.draft.fps_smoothing_interval_ms = ComboValue(state.fps_smoothing, state.draft.fps_smoothing_interval_ms);
    state.draft.fps_color_rgb = ComboValue(state.fps_color, state.draft.fps_color_rgb);
    state.draft.fps_scale_percent = ComboValue(state.fps_scale, state.draft.fps_scale_percent);
    state.draft.fps_one_percent_low_enabled = Checked(state.fps_one_percent_low);
    state.draft.osd_graph_enabled = Checked(state.graph_enabled);
    state.draft.floating_graph_enabled = Checked(state.floating_graph);
    state.draft.floating_graph_topmost = Checked(state.floating_graph_topmost);
    std::array<std::uint64_t, AppSettings::kMaximumGraphSensors> graph_ids{};
    std::uint32_t graph_count{};
    std::optional<SensorUnit> graph_unit;
    const auto graph_items = static_cast<int>(SendMessageW(state.graph_sources, LB_GETCOUNT, 0, 0));
    for (int item{}; item < graph_items && graph_count < graph_ids.size(); ++item) {
        if (SendMessageW(state.graph_sources, LB_GETSEL, item, 0) <= 0) continue;
        const auto id = static_cast<std::uint64_t>(SendMessageW(state.graph_sources, LB_GETITEMDATA, item, 0));
        const auto* sensor = SnapshotSensor(state, id);
        if (sensor == nullptr) continue;
        if (!graph_unit) graph_unit = sensor->unit;
        if (sensor->unit != *graph_unit) continue;
        graph_ids[graph_count++] = id;
    }
    if (graph_count != 0U) {
        state.draft.osd_graph_sensor_ids = graph_ids;
        state.draft.osd_graph_sensor_count = graph_count;
        state.draft.osd_graph_sensor_id = graph_ids[0];
    }
    state.draft.osd_graph_history_seconds = ComboValue(state.graph_history, state.draft.osd_graph_history_seconds);
    state.draft.osd_graph_refresh_interval_ms = ComboValue(state.graph_refresh, state.draft.osd_graph_refresh_interval_ms);
    state.draft.osd_graph_width_px = ComboValue(state.graph_width, state.draft.osd_graph_width_px);
    state.draft.osd_graph_height_px = ComboValue(state.graph_height, state.draft.osd_graph_height_px);
    state.draft.osd_graph_scale_mode = static_cast<GraphScaleMode>(ComboValue(state.graph_scale_mode, 0U));
    state.draft.osd_graph_custom_minimum = NumberValue(state.graph_custom_minimum, state.draft.osd_graph_custom_minimum);
    state.draft.osd_graph_custom_maximum = NumberValue(state.graph_custom_maximum, state.draft.osd_graph_custom_maximum);
    state.draft.osd_graph_line_thickness_px = ComboValue(state.graph_line_thickness, state.draft.osd_graph_line_thickness_px);
    state.draft.osd_graph_grid = Checked(state.graph_grid);
    state.draft.osd_graph_labels = Checked(state.graph_labels);
    state.draft.cpu_temperature_color_rgb = ComboValue(state.section_colors[0], state.draft.cpu_temperature_color_rgb);
    state.draft.cpu_usage_color_rgb = ComboValue(state.section_colors[1], state.draft.cpu_usage_color_rgb);
    state.draft.cpu_clock_color_rgb = ComboValue(state.section_colors[2], state.draft.cpu_clock_color_rgb);
    state.draft.cpu_power_color_rgb = ComboValue(state.section_colors[3], state.draft.cpu_power_color_rgb);
    state.draft.graphics_color_rgb = ComboValue(state.section_colors[4], state.draft.graphics_color_rgb);
    state.draft.storage_color_rgb = ComboValue(state.section_colors[5], state.draft.storage_color_rgb);
    state.draft.memory_color_rgb = ComboValue(state.section_colors[6], state.draft.memory_color_rgb);
    state.draft.system_color_rgb = ComboValue(state.section_colors[7], state.draft.system_color_rgb);
    for (std::size_t index{}; index < state.graph_colors.size(); ++index) {
        state.draft.osd_graph_colors_rgb[index] = ComboValue(state.graph_colors[index], state.draft.osd_graph_colors_rgb[index]);
    }
    const auto count = static_cast<int>(SendMessageW(state.sensor_list, LB_GETCOUNT, 0, 0));
    if (count > 0) {
        state.draft.pinned_sensor_ids = {};
        state.draft.pinned_sensor_count = 0U;
        for (int item = 0; item < count && state.draft.pinned_sensor_count < state.draft.pinned_sensor_ids.size(); ++item) {
            if (SendMessageW(state.sensor_list, LB_GETSEL, item, 0) <= 0) continue;
            const auto sensor_index = static_cast<std::uint32_t>(SendMessageW(state.sensor_list, LB_GETITEMDATA, item, 0));
            if (state.snapshot != nullptr && sensor_index < state.snapshot->count) state.draft.pinned_sensor_ids[state.draft.pinned_sensor_count++] = state.snapshot->sensors[sensor_index].id;
        }
    }
    SyncOsdOrderFromList(state);
    state.draft.Normalize();
}

void ApplyDraftToControls(DialogState& state) noexcept {
    SelectComboValue(state.theme, static_cast<std::uint32_t>(state.draft.theme));
    SelectComboValue(state.color, state.draft.text_color_rgb);
    SelectComboValue(state.refresh, state.draft.refresh_interval_ms);
    SelectComboValue(state.text_scale, state.draft.interface_text_scale_percent);
    SendMessageW(state.high_contrast, BM_SETCHECK, state.draft.high_contrast ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.start_windows, BM_SETCHECK, state.draft.start_with_windows ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.start_minimized, BM_SETCHECK, state.draft.start_minimized ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.reset_on_startup, BM_SETCHECK, state.draft.reset_min_max_on_startup ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.reset_on_game_launch, BM_SETCHECK, state.draft.reset_min_max_on_game_launch ? BST_CHECKED : BST_UNCHECKED, 0);
    SelectComboValue(state.reset_interval, state.draft.reset_min_max_interval_minutes);
    SendMessageW(state.updates, BM_SETCHECK, state.draft.automatic_updates ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.show_osd, BM_SETCHECK, state.draft.show_osd ? BST_CHECKED : BST_UNCHECKED, 0);
    SelectComboValue(state.position, static_cast<std::uint32_t>(state.draft.osd_position));
    SelectComboValue(state.layout, static_cast<std::uint32_t>(state.draft.osd_layout));
    SelectComboValue(state.opacity, state.draft.osd_opacity_percent);
    SelectComboValue(state.scale, state.draft.osd_scale_percent);
    SelectComboValue(state.spacing, state.draft.osd_spacing_px);
    SendMessageW(state.separators, BM_SETCHECK, state.draft.osd_group_separators ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.background, BM_SETCHECK, state.draft.osd_background ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.fps_enabled, BM_SETCHECK, state.draft.fps_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.fps_game_only, BM_SETCHECK, state.draft.fps_game_only ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.fps_separate_position, BM_SETCHECK, state.draft.fps_separate_position ? BST_CHECKED : BST_UNCHECKED, 0);
    SelectComboValue(state.fps_position, static_cast<std::uint32_t>(state.draft.fps_osd_position));
    SelectComboValue(state.fps_refresh, state.draft.fps_refresh_interval_ms);
    SelectComboValue(state.fps_smoothing, state.draft.fps_smoothing_interval_ms);
    SelectComboValue(state.fps_color, state.draft.fps_color_rgb);
    SelectComboValue(state.fps_scale, state.draft.fps_scale_percent);
    SendMessageW(state.fps_one_percent_low, BM_SETCHECK, state.draft.fps_one_percent_low_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.graph_enabled, BM_SETCHECK, state.draft.osd_graph_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.floating_graph, BM_SETCHECK, state.draft.floating_graph_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.floating_graph_topmost, BM_SETCHECK, state.draft.floating_graph_topmost ? BST_CHECKED : BST_UNCHECKED, 0);
    SelectComboValue(state.graph_scale_mode, static_cast<std::uint32_t>(state.draft.osd_graph_scale_mode));
    wchar_t number[48]{};
    static_cast<void>(swprintf_s(number, L"%.2f", state.draft.osd_graph_custom_minimum));
    SetWindowTextW(state.graph_custom_minimum, number);
    static_cast<void>(swprintf_s(number, L"%.2f", state.draft.osd_graph_custom_maximum));
    SetWindowTextW(state.graph_custom_maximum, number);
    SelectComboValue(state.graph_history, state.draft.osd_graph_history_seconds);
    SelectComboValue(state.graph_refresh, state.draft.osd_graph_refresh_interval_ms);
    SelectComboValue(state.graph_width, state.draft.osd_graph_width_px);
    SelectComboValue(state.graph_height, state.draft.osd_graph_height_px);
    SelectComboValue(state.graph_line_thickness, state.draft.osd_graph_line_thickness_px);
    SendMessageW(state.graph_grid, BM_SETCHECK, state.draft.osd_graph_grid ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.graph_labels, BM_SETCHECK, state.draft.osd_graph_labels ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto graph_items = static_cast<int>(SendMessageW(state.graph_sources, LB_GETCOUNT, 0, 0));
    for (int item{}; item < graph_items; ++item) {
        const auto id = static_cast<std::uint64_t>(SendMessageW(state.graph_sources, LB_GETITEMDATA, item, 0));
        const auto selected = std::find(
            state.draft.osd_graph_sensor_ids.begin(),
            state.draft.osd_graph_sensor_ids.begin() + state.draft.osd_graph_sensor_count,
            id) != state.draft.osd_graph_sensor_ids.begin() + state.draft.osd_graph_sensor_count;
        SendMessageW(state.graph_sources, LB_SETSEL, selected ? TRUE : FALSE, item);
    }
    const std::array<std::uint32_t, 8U> colors{
        state.draft.cpu_temperature_color_rgb, state.draft.cpu_usage_color_rgb,
        state.draft.cpu_clock_color_rgb, state.draft.cpu_power_color_rgb,
        state.draft.graphics_color_rgb, state.draft.storage_color_rgb,
        state.draft.memory_color_rgb, state.draft.system_color_rgb};
    for (std::size_t index = 0U; index < colors.size(); ++index) SelectComboValue(state.section_colors[index], colors[index]);
    for (std::size_t index{}; index < state.graph_colors.size(); ++index) {
        SelectComboValue(state.graph_colors[index], state.draft.osd_graph_colors_rgb[index]);
    }
    UpdateGraphControlStates(state);
    const auto count = static_cast<int>(SendMessageW(state.sensor_list, LB_GETCOUNT, 0, 0));
    for (int item = 0; item < count; ++item) {
        const auto sensor_index = static_cast<std::uint32_t>(SendMessageW(state.sensor_list, LB_GETITEMDATA, item, 0));
        const auto selected = state.snapshot != nullptr
            && sensor_index < state.snapshot->count
            && IsSensorSelectedForOsd(state.snapshot->sensors[sensor_index], state.draft);
        SendMessageW(state.sensor_list, LB_SETSEL, selected ? TRUE : FALSE, item);
    }
    RebuildOsdOrderList(state);
    if (state.osd_preview != nullptr) InvalidateRect(state.osd_preview, nullptr, TRUE);
}

std::optional<std::filesystem::path> SelectSettingsPath(const HWND owner, const bool save) noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    if (save) static_cast<void>(wcscpy_s(path.data(), path.size(), L"HardwareScope-settings.ini"));
    constexpr wchar_t filter[] = L"HardwareScope settings (*.ini)\0*.ini\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrDefExt = L"ini";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
        | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const auto accepted = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    return accepted ? std::optional<std::filesystem::path>{path.data()} : std::nullopt;
}

bool IsPushButton(const DialogState& state, const HWND control) noexcept {
    return std::find(state.push_buttons.begin(), state.push_buttons.end(), control) != state.push_buttons.end();
}

void DrawControlText(
    const DialogState& state,
    const HDC dc,
    std::wstring& text,
    RECT bounds,
    const COLORREF color,
    const UINT alignment) noexcept {
    const auto previous_font = SelectObject(dc, state.font);
    const auto previous_mode = SetBkMode(dc, TRANSPARENT);
    const auto previous_color = SetTextColor(dc, color);
    bounds.left += Scale(state, 9);
    bounds.right -= Scale(state, 9);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &bounds, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | alignment);
    SetTextColor(dc, previous_color);
    SetBkMode(dc, previous_mode);
    SelectObject(dc, previous_font);
}

COLORREF PreviewColor(const std::uint32_t rgb, const std::uint32_t opacity, const std::uint32_t background) noexcept {
    const auto blend = [&](const unsigned shift) {
        const auto foreground_component = (rgb >> shift) & 0xFFU;
        const auto background_component = (background >> shift) & 0xFFU;
        return (foreground_component * opacity + background_component * (100U - opacity)) / 100U;
    };
    return RGB(blend(16U), blend(8U), blend(0U));
}

void DrawOsdPreview(DialogState& state, const DRAWITEMSTRUCT& item) noexcept {
    auto bounds = item.rcItem;
    FillRect(item.hDC, &bounds, state.background_brush);
    InflateRect(&bounds, -Scale(state, 7), -Scale(state, 7));
    const auto monitor_brush = CreateSolidBrush(RGB(4, 7, 10));
    FillRect(item.hDC, &bounds, monitor_brush);
    DeleteObject(monitor_brush);

    std::vector<OsdDisplayItem> hardware_items;
    std::vector<OsdDisplayItem> fps_items;
    if (state.snapshot != nullptr) {
        for (auto& display_item : BuildOsdDisplayItems(*state.snapshot, state.draft)) {
            (display_item.fps ? fps_items : hardware_items).push_back(std::move(display_item));
        }
    }
    if (state.draft.fps_enabled && fps_items.empty()) {
        fps_items.push_back(OsdDisplayItem{kFpsSensorId, L"FPS 144", OsdHardwareGroup::fps, state.draft.fps_color_rgb, true});
        if (state.draft.fps_one_percent_low_enabled) {
            fps_items.push_back(OsdDisplayItem{kFpsOnePercentLowSensorId, L"1% Low 112", OsdHardwareGroup::fps, state.draft.fps_color_rgb, true});
        }
    }
    if (!state.draft.fps_separate_position) {
        hardware_items.insert(hardware_items.begin(), fps_items.begin(), fps_items.end());
        fps_items.clear();
    }

    const auto draw_surface = [&](const std::vector<OsdDisplayItem>& items, const OsdPosition position, const bool fps_surface) {
        if (items.empty()) return;
        const auto scale_percent = fps_surface ? state.draft.fps_scale_percent : state.draft.osd_scale_percent;
        const auto font_height = std::clamp(MulDiv(11, static_cast<int>(scale_percent), 100), 8, 22);
        const auto font = CreateFontW(-Scale(state, font_height), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const auto previous_font = SelectObject(item.hDC, font);
        const auto previous_mode = SetBkMode(item.hDC, TRANSPARENT);
        const auto gap = std::clamp(Scale(state, static_cast<int>(state.draft.osd_spacing_px / 2U)), 1, Scale(state, 12));
        const auto line_height = Scale(state, font_height + 4);
        const auto graph_height = !fps_surface && state.draft.osd_graph_enabled ? Scale(state, 46) : 0;
        int surface_width = state.draft.osd_layout == OsdLayout::horizontal
            ? std::max(120, static_cast<int>(bounds.right - bounds.left) - Scale(state, 16))
            : Scale(state, 190);
        surface_width = std::min(surface_width, static_cast<int>(bounds.right - bounds.left) - Scale(state, 8));

        struct PreviewPlacement final {
            const OsdDisplayItem* item{};
            std::wstring text;
            int offset_x{};
            int row{};
        };
        std::vector<PreviewPlacement> placements;
        placements.reserve(items.size());
        const auto content_width = std::max(Scale(state, 40), surface_width - Scale(state, 12));
        int row{};
        int cursor_x{};
        OsdHardwareGroup previous_group = OsdHardwareGroup::other;
        bool first = true;
        for (const auto& display_item : items) {
            auto text = display_item.text;
            const auto group_changed = !first && display_item.group != previous_group;
            if (state.draft.osd_layout == OsdLayout::horizontal
                && state.draft.osd_group_separators && group_changed) {
                text = L"|  " + text;
            }

            SIZE size{};
            GetTextExtentPoint32W(item.hDC, text.data(), static_cast<int>(text.size()), &size);
            if (state.draft.osd_layout == OsdLayout::horizontal
                && cursor_x != 0 && cursor_x + size.cx > content_width) {
                ++row;
                cursor_x = 0;
                // A wrapped row is already visually separated, so do not begin it with a divider.
                text = display_item.text;
                GetTextExtentPoint32W(item.hDC, text.data(), static_cast<int>(text.size()), &size);
            }

            placements.push_back(PreviewPlacement{&display_item, std::move(text), cursor_x, row});
            if (state.draft.osd_layout == OsdLayout::horizontal) {
                cursor_x += std::min(static_cast<int>(size.cx), content_width) + gap;
            } else {
                ++row;
            }
            previous_group = display_item.group;
            first = false;
        }

        const auto row_count = state.draft.osd_layout == OsdLayout::horizontal
            ? (placements.empty() ? 0 : placements.back().row + 1)
            : static_cast<int>(placements.size());
        int surface_height = row_count * line_height
            + std::max(0, row_count - 1) * gap + Scale(state, 12) + graph_height;
        surface_height = std::min(surface_height, static_cast<int>(bounds.bottom - bounds.top) - Scale(state, 8));
        RECT surface{bounds.left + Scale(state, 5), bounds.top + Scale(state, 5), 0, 0};
        if (position == OsdPosition::top_right || position == OsdPosition::bottom_right) surface.left = bounds.right - surface_width - Scale(state, 5);
        if (position == OsdPosition::bottom_left || position == OsdPosition::bottom_right) surface.top = bounds.bottom - surface_height - Scale(state, 5);
        surface.right = surface.left + surface_width;
        surface.bottom = surface.top + surface_height;
        if (state.draft.osd_background) FillRect(item.hDC, &surface, state.surface_brush);

        for (const auto& placement : placements) {
            const auto& display_item = *placement.item;
            SetTextColor(item.hDC, PreviewColor(
                display_item.fps ? state.draft.fps_color_rgb : display_item.color_rgb,
                state.draft.osd_opacity_percent,
                0x04070AU));
            const auto x = surface.left + Scale(state, 6) + placement.offset_x;
            const auto y = surface.top + Scale(state, 4) + placement.row * (line_height + gap);
            RECT text_bounds{x, y, surface.right - Scale(state, 5), y + line_height};
            DrawTextW(item.hDC, placement.text.data(), static_cast<int>(placement.text.size()), &text_bounds,
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
        if (graph_height != 0) {
            const auto graph_top = surface.bottom - graph_height + Scale(state, 4);
            const auto grid_pen = CreatePen(PS_SOLID, 1, WinColor(state.palette.line));
            const auto old_pen = SelectObject(item.hDC, grid_pen);
            for (int grid{}; grid < 3; ++grid) {
                const auto gy = graph_top + grid * std::max(1, graph_height / 3);
                MoveToEx(item.hDC, surface.left + Scale(state, 6), gy, nullptr);
                LineTo(item.hDC, surface.right - Scale(state, 6), gy);
            }
            SelectObject(item.hDC, old_pen);
            DeleteObject(grid_pen);
            const auto graph_pen = CreatePen(PS_SOLID, Scale(state, static_cast<int>(state.draft.osd_graph_line_thickness_px)), WinColor(state.draft.osd_graph_colors_rgb[0]));
            const auto previous_pen = SelectObject(item.hDC, graph_pen);
            const auto left = surface.left + Scale(state, 6);
            const auto width = std::max(1, static_cast<int>(surface.right - left) - Scale(state, 6));
            for (int px{}; px < width; ++px) {
                const auto wave = std::sin(static_cast<double>(px) * 0.12) * static_cast<double>(graph_height) * 0.22;
                const auto gy = graph_top + graph_height / 2 - static_cast<int>(wave);
                if (px == 0) MoveToEx(item.hDC, left, gy, nullptr); else LineTo(item.hDC, left + px, gy);
            }
            SelectObject(item.hDC, previous_pen);
            DeleteObject(graph_pen);
        }
        SetBkMode(item.hDC, previous_mode);
        SelectObject(item.hDC, previous_font);
        DeleteObject(font);
    };

    draw_surface(hardware_items, state.draft.osd_position, false);
    draw_surface(fps_items, state.draft.fps_osd_position, true);
}

void DrawOwnerItem(DialogState& state, const DRAWITEMSTRUCT& item) noexcept {
    if (item.hDC == nullptr) return;
    const auto selected = (item.itemState & ODS_SELECTED) != 0U;
    const auto focused = (item.itemState & ODS_FOCUS) != 0U;
    const auto disabled = (item.itemState & ODS_DISABLED) != 0U;
    auto bounds = item.rcItem;
    std::wstring text;

    if (item.CtlType == ODT_STATIC && item.hwndItem == state.osd_preview) {
        DrawOsdPreview(state, item);
        return;
    }

    if (item.CtlType == ODT_TAB) {
        std::array<wchar_t, 128U> buffer{};
        TCITEMW tab{};
        tab.mask = TCIF_TEXT;
        tab.pszText = buffer.data();
        tab.cchTextMax = static_cast<int>(buffer.size());
        if (TabCtrl_GetItem(item.hwndItem, static_cast<int>(item.itemID), &tab)) text = buffer.data();
        FillRect(item.hDC, &bounds, selected ? state.surface_brush : state.background_brush);
        if (selected) {
            auto accent_line = bounds;
            accent_line.top = accent_line.bottom - Scale(state, 3);
            FillRect(item.hDC, &accent_line, state.accent_brush);
        }
        DrawControlText(state, item.hDC, text, bounds, WinColor(selected ? state.palette.accent : state.palette.muted), DT_CENTER);
        return;
    }

    if (item.CtlType == ODT_COMBOBOX || item.CtlType == ODT_LISTBOX) {
        if (item.itemID != static_cast<UINT>(-1)) {
            const auto length_message = item.CtlType == ODT_COMBOBOX ? CB_GETLBTEXTLEN : LB_GETTEXTLEN;
            const auto text_message = item.CtlType == ODT_COMBOBOX ? CB_GETLBTEXT : LB_GETTEXT;
            const auto length = SendMessageW(item.hwndItem, length_message, item.itemID, 0);
            if (length >= 0) {
                text.resize(static_cast<std::size_t>(length) + 1U);
                SendMessageW(item.hwndItem, text_message, item.itemID, reinterpret_cast<LPARAM>(text.data()));
                text.resize(static_cast<std::size_t>(length));
            }
        } else {
            std::array<wchar_t, 256U> buffer{};
            GetWindowTextW(item.hwndItem, buffer.data(), static_cast<int>(buffer.size()));
            text = buffer.data();
        }
        FillRect(item.hDC, &bounds, selected ? state.selection_brush : state.field_brush);
        DrawControlText(
            state,
            item.hDC,
            text,
            bounds,
            WinColor(disabled ? state.palette.disabled : selected ? state.palette.selection_text : state.palette.text),
            DT_LEFT);
        if (focused) FrameRect(item.hDC, &bounds, state.accent_brush);
        return;
    }

    if (item.CtlType == ODT_BUTTON && IsPushButton(state, item.hwndItem)) {
        std::array<wchar_t, 256U> buffer{};
        GetWindowTextW(item.hwndItem, buffer.data(), static_cast<int>(buffer.size()));
        text = buffer.data();
        FillRect(item.hDC, &bounds, state.background_brush);
        auto face = bounds;
        InflateRect(&face, -1, -1);
        const auto is_primary = GetDlgCtrlID(item.hwndItem) == kSave;
        FillRect(item.hDC, &face, selected ? state.selection_brush : is_primary ? state.accent_brush : focused ? state.hover_brush : state.surface_brush);
        FrameRect(item.hDC, &face, focused ? state.accent_brush : state.line_brush);
        DrawControlText(
            state,
            item.hDC,
            text,
            bounds,
            WinColor(disabled ? state.palette.disabled : is_primary ? state.palette.selection_text : state.palette.text),
            DT_CENTER);
        return;
    }
}

void PreviewPalette(DialogState& state) noexcept {
    state.draft.theme = static_cast<Theme>(ComboValue(state.theme, static_cast<std::uint32_t>(state.draft.theme)));
    state.draft.text_color_rgb = ComboValue(state.color, state.draft.text_color_rgb);
    state.draft.interface_text_scale_percent = ComboValue(state.text_scale, state.draft.interface_text_scale_percent);
    state.draft.high_contrast = Checked(state.high_contrast);
    state.palette = PaletteFor(state.draft.theme, state.draft.text_color_rgb, state.draft.high_contrast);
    RecreateFont(state);
    UpdateOwnerDrawMetrics(state);
    RecreateBrushes(state);
    for (const auto& record : state.paged_controls) StyleControl(state, record.window);
    const BOOL dark = state.draft.theme != Theme::light ? TRUE : FALSE;
    static_cast<void>(DwmSetWindowAttribute(state.window, 20U, &dark, sizeof(dark)));
    static_cast<void>(SetWindowTheme(state.window, state.draft.theme != Theme::light ? L"DarkMode_Explorer" : L"Explorer", nullptr));
    Relayout(state);
    if (state.osd_preview != nullptr) InvalidateRect(state.osd_preview, nullptr, TRUE);
    RedrawWindow(state.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void StartSettingsTransfer(DialogState& state, const std::filesystem::path& path, const bool exporting) noexcept {
    if (state.transfer) return;
    try {
        auto job = std::make_shared<SettingsTransfer>();
        job->settings = state.draft;
        job->exporting = exporting;
        // The worker owns only a copied path and independent result, never the
        // dialog or HWND. Closing the dialog during I/O cannot dangle a callback.
        std::thread([job, path] {
            const SettingsStore store(path);
            job->success = job->exporting ? store.Save(job->settings) : store.Load(job->settings);
            job->done.store(true, std::memory_order_release);
        }).detach();
        state.transfer = std::move(job);
        EnableWindow(state.import_settings, FALSE);
        EnableWindow(state.export_settings, FALSE);
        EnableWindow(GetDlgItem(state.window, kSave), FALSE);
        SetTimer(state.window, kSettingsTransferTimer, 100U, nullptr);
    } catch (...) {
        MessageBoxW(state.window, L"Could not start the settings file operation.", L"Settings", MB_OK | MB_ICONWARNING);
    }
}

LRESULT CALLBACK WindowProcedure(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) noexcept {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<DialogState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->window = window;
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
#if HARDWARESCOPE_INTERNAL_TEST_HOOKS
    case kSelectSettingsTabTestMessage: {
        const auto page = std::clamp(static_cast<int>(wparam), 0, TabCtrl_GetItemCount(state->tabs) - 1);
        static_cast<void>(TabCtrl_SetCurSel(state->tabs, page));
        ShowPage(*state, page);
        return page + 1;
    }
    case kApplySettingsDpiTestMessage: {
        const auto previous_dpi = std::max(96U, state->dpi);
        const auto target_dpi = std::clamp(static_cast<UINT>(wparam), 96U, 384U);
        RECT target{};
        GetWindowRect(window, &target);
        const auto width = MulDiv(target.right - target.left, static_cast<int>(target_dpi), static_cast<int>(previous_dpi));
        const auto height = MulDiv(target.bottom - target.top, static_cast<int>(target_dpi), static_cast<int>(previous_dpi));
        target.right = target.left + width;
        target.bottom = target.top + height;
        static_cast<void>(SendMessageW(
            window,
            WM_DPICHANGED,
            MAKEWPARAM(target_dpi, target_dpi),
            reinterpret_cast<LPARAM>(&target)));
        return static_cast<LRESULT>(state->dpi);
    }
#endif
    case WM_DPICHANGED: {
        state->dpi = HIWORD(wparam) == 0U ? 96U : HIWORD(wparam);
        RecreateFont(*state);
        UpdateOwnerDrawMetrics(*state);
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        MONITORINFO monitor_information{};
        monitor_information.cbSize = sizeof(monitor_information);
        const auto monitor = MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST);
        RECT target = *suggested;
        if (GetMonitorInfoW(monitor, &monitor_information)) {
            const auto maximum_width = monitor_information.rcWork.right - monitor_information.rcWork.left;
            const auto maximum_height = monitor_information.rcWork.bottom - monitor_information.rcWork.top;
            const auto width = std::min(target.right - target.left, maximum_width);
            const auto height = std::min(target.bottom - target.top, maximum_height);
            target.left = std::clamp(target.left, monitor_information.rcWork.left, monitor_information.rcWork.right - width);
            target.top = std::clamp(target.top, monitor_information.rcWork.top, monitor_information.rcWork.bottom - height);
            target.right = target.left + width;
            target.bottom = target.top + height;
        }
        static_cast<void>(SetWindowPos(
            window,
            nullptr,
            target.left,
            target.top,
            target.right - target.left,
            target.bottom - target.top,
            SWP_NOACTIVATE | SWP_NOZORDER));
        Relayout(*state);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_SIZE:
        Relayout(*state);
        return 0;
    case WM_HSCROLL:
        HandleScroll(*state, SB_HORZ, wparam);
        return 0;
    case WM_VSCROLL:
        HandleScroll(*state, SB_VERT, wparam);
        return 0;
    case WM_MOUSEWHEEL: {
        const auto lines = std::max(1, std::abs(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA) * 3;
        HandleScroll(*state, SB_VERT, MAKEWPARAM(GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? SB_LINEUP : SB_LINEDOWN, 0), lines);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kSettingsTransferTimer && state->transfer && state->transfer->done.load(std::memory_order_acquire)) {
            KillTimer(window, kSettingsTransferTimer);
            const auto job = std::move(state->transfer);
            EnableWindow(state->import_settings, TRUE);
            EnableWindow(state->export_settings, TRUE);
            EnableWindow(GetDlgItem(window, kSave), TRUE);
            if (job->success && !job->exporting) {
                state->draft = job->settings;
                state->draft.onboarding_completed = true;
                ApplyDraftToControls(*state);
                PreviewPalette(*state);
            }
            MessageBoxW(window, job->success
                ? (job->exporting ? L"Settings exported successfully." : L"Settings imported. Review them, then choose Save settings to apply.")
                : L"Could not read or write the settings file. Imports must be supported HardwareScope files no larger than 256 KiB.",
                L"Settings", MB_OK | (job->success ? MB_ICONINFORMATION : MB_ICONWARNING));
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == kSettingsResetMinMaxCommand) {
            static_cast<void>(SendMessageW(state->owner, WM_COMMAND, kCommandResetMinMax, 0));
            MessageBoxW(window, L"Minimum and maximum values will restart with the next sensor update.", L"Reset Min/Max", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (HIWORD(wparam) == LBN_SELCHANGE && reinterpret_cast<HWND>(lparam) == state->sensor_list) {
            ReadControls(*state);
            RebuildOsdOrderList(*state);
            InvalidateRect(state->osd_preview, nullptr, TRUE);
            return 0;
        }
        if (HIWORD(wparam) == LBN_SELCHANGE && reinterpret_cast<HWND>(lparam) == state->graph_sources) {
            EnforceGraphSelection(*state);
            return 0;
        }
        if ((HIWORD(wparam) == CBN_SELCHANGE && reinterpret_cast<HWND>(lparam) == state->graph_scale_mode)
            || (HIWORD(wparam) == BN_CLICKED && reinterpret_cast<HWND>(lparam) == state->floating_graph)) {
            UpdateGraphControlStates(*state);
            return 0;
        }
        if (HIWORD(wparam) == CBN_SELCHANGE
            && (reinterpret_cast<HWND>(lparam) == state->theme
                || reinterpret_cast<HWND>(lparam) == state->color
                || reinterpret_cast<HWND>(lparam) == state->text_scale)) {
            PreviewPalette(*state);
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED && reinterpret_cast<HWND>(lparam) == state->high_contrast) {
            PreviewPalette(*state);
            return 0;
        }
        if ((HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == BN_CLICKED)
            && reinterpret_cast<HWND>(lparam) != nullptr
            && !IsPushButton(*state, reinterpret_cast<HWND>(lparam))) {
            ReadControls(*state);
            InvalidateRect(state->osd_preview, nullptr, TRUE);
            return 0;
        }
        if (LOWORD(wparam) == kSettingsExportCommand) {
            if (state->transfer) return 0;
            if (const auto path = SelectSettingsPath(window, true)) {
                ReadControls(*state);
                StartSettingsTransfer(*state, *path, true);
            }
            return 0;
        }
        if (LOWORD(wparam) == kSettingsImportCommand) {
            if (state->transfer) return 0;
            if (const auto path = SelectSettingsPath(window, false)) {
                StartSettingsTransfer(*state, *path, false);
            }
            return 0;
        }
        if (LOWORD(wparam) == kSettingsCheckUpdatesCommand) {
            if (PostMessageW(state->owner, kManualUpdateRequestMessage, 0U, 0)) {
                EnableWindow(state->check_updates, FALSE);
                SetWindowTextW(state->check_updates, L"Checking...");
            }
            return 0;
        }
        if (LOWORD(wparam) == kSave) {
            if (state->transfer) return 0;
            ReadControls(*state);
            state->saved = true;
            state->done = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wparam) == kCancel) {
            state->done = true;
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_DRAWITEM:
        DrawOwnerItem(*state, *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
        return TRUE;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lparam)->idFrom == kTabs && reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
            ReadControls(*state);
            const auto page = TabCtrl_GetCurSel(state->tabs);
            if (page == 1) RebuildOsdOrderList(*state);
            ShowPage(*state, page);
            if (page == 1) InvalidateRect(state->osd_preview, nullptr, TRUE);
            return 0;
        }
        break;
    case WM_CLOSE:
        state->done = true;
        DestroyWindow(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        if (dc != nullptr) FillRect(dc, &paint.rcPaint, state->background_brush);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        const auto dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, WinColor(state->palette.text));
        SetBkColor(dc, WinColor(state->palette.background));
        return reinterpret_cast<LRESULT>(state->background_brush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, WinColor(state->palette.text));
        SetBkColor(dc, WinColor(state->palette.surface_alternate));
        return reinterpret_cast<LRESULT>(state->field_brush);
    }
    case WM_DESTROY:
        state->done = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

bool ShowSettingsWindow(const HWND owner, AppSettings& settings, const SensorSnapshot& snapshot) noexcept {
    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
    static_cast<void>(InitCommonControlsEx(&common_controls));

    const auto instance = GetModuleHandleW(nullptr);
    auto owner_dpi = GetDpiForWindow(owner);
    if (owner_dpi == 0U) owner_dpi = 96U;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(101),
        IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXICON, owner_dpi),
        GetSystemMetricsForDpi(SM_CYICON, owner_dpi),
        LR_DEFAULTCOLOR | LR_SHARED));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(101),
        IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, owner_dpi),
        GetSystemMetricsForDpi(SM_CYSMICON, owner_dpi),
        LR_DEFAULTCOLOR | LR_SHARED));
    window_class.lpszClassName = kClassName;
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    DialogState state{};
    state.owner = owner;
    state.draft = settings;
    state.snapshot = &snapshot;
    state.dpi = owner_dpi;
    state.palette = PaletteFor(state.draft.theme, state.draft.text_color_rgb, state.draft.high_contrast);
    RecreateFont(state);
    RecreateBrushes(state);
    MONITORINFO monitor_information{};
    monitor_information.cbSize = sizeof(monitor_information);
    const auto monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &monitor_information)) static_cast<void>(SystemParametersInfoW(SPI_GETWORKAREA, 0U, &monitor_information.rcWork, 0U));
    constexpr DWORD window_ex_style = WS_EX_DLGMODALFRAME;
    constexpr DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_HSCROLL | WS_VSCROLL | WS_CLIPCHILDREN;
    RECT desired_frame{0, 0, Scale(state, kLogicalDialogWidth), Scale(state, kLogicalDialogHeight)};
    if (!AdjustWindowRectExForDpi(&desired_frame, window_style, FALSE, window_ex_style, state.dpi)) {
        desired_frame.right = Scale(state, kLogicalDialogWidth);
        desired_frame.bottom = Scale(state, kLogicalDialogHeight);
    }
    const auto desired_width = static_cast<int>(desired_frame.right - desired_frame.left);
    const auto desired_height = static_cast<int>(desired_frame.bottom - desired_frame.top);
    const auto available_width = std::max(1, static_cast<int>(monitor_information.rcWork.right - monitor_information.rcWork.left));
    const auto available_height = std::max(1, static_cast<int>(monitor_information.rcWork.bottom - monitor_information.rcWork.top));
    const auto width = std::min(desired_width, available_width);
    const auto height = std::min(desired_height, available_height);
    const auto x = monitor_information.rcWork.left + (available_width - width) / 2;
    const auto y = monitor_information.rcWork.top + (available_height - height) / 2;
    state.window = CreateWindowExW(
        window_ex_style,
        kClassName,
        L"HardwareScope settings",
        window_style,
        x,
        y,
        width,
        height,
        owner,
        nullptr,
        instance,
        &state);
    if (state.window == nullptr) {
        DeleteDialogResources(state);
        return false;
    }
    static_cast<void>(SendMessageW(state.window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(window_class.hIcon)));
    static_cast<void>(SendMessageW(state.window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(window_class.hIconSm)));
    const BOOL dark = state.draft.theme != Theme::light ? TRUE : FALSE;
    static_cast<void>(DwmSetWindowAttribute(state.window, 20U, &dark, sizeof(dark)));
    static_cast<void>(SetWindowTheme(state.window, state.draft.theme != Theme::light ? L"DarkMode_Explorer" : L"Explorer", nullptr));
    BuildControls(state);
    Relayout(state);
    EnableWindow(owner, FALSE);
    ShowWindow(state.window, SW_SHOW);
    UpdateWindow(state.window);
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0U, 0U) > 0) {
        if (!IsDialogMessageW(state.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    DeleteDialogResources(state);
    if (state.saved) settings = state.draft;
    return state.saved;
}

} // namespace hardwarescope
