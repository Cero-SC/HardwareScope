#pragma once

#include "hardwarescope/pawn_io.hpp"
#include "hardwarescope/sensor_snapshot.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hardwarescope {

[[nodiscard]] bool IsNct6687DrBoardIdentity(std::wstring_view manufacturer, std::wstring_view product) noexcept;

class Nct6687Provider final {
public:
    Nct6687Provider() = default;
    ~Nct6687Provider();

    Nct6687Provider(const Nct6687Provider&) = delete;
    Nct6687Provider& operator=(const Nct6687Provider&) = delete;

    [[nodiscard]] bool Initialize(HINSTANCE resources) noexcept;
    void Close() noexcept;
    void Collect(SensorSnapshot& snapshot) noexcept;

    [[nodiscard]] bool Available() const noexcept { return available_; }
    [[nodiscard]] std::uint16_t BasePort() const noexcept { return base_port_; }
    [[nodiscard]] const std::wstring& ChipName() const noexcept { return chip_name_; }
    [[nodiscard]] const std::array<std::uint8_t, 2U>& DetectedChipIds() const noexcept { return detected_chip_ids_; }
    [[nodiscard]] const std::array<std::uint8_t, 2U>& DetectedRevisions() const noexcept { return detected_revisions_; }
    [[nodiscard]] HRESULT LastError() const noexcept { return pawn_io_.LastError(); }

private:
    friend struct SensorProviderTestAccess;
    [[nodiscard]] bool ExecuteOne(const char* function, std::uint64_t input, std::uint64_t& output) noexcept;
    [[nodiscard]] bool ExecuteNone(const char* function) noexcept;
    [[nodiscard]] bool SelectSlot(std::uint64_t slot) noexcept;
    [[nodiscard]] bool ReadConfigByte(std::uint8_t address, std::uint8_t& value) noexcept;
    [[nodiscard]] bool ReadConfigWord(std::uint8_t address, std::uint16_t& value) noexcept;
    [[nodiscard]] bool WriteConfigByte(std::uint8_t address, std::uint8_t value) noexcept;
    [[nodiscard]] bool ReadPort(std::uint16_t port, std::uint8_t& value) noexcept;
    [[nodiscard]] bool WritePort(std::uint16_t port, std::uint8_t value) noexcept;
    [[nodiscard]] bool ReadEcByte(std::uint16_t address, std::uint8_t& value) noexcept;
    [[nodiscard]] bool ReadBankedByte(std::uint16_t address, std::uint8_t& value) noexcept;
    [[nodiscard]] bool AcquireMutex(DWORD timeout_ms) noexcept;
    void ReleaseMutex() noexcept;
    void Refresh() noexcept;

    PawnIoExecutor pawn_io_{};
    HANDLE isa_mutex_{};
    std::uint16_t base_port_{};
    std::wstring chip_name_;
    std::array<std::uint8_t, 2U> detected_chip_ids_{};
    std::array<std::uint8_t, 2U> detected_revisions_{};
    std::array<SensorValue, 32U> cached_sensors_{};
    std::size_t cached_sensor_count_{};
    std::chrono::steady_clock::time_point last_refresh_{};
    bool available_{};
    bool ec_register_space_{};
    bool nct6687_dr_layout_{};
    bool mutex_owned_{};
};

} // namespace hardwarescope
