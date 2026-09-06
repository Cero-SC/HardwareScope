#include "hardwarescope/game_detector.hpp"

#include <algorithm>
#include <array>
#include <cwchar>
#include <string>

namespace hardwarescope {
namespace {

constexpr std::array<std::wstring_view, 37U> kExcluded{
    L"HardwareScope.exe", L"HardwareScopeNative.exe", L"dwm.exe", L"explorer.exe", L"SearchHost.exe",
    L"ApplicationFrameHost.exe", L"ShellExperienceHost.exe", L"StartMenuExperienceHost.exe", L"TextInputHost.exe",
    L"LockApp.exe", L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe", L"ChatGPT.exe",
    L"Discord.exe", L"slack.exe", L"Teams.exe", L"ms-teams.exe", L"Spotify.exe", L"Code.exe", L"devenv.exe",
    L"WindowsTerminal.exe", L"notepad.exe", L"obs64.exe", L"Steam.exe", L"steamwebhelper.exe", L"EpicGamesLauncher.exe",
    L"EADesktop.exe", L"UbisoftConnect.exe", L"upc.exe", L"Battle.net.exe", L"GalaxyClient.exe", L"GameBar.exe",
    L"RadeonSoftware.exe", L"Codex.exe"};

constexpr std::array<std::wstring_view, 13U> kGamePaths{
    L"\\steamapps\\common\\", L"\\epic games\\", L"\\gog games\\", L"\\gog galaxy\\games\\",
    L"\\xboxgames\\", L"\\riot games\\", L"\\ea games\\", L"\\ubisoft\\games\\",
    L"\\starcitizen\\", L"\\world of warcraft\\", L"\\diablo ", L"\\overwatch\\", L"\\call of duty\\"};

bool EqualsInsensitive(const std::wstring_view left, const std::wstring_view right) noexcept {
    return left.size() == right.size() && _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

bool ContainsInsensitive(const std::wstring_view text, const std::wstring_view fragment) noexcept {
    if (fragment.empty() || fragment.size() > text.size()) return false;
    for (std::size_t index = 0U; index + fragment.size() <= text.size(); ++index) {
        if (_wcsnicmp(text.data() + index, fragment.data(), fragment.size()) == 0) return true;
    }
    return false;
}

bool EndsWithInsensitive(const std::wstring_view text, const std::wstring_view suffix) noexcept {
    return suffix.size() <= text.size() && EqualsInsensitive(text.substr(text.size() - suffix.size()), suffix);
}

std::wstring ProcessPath(const std::uint32_t process_id) {
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) return {};
    std::array<wchar_t, 32'768U> path{};
    DWORD length = static_cast<DWORD>(path.size());
    const auto success = QueryFullProcessImageNameW(process, 0U, path.data(), &length);
    CloseHandle(process);
    return success ? std::wstring{path.data(), length} : std::wstring{};
}

std::wstring_view FileName(const std::wstring_view path) noexcept {
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring_view::npos ? path : path.substr(separator + 1U);
}

bool IsGameSizedWindow(const HWND window, const std::wstring_view application) noexcept {
    if (window == nullptr || IsExcludedGameExecutable(application)) return false;
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return false;
    const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO information{sizeof(information)};
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &information)) return false;
    const auto monitor_width = std::max(1L, information.rcMonitor.right - information.rcMonitor.left);
    const auto monitor_height = std::max(1L, information.rcMonitor.bottom - information.rcMonitor.top);
    const auto width = std::max(0L, std::min(bounds.right, information.rcMonitor.right) - std::max(bounds.left, information.rcMonitor.left));
    const auto height = std::max(0L, std::min(bounds.bottom, information.rcMonitor.bottom) - std::max(bounds.top, information.rcMonitor.top));
    const auto coverage = static_cast<double>(width) * static_cast<double>(height)
        / (static_cast<double>(monitor_width) * static_cast<double>(monitor_height));
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    return coverage >= 0.85 && ((style & WS_CAPTION) == 0 || coverage >= 0.97);
}

GameProcess ResolveCandidate(const std::uint32_t process_id, const bool allow_sized_window, const HWND window) noexcept {
    GameProcess result{};
    if (process_id == 0U) return result;
    const auto path = ProcessPath(process_id);
    if (path.empty()) return result;
    const auto name = FileName(path);
    if (IsExcludedGameExecutable(name)) return result;
    const auto known = IsKnownGameExecutable(name, path);
    if (!known && (!allow_sized_window || !IsGameSizedWindow(window, name))) return result;
    result.process_id = process_id;
    result.known_game = known;
    static_cast<void>(wcsncpy_s(result.application.data(), result.application.size(), std::wstring{name}.c_str(), _TRUNCATE));
    return result;
}

struct EnumerationContext final {
    std::uint32_t own_process_id{};
    GameProcess result{};
};

BOOL CALLBACK EnumerateGames(const HWND window, const LPARAM parameter) noexcept {
    if (!IsWindowVisible(window)) return TRUE;
    auto& context = *reinterpret_cast<EnumerationContext*>(parameter);
    DWORD process_id{};
    static_cast<void>(GetWindowThreadProcessId(window, &process_id));
    if (process_id == 0U || process_id == context.own_process_id) return TRUE;
    context.result = ResolveCandidate(process_id, false, window);
    return context.result.process_id == 0U ? TRUE : FALSE;
}

} // namespace

bool IsExcludedGameExecutable(const std::wstring_view application) noexcept {
    if (application.size() >= 11U && _wcsnicmp(application.data(), L"PowerToys.", 11U) == 0) return true;
    return std::any_of(kExcluded.begin(), kExcluded.end(), [application](const auto excluded) { return EqualsInsensitive(application, excluded); });
}

bool IsKnownGameExecutable(const std::wstring_view application, const std::wstring_view path) noexcept {
    if (IsExcludedGameExecutable(application)) return false;
    if (EndsWithInsensitive(application, L"-Win64-Shipping.exe") || EndsWithInsensitive(application, L"-WinGDK-Shipping.exe")
        || ContainsInsensitive(application, L"StarCitizen")) return true;
    return std::any_of(kGamePaths.begin(), kGamePaths.end(), [path](const auto fragment) { return ContainsInsensitive(path, fragment); });
}

GameProcess FindGameProcess(const std::uint32_t own_process_id, const std::uint32_t current_process_id, const bool allow_unknown_fullscreen) noexcept {
    const auto foreground = GetForegroundWindow();
    DWORD foreground_process_id{};
    if (foreground != nullptr) static_cast<void>(GetWindowThreadProcessId(foreground, &foreground_process_id));
    if (foreground_process_id != 0U && foreground_process_id != own_process_id) {
        const auto candidate = ResolveCandidate(foreground_process_id, allow_unknown_fullscreen, foreground);
        if (candidate.process_id != 0U) return candidate;
    }
    if (current_process_id != 0U && current_process_id != own_process_id) {
        const auto current = ResolveCandidate(current_process_id, false, nullptr);
        if (current.process_id != 0U) return current;
    }
    EnumerationContext context{own_process_id, {}};
    static_cast<void>(EnumWindows(&EnumerateGames, reinterpret_cast<LPARAM>(&context)));
    return context.result;
}

} // namespace hardwarescope
