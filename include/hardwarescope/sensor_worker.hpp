#pragma once

#include "hardwarescope/amd_gpu_provider.hpp"
#include "hardwarescope/nvidia_gpu_provider.hpp"
#include "hardwarescope/privileged_sensor_collector.hpp"
#include "hardwarescope/sensor_bridge.hpp"
#include "hardwarescope/sensor_snapshot.hpp"
#include "hardwarescope/storage_temperature_provider.hpp"
#include "hardwarescope/system_performance_provider.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace hardwarescope {

using SnapshotPublishedCallback = void (*)(void* context, std::uint64_t sequence) noexcept;

enum class SensorWorkerMode : std::uint8_t {
    native,
    synthetic,
};

enum class PrivilegedSensorStatus : std::uint8_t {
    starting,
    service_connected,
    direct_access,
    unavailable,
};

[[nodiscard]] std::chrono::milliseconds SelectSensorPublishInterval(
    std::chrono::milliseconds hardware_interval,
    std::uint32_t fps_refresh_interval_ms,
    bool fps_enabled,
    bool fps_game_only,
    bool frame_rate_available) noexcept;

class SensorWorker final {
public:
    SensorWorker(
        SnapshotStore& store,
        SnapshotPublishedCallback callback,
        void* context,
        SensorWorkerMode mode = SensorWorkerMode::synthetic,
        HINSTANCE resources = nullptr) noexcept;
    ~SensorWorker();

    SensorWorker(const SensorWorker&) = delete;
    SensorWorker& operator=(const SensorWorker&) = delete;

    void Start(std::chrono::milliseconds interval = std::chrono::milliseconds{500});
    void ConfigureFps(bool enabled, bool game_only, std::uint32_t refresh_interval_ms, std::uint32_t smoothing_interval_ms) noexcept;
    void ConfigureMinMaxReset(bool on_game_launch, std::uint32_t interval_minutes) noexcept;
    void RequestMinMaxReset() noexcept;
    void ConfigureInterval(std::chrono::milliseconds interval) noexcept;
    void SetSuspended(bool suspended) noexcept;
    void RequestStop() noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] PrivilegedSensorStatus PrivilegedStatus() const noexcept;

private:
    struct Workspace;

    void Run(std::stop_token stop_token, std::chrono::milliseconds interval) noexcept;
    void CollectNative(std::uint64_t sequence, SensorSnapshot& snapshot) noexcept;
    static void CollectSynthetic(std::uint64_t sequence, SensorSnapshot& snapshot) noexcept;
    void ApplyHistory(SensorSnapshot& snapshot) noexcept;

    struct HistoryEntry final {
        std::uint64_t id{};
        double minimum{};
        double maximum{};
    };

    SnapshotStore& store_;
    SnapshotPublishedCallback callback_{};
    void* callback_context_{};
    SensorWorkerMode mode_{};
    HINSTANCE resources_{};
    std::jthread thread_{};
    std::atomic<bool> running_{};
    std::atomic<PrivilegedSensorStatus> privileged_status_{PrivilegedSensorStatus::starting};
    std::atomic<bool> reset_history_requested_{};
    AmdGpuProvider amd_gpu_provider_{};
    NvidiaGpuProvider nvidia_provider_{};
    PrivilegedSensorCollector privileged_collector_{};
    SensorBridgeClient privileged_bridge_{};
    SystemPerformanceProvider system_provider_{};
    StorageTemperatureProvider storage_provider_{};
    std::array<HistoryEntry, kMaxSensors> history_{};
    std::size_t history_count_{};
    struct Configuration {
        bool fps_enabled{true};
        bool fps_game_only{true};
        std::uint32_t fps_refresh_interval_ms{100U};
        std::uint32_t fps_smoothing_interval_ms{500U};
        bool reset_min_max_on_game_launch{};
        std::uint32_t reset_min_max_interval_minutes{};
        std::chrono::milliseconds interval{500};
        bool suspended{};
        std::uint64_t revision{};
    };
    std::mutex configuration_mutex_;
    std::condition_variable_any configuration_wake_;
    Configuration configuration_;
    std::unique_ptr<Workspace> workspace_;
};

} // namespace hardwarescope
