#include "hardwarescope/update_prompt_window.hpp"

#include "hardwarescope/app_commands.hpp"
#include "hardwarescope/ui_palette.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <string>

namespace hardwarescope {
namespace {

enum class PromptMode : std::uint8_t { update, notice };

struct PromptState final {
    HWND owner{};
    HWND window{};
    HINSTANCE instance{};
    AppSettings settings{};
    UiPalette palette{};
    PromptMode mode{PromptMode::update};
    UpdateManifest manifest{};
    std::wstring heading;
    std::wstring message;
    bool warning{};
    bool done{};
    bool owner_was_enabled{};
    int selected_reminder{kUpdateIn24HoursRadio};
    int hovered{};
    UINT dpi{96U};
    UpdatePromptResult result{};
};

COLORREF WinColor(const std::uint32_t rgb) noexcept {
    return RGB((rgb >> 16U) & 0xFFU, (rgb >> 8U) & 0xFFU, rgb & 0xFFU);
}

int Scale(const PromptState& state, const int value) noexcept {
    return MulDiv(value, static_cast<int>(state.dpi == 0U ? 96U : state.dpi), 96);
}

RECT ScaledRect(const PromptState& state, const int left, const int top, const int right, const int bottom) noexcept {
    return RECT{Scale(state, left), Scale(state, top), Scale(state, right), Scale(state, bottom)};
}

RECT CloseBounds(const PromptState& state) noexcept { return ScaledRect(state, 436, 0, 480, 44); }
RECT UpdateBounds(const PromptState& state) noexcept { return ScaledRect(state, 250, 294, 458, 334); }
RECT LaterBounds(const PromptState& state) noexcept { return ScaledRect(state, 22, 294, 230, 334); }
RECT NoticeCloseBounds(const PromptState& state) noexcept { return ScaledRect(state, 278, 226, 458, 266); }

RECT ReminderBounds(const PromptState& state, const int index) noexcept {
    constexpr int left = 22;
    constexpr int gap = 8;
    constexpr int width = 103;
    return ScaledRect(state, left + index * (width + gap), 222, left + index * (width + gap) + width, 264);
}

bool ContainsPoint(const RECT& rectangle, const POINT point) noexcept {
    return PtInRect(&rectangle, point) != FALSE;
}

int HitControl(const PromptState& state, const POINT point) noexcept {
    if (ContainsPoint(CloseBounds(state), point)) return IDCANCEL;
    if (state.mode == PromptMode::notice) return ContainsPoint(NoticeCloseBounds(state), point) ? IDOK : 0;
    if (ContainsPoint(UpdateBounds(state), point)) return kUpdateNowButton;
    if (ContainsPoint(LaterBounds(state), point)) return kUpdateLaterButton;
    constexpr std::array reminders{kUpdateIn24HoursRadio, kUpdateIn3DaysRadio, kUpdateIn1WeekRadio, kSkipUpdateRadio};
    for (int index{}; index < static_cast<int>(reminders.size()); ++index) {
        if (ContainsPoint(ReminderBounds(state, index), point)) return reminders[static_cast<std::size_t>(index)];
    }
    return 0;
}

void Finish(PromptState& state, const int command) noexcept {
    if (state.mode == PromptMode::notice) {
        state.done = true;
        DestroyWindow(state.window);
        return;
    }
    if (command == kUpdateNowButton) {
        state.result = UpdatePromptResult{UpdatePromptAction::install_now, {}};
    } else if (state.selected_reminder == kSkipUpdateRadio) {
        state.result = UpdatePromptResult{UpdatePromptAction::skip_version, {}};
    } else if (state.selected_reminder == kUpdateIn3DaysRadio) {
        state.result = UpdatePromptResult{UpdatePromptAction::remind_later, std::chrono::hours{72}};
    } else if (state.selected_reminder == kUpdateIn1WeekRadio) {
        state.result = UpdatePromptResult{UpdatePromptAction::remind_later, std::chrono::hours{24 * 7}};
    } else {
        state.result = UpdatePromptResult{};
    }
    state.done = true;
    DestroyWindow(state.window);
}

void DrawTextBlock(
    const HDC dc,
    const HFONT font,
    const COLORREF color,
    std::wstring_view text,
    RECT bounds,
    const UINT flags) noexcept {
    const auto old_font = SelectObject(dc, font);
    const auto old_color = SetTextColor(dc, color);
    const auto old_mode = SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &bounds, flags);
    SetBkMode(dc, old_mode);
    SetTextColor(dc, old_color);
    SelectObject(dc, old_font);
}

void DrawButton(
    const PromptState& state,
    const HDC dc,
    const RECT bounds,
    std::wstring_view text,
    const int command,
    const bool primary = false,
    const bool selected = false) noexcept {
    const auto background_color = primary ? state.palette.accent
        : selected ? state.palette.selection
        : command == state.hovered ? state.palette.hover : state.palette.surface;
    const auto brush = CreateSolidBrush(WinColor(background_color));
    const auto pen = CreatePen(PS_SOLID, std::max(1, Scale(state, selected ? 2 : 1)),
        WinColor(selected ? state.palette.accent : state.palette.line));
    const auto old_brush = SelectObject(dc, brush);
    const auto old_pen = SelectObject(dc, pen);
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, Scale(state, 8), Scale(state, 8));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
    const auto font = CreateFontW(-Scale(state, 14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto text_bounds = bounds;
    DrawTextBlock(dc, font, WinColor(primary ? state.palette.selection_text : state.palette.text), text, text_bounds,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    DeleteObject(font);
}

void Render(PromptState& state, const HDC destination) noexcept {
    RECT client{};
    GetClientRect(state.window, &client);
    const auto width = client.right;
    const auto height = client.bottom;
    const auto memory = CreateCompatibleDC(destination);
    const auto bitmap = CreateCompatibleBitmap(destination, width, height);
    const auto old_bitmap = SelectObject(memory, bitmap);
    const auto background = CreateSolidBrush(WinColor(state.palette.background));
    const auto header = CreateSolidBrush(WinColor(state.palette.header));
    const auto line = CreatePen(PS_SOLID, std::max(1, Scale(state, 1)), WinColor(state.palette.line));
    FillRect(memory, &client, background);
    auto header_bounds = client;
    header_bounds.bottom = Scale(state, 44);
    FillRect(memory, &header_bounds, header);
    const auto old_pen = SelectObject(memory, line);
    MoveToEx(memory, 0, header_bounds.bottom - 1, nullptr);
    LineTo(memory, width, header_bounds.bottom - 1);
    SelectObject(memory, old_pen);

    const auto small_font = CreateFontW(-Scale(state, 13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const auto heading_font = CreateFontW(-Scale(state, 22), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const auto body_font = CreateFontW(-Scale(state, 14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto title = ScaledRect(state, 18, 0, 420, 44);
    DrawTextBlock(memory, small_font, WinColor(state.palette.text), L"HardwareScope update", title,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    auto close = CloseBounds(state);
    if (state.hovered == IDCANCEL) {
        const auto hover = CreateSolidBrush(WinColor(state.palette.hover));
        FillRect(memory, &close, hover);
        DeleteObject(hover);
    }
    DrawTextBlock(memory, heading_font, WinColor(state.palette.text), L"×", close,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    auto heading = ScaledRect(state, 22, 64, 458, 100);
    DrawTextBlock(memory, heading_font, WinColor(state.warning ? 0xFFB347U : state.palette.accent), state.heading, heading,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    auto message = state.mode == PromptMode::update
        ? ScaledRect(state, 22, 108, 458, 174)
        : ScaledRect(state, 22, 108, 458, 204);
    DrawTextBlock(memory, body_font, WinColor(state.palette.text), state.message, message,
        DT_LEFT | DT_TOP | DT_WORDBREAK);

    if (state.mode == PromptMode::update) {
        auto reminder = ScaledRect(state, 22, 188, 458, 216);
        DrawTextBlock(memory, small_font, WinColor(state.palette.muted), L"REMIND ME", reminder,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        constexpr std::array labels{L"24 hours", L"3 days", L"1 week", L"Never"};
        constexpr std::array commands{kUpdateIn24HoursRadio, kUpdateIn3DaysRadio, kUpdateIn1WeekRadio, kSkipUpdateRadio};
        for (int index{}; index < static_cast<int>(labels.size()); ++index) {
            DrawButton(state, memory, ReminderBounds(state, index), labels[static_cast<std::size_t>(index)],
                commands[static_cast<std::size_t>(index)], false,
                state.selected_reminder == commands[static_cast<std::size_t>(index)]);
        }
        DrawButton(state, memory, LaterBounds(state), L"Remind me later", kUpdateLaterButton);
        DrawButton(state, memory, UpdateBounds(state), L"Update now", kUpdateNowButton, true);
    } else {
        DrawButton(state, memory, NoticeCloseBounds(state), L"Close", IDOK, true);
    }

    const auto border_pen = CreatePen(PS_SOLID, std::max(1, Scale(state, 1)), WinColor(state.palette.line));
    const auto previous_border_pen = SelectObject(memory, border_pen);
    const auto previous_border_brush = SelectObject(memory, GetStockObject(HOLLOW_BRUSH));
    RoundRect(memory, 0, 0, width - 1, height - 1, Scale(state, 10), Scale(state, 10));
    SelectObject(memory, previous_border_brush);
    SelectObject(memory, previous_border_pen);
    DeleteObject(border_pen);

    BitBlt(destination, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    DeleteObject(small_font);
    DeleteObject(heading_font);
    DeleteObject(body_font);
    DeleteObject(line);
    DeleteObject(background);
    DeleteObject(header);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

LRESULT CALLBACK PromptProcedure(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) noexcept {
    auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<PromptState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST: {
        const auto result = DefWindowProcW(window, message, wparam, lparam);
        if (result != HTCLIENT) return result;
        POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &screen);
        if (ContainsPoint(CloseBounds(*state), screen)) return HTCLIENT;
        return screen.y < Scale(*state, 44) ? HTCAPTION : HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        const auto next = HitControl(*state, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        if (next != state->hovered) {
            state->hovered = next;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0U};
        static_cast<void>(TrackMouseEvent(&tracking));
        return 0;
    }
    case WM_MOUSELEAVE:
        state->hovered = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const auto command = HitControl(*state, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        if (command != 0) static_cast<void>(SendMessageW(window, WM_COMMAND, command, 0U));
        return 0;
    }
    case WM_COMMAND: {
        const auto command = LOWORD(wparam);
        if (command == kUpdateIn24HoursRadio || command == kUpdateIn3DaysRadio
            || command == kUpdateIn1WeekRadio || command == kSkipUpdateRadio) {
            state->selected_reminder = command;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (command == kUpdateNowButton || command == kUpdateLaterButton || command == IDOK || command == IDCANCEL) {
            Finish(*state, command);
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) Finish(*state, IDCANCEL);
        else if (wparam == VK_RETURN) Finish(*state, state->mode == PromptMode::update ? kUpdateNowButton : IDOK);
        return 0;
    case WM_CLOSE:
        Finish(*state, IDCANCEL);
        return 0;
    case WM_DPICHANGED: {
        state->dpi = HIWORD(wparam);
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        static_cast<void>(SetWindowPos(window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOACTIVATE | SWP_NOZORDER));
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        Render(*state, dc);
        EndPaint(window, &paint);
        return 0;
    }
#if HARDWARESCOPE_INTERNAL_TEST_HOOKS
    case kQueryUpdatePromptChoicesMessage:
        return state->mode == PromptMode::update ? 0x3F : 0;
    case kQueryUpdatePromptSelectionMessage:
        return state->selected_reminder;
#endif
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        state->window = nullptr;
        return DefWindowProcW(window, message, wparam, lparam);
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool RegisterPromptClass(const HINSTANCE instance) noexcept {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.hInstance = instance;
    type.lpszClassName = kUpdatePromptWindowClass;
    type.lpfnWndProc = &PromptProcedure;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED));
    type.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));
    return RegisterClassExW(&type) != 0U || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool CreateAndRun(PromptState& state) noexcept {
    if (!RegisterPromptClass(state.instance)) return false;
    state.palette = PaletteFor(state.settings.theme, state.settings.text_color_rgb, state.settings.high_contrast);
    const auto owner_dpi = state.owner == nullptr ? GetDpiForSystem() : GetDpiForWindow(state.owner);
    state.dpi = owner_dpi == 0U ? 96U : owner_dpi;
    const auto logical_height = state.mode == PromptMode::update ? 356 : 288;
    const auto width = Scale(state, 480);
    const auto height = Scale(state, logical_height);
    RECT owner_bounds{};
    if (state.owner == nullptr || !GetWindowRect(state.owner, &owner_bounds)) {
        static_cast<void>(SystemParametersInfoW(SPI_GETWORKAREA, 0U, &owner_bounds, 0U));
    }
    const auto x = owner_bounds.left + std::max(0L, (owner_bounds.right - owner_bounds.left - width) / 2L);
    const auto y = owner_bounds.top + std::max(0L, (owner_bounds.bottom - owner_bounds.top - height) / 2L);
    state.window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kUpdatePromptWindowClass,
        L"HardwareScope update",
        WS_POPUP,
        x, y, width, height,
        state.owner, nullptr, state.instance, &state);
    if (state.window == nullptr) return false;
    constexpr DWORD dark_mode = 20U;
    constexpr DWORD corner_preference = 33U;
    constexpr DWORD rounded = 2U;
    const BOOL dark = state.settings.theme == Theme::light ? FALSE : TRUE;
    static_cast<void>(DwmSetWindowAttribute(state.window, dark_mode, &dark, sizeof(dark)));
    static_cast<void>(DwmSetWindowAttribute(state.window, corner_preference, &rounded, sizeof(rounded)));
    state.owner_was_enabled = state.owner != nullptr && IsWindowEnabled(state.owner) != FALSE;
    if (state.owner_was_enabled) EnableWindow(state.owner, FALSE);
    ShowWindow(state.window, SW_SHOWNORMAL);
    UpdateWindow(state.window);
    SetForegroundWindow(state.window);
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0U, 0U) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (state.window != nullptr) DestroyWindow(state.window);
    if (state.owner_was_enabled && state.owner != nullptr && IsWindow(state.owner)) {
        EnableWindow(state.owner, TRUE);
        SetForegroundWindow(state.owner);
    }
    return true;
}

} // namespace

UpdatePromptResult ShowUpdatePromptWindow(
    const HWND owner,
    const HINSTANCE instance,
    const UpdateManifest& manifest,
    const AppSettings& settings) noexcept {
    PromptState state{};
    state.owner = owner;
    state.instance = instance;
    state.settings = settings;
    state.manifest = manifest;
    wchar_t heading[128]{};
    static_cast<void>(swprintf_s(heading, L"Version %u.%u.%u is ready",
        manifest.version.major, manifest.version.minor, manifest.version.patch));
    state.heading = heading;
    state.message = L"A new release is available. Update now to download and verify it, or choose when HardwareScope should remind you again.";
    static_cast<void>(CreateAndRun(state));
    return state.result;
}

void ShowUpdateNoticeWindow(
    const HWND owner,
    const HINSTANCE instance,
    const AppSettings& settings,
    const std::wstring_view heading,
    const std::wstring_view message,
    const bool warning) noexcept {
    PromptState state{};
    state.owner = owner;
    state.instance = instance;
    state.settings = settings;
    state.mode = PromptMode::notice;
    state.heading.assign(heading);
    state.message.assign(message);
    state.warning = warning;
    static_cast<void>(CreateAndRun(state));
}

} // namespace hardwarescope
