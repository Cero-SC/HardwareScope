#pragma once

#include "hardwarescope/sensor_snapshot.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hardwarescope {

inline constexpr wchar_t kSensorBridgeName[] = L"Global\\HardwareScope.SensorBridge.v4";
inline constexpr wchar_t kFpsControlPipeName[] = L"\\\\.\\pipe\\HardwareScope.MonitoringControl.v4";
inline constexpr std::uint32_t kSensorBridgeMagic = 0x4853'5632U;
inline constexpr std::uint32_t kSensorBridgeVersion = 4U;

struct SharedSensorSnapshot final {
    std::uint32_t magic{};
    std::uint32_t version{};
    volatile LONG64 sequence{};
    std::uint64_t captured_qpc{};
    std::uint32_t collection_microseconds{};
    std::uint32_t count{};
    std::array<SensorValue, kMaxSensors> sensors{};
};

struct SharedFpsControl final {
    std::uint32_t magic{};
    std::uint32_t version{};
    volatile LONG target_process_id{};
    volatile LONG smoothing_milliseconds{500};
    volatile LONG hardware_polling_milliseconds{750};
};

static_assert(offsetof(SharedSensorSnapshot, sequence) % alignof(LONG64) == 0U);

class SensorBridgePublisher final {
public:
    SensorBridgePublisher() = default;
    ~SensorBridgePublisher();

    SensorBridgePublisher(const SensorBridgePublisher&) = delete;
    SensorBridgePublisher& operator=(const SensorBridgePublisher&) = delete;

    [[nodiscard]] bool Initialize() noexcept;
    void Publish(const SensorSnapshot& snapshot) noexcept;
    [[nodiscard]] std::uint32_t RequestedFpsTarget() noexcept;
    [[nodiscard]] std::uint32_t RequestedFpsSmoothing() noexcept;
    [[nodiscard]] std::uint32_t RequestedHardwarePollingInterval() noexcept;
    void Close() noexcept;

private:
    HANDLE mapping_{};
    SharedSensorSnapshot* shared_{};
    void PollFpsControl() noexcept;
    std::uint32_t requested_fps_target_{};
    std::uint32_t requested_fps_smoothing_{500U};
    std::uint32_t requested_hardware_polling_interval_{750U};
    std::wstring expected_control_server_;
    HANDLE pending_control_pipe_{};
    std::chrono::steady_clock::time_point last_control_request_{};
};

class SensorBridgeClient final {
public:
    SensorBridgeClient() = default;
    ~SensorBridgeClient();

    SensorBridgeClient(const SensorBridgeClient&) = delete;
    SensorBridgeClient& operator=(const SensorBridgeClient&) = delete;

    [[nodiscard]] bool Collect(SensorSnapshot& destination) noexcept;
    [[nodiscard]] bool CollectFrameRate(SensorValue& destination) noexcept;
    [[nodiscard]] bool CollectFrameRates(SensorSnapshot& destination) noexcept;
    [[nodiscard]] bool SetFpsTarget(
        std::uint32_t process_id,
        std::uint32_t smoothing_milliseconds,
        std::uint32_t hardware_polling_milliseconds) noexcept;
    [[nodiscard]] bool Available() const noexcept { return shared_ != nullptr; }
    void Close() noexcept;

private:
    [[nodiscard]] bool Connect() noexcept;

    HANDLE mapping_{};
    const SharedSensorSnapshot* shared_{};
    HANDLE control_pipe_{};
    std::chrono::steady_clock::time_point last_connect_attempt_{};
    std::unique_ptr<SensorSnapshot> copy_buffer_;
};

} // namespace hardwarescope
