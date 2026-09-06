#pragma once

#include "hardwarescope/pawn_io.hpp"
#include "hardwarescope/sensor_snapshot.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace hardwarescope {

[[nodiscard]] double ConvertDdr5TemperatureRaw(std::uint16_t raw) noexcept;
[[nodiscard]] bool IsPlausibleDdr5Temperature(double celsius) noexcept;

class Ddr5TemperatureProvider final {
public:
    Ddr5TemperatureProvider() = default;
    ~Ddr5TemperatureProvider();

    Ddr5TemperatureProvider(const Ddr5TemperatureProvider&) = delete;
    Ddr5TemperatureProvider& operator=(const Ddr5TemperatureProvider&) = delete;

    [[nodiscard]] bool Initialize(HINSTANCE resources) noexcept;
    void Close() noexcept;
    void Collect(SensorSnapshot& snapshot) noexcept;

    [[nodiscard]] bool Available() const noexcept { return available_; }
    [[nodiscard]] HRESULT LastError() const noexcept { return last_error_; }

private:
    friend struct SensorProviderTestAccess;
    struct Bus final {
        PawnIoExecutor executor{};
        bool available{};
    };

    [[nodiscard]] static bool SelectPiix4Port(Bus& bus, std::uint64_t port) noexcept;
    [[nodiscard]] static bool ReadByte(Bus& bus, std::uint8_t address, std::uint8_t command, std::uint8_t& value) noexcept;
    [[nodiscard]] static bool ReadWord(Bus& bus, std::uint8_t address, std::uint8_t command, std::uint16_t& value) noexcept;
    [[nodiscard]] bool AcquireMutex(HANDLE mutex, bool& owned, DWORD timeout_ms) noexcept;
    static void ReleaseMutex(HANDLE mutex, bool& owned) noexcept;
    void Refresh() noexcept;

    std::array<Bus, 2U> buses_{};
    HANDLE smbus_mutex_{};
    HANDLE pci_mutex_{};
    std::array<SensorValue, 16U> cached_sensors_{};
    std::size_t cached_sensor_count_{};
    std::chrono::steady_clock::time_point last_refresh_{};
    HRESULT last_error_{E_FAIL};
    bool available_{};
    bool smbus_mutex_owned_{};
    bool pci_mutex_owned_{};
};

} // namespace hardwarescope
