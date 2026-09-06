#pragma once

#include "hardwarescope/pawn_io.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hardwarescope {

struct AmdZenTemperatures final {
    double package_celsius{};
    // Physical CCD slots, never compacted. Zero means unavailable, not 0 degrees.
    std::array<double, 8> ccd_celsius{};
    std::size_t ccd_count{};
};

class AmdZenProvider final {
public:
    AmdZenProvider() = default;
    ~AmdZenProvider();

    AmdZenProvider(const AmdZenProvider&) = delete;
    AmdZenProvider& operator=(const AmdZenProvider&) = delete;

    [[nodiscard]] bool Initialize(HINSTANCE resources) noexcept;
    [[nodiscard]] bool ReadTemperatures(AmdZenTemperatures& temperatures) noexcept;
    void Close() noexcept;

    [[nodiscard]] bool Available() const noexcept { return available_; }
    [[nodiscard]] std::uint32_t Family() const noexcept { return family_; }
    [[nodiscard]] std::uint32_t Model() const noexcept { return model_; }
    [[nodiscard]] const std::wstring& ProcessorName() const noexcept { return processor_name_; }
    [[nodiscard]] HRESULT LastError() const noexcept { return pawn_io_.LastError(); }

private:
    [[nodiscard]] bool ReadSmn(std::uint32_t offset, std::uint32_t& value) noexcept;
    [[nodiscard]] bool AcquirePciMutex(DWORD timeout_ms) noexcept;
    void ReleasePciMutex() noexcept;

    PawnIoExecutor pawn_io_{};
    HANDLE pci_mutex_{};
    std::uint32_t family_{};
    std::uint32_t model_{};
    std::wstring processor_name_;
    bool available_{};
    bool mutex_owned_{};
};

} // namespace hardwarescope
