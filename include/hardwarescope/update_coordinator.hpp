#pragma once

#include "hardwarescope/update_client.hpp"

#include <windows.h>

#include <filesystem>
#include <optional>

namespace hardwarescope {

constexpr UINT kManualUpdateRequestMessage = WM_APP + 51U;
constexpr UINT kUpdateCompletedMessage = WM_APP + 52U;

enum class UpdateCompletionStatus : std::uint8_t {
    failed,
    current,
    available,
    ready,
};

struct UpdateCompletion final {
    UpdateCompletionStatus status{UpdateCompletionStatus::failed};
    UpdateManifest manifest{};
    std::filesystem::path installer;
    std::uint32_t system_error{};
    bool automatic{};
};

[[nodiscard]] bool BeginNativeUpdateCheck(
    HWND notification_window,
    bool automatic,
    std::optional<SemanticVersion> skipped_version = std::nullopt) noexcept;
[[nodiscard]] std::optional<UpdateCompletion> TakeNativeUpdateCompletion() noexcept;
[[nodiscard]] bool BeginNativeUpdateDownload(HWND notification_window, UpdateManifest manifest) noexcept;

} // namespace hardwarescope
