#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace hardwarescope {

struct GameProcess final {
    std::uint32_t process_id{};
    std::array<wchar_t, 64U> application{};
    bool known_game{};
};

[[nodiscard]] bool IsExcludedGameExecutable(std::wstring_view application) noexcept;
[[nodiscard]] bool IsKnownGameExecutable(std::wstring_view application, std::wstring_view path) noexcept;
[[nodiscard]] GameProcess FindGameProcess(std::uint32_t own_process_id, std::uint32_t current_process_id = 0U,
    bool allow_unknown_fullscreen = false) noexcept;

} // namespace hardwarescope
