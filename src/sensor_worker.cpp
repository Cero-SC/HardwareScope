#include "hardwarescope/sensor_worker.hpp"
#include "hardwarescope/game_detector.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <memory>
#include <mutex>
#include <new>

namespace hardwarescope {
namespace {

void CopyText(auto& destination, const wchar_t* source) noexcept {
    static_cast<void>(wcsncpy_s(destination.data(), destination.size(), source, _TRUNCATE));
}

SensorValue MakeSensor(
    const std::uint64_t id,
    const SensorKind kind,
    const SensorUnit unit,
    const wchar_t* name,
    const wchar_t* hardware,
    const double current,
    const double minimum,
    const double maximum) noexcept {
    SensorValue value{};
    value.id = id;
    value.kind = kind;
    value.unit = unit;
    value.available = true;
    CopyText(value.name, name);
    CopyText(value.hardware, hardware);
    value.current = current;
    value.minimum = minimum;
    value.maximum = maximum;
    return value;
}

bool IsProcessElevated() noexcept {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size{};
    const auto success = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return success && elevation.TokenIsElevated != 0U;
}

} // namespace

struct SensorWorker::Workspace final {
    SensorSnapshot bridge_probe{};
    SensorSnapshot last_hardware_snapshot{};
    SensorSnapshot publish_snapshot{};
};

std::chrono::milliseconds SelectSensorPublishInterval(
    const std::chrono::milliseconds hardware_interval,
    const std::uint32_t fps_refresh_interval_ms,
    const bool fps_enabled,
    const bool /*fps_game_only*/,
    const bool frame_rate_available) noexcept {
    if (!fps_enabled) return hardware_interval;
    // Probe for a game / first frame at a bounded cadence independent of slow
    // hardware reads. Actual hardware collection still follows its own deadline.
    if (!frame_rate_available) return std::min(hardware_interval, std::chrono::milliseconds{500});
    return std::min(hardware_interval, std::chrono::milliseconds{fps_refresh_interval_ms});
}

SensorWorker::SensorWorker(
    SnapshotStore& store,
    const SnapshotPublishedCallback callback,
    void* const context,
    const SensorWorkerMode mode,
    const HINSTANCE resources) noexcept
    : store_(store),
      callback_(callback),
      callback_context_(context),
      mode_(mode),
      resources_(resources),
      workspace_(new (std::nothrow) Workspace{}) {}

SensorWorker::~SensorWorker() {
    Stop();
}

void SensorWorker::Start(std::chrono::milliseconds interval) {
    ConfigureInterval(interval);
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    privileged_status_.store(PrivilegedSensorStatus::starting, std::memory_order_release);
    interval = std::clamp(interval, std::chrono::milliseconds{100}, std::chrono::milliseconds{10'000});
    thread_ = std::jthread([this, interval](const std::stop_token stop_token) { Run(stop_token, interval); });
}

void SensorWorker::Stop() noexcept {
    RequestStop();
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void SensorWorker::RequestStop() noexcept {
    if (thread_.joinable()) thread_.request_stop();
    configuration_wake_.notify_all();
}

void SensorWorker::ConfigureInterval(const std::chrono::milliseconds interval) noexcept {
    const std::scoped_lock lock(configuration_mutex_);
    configuration_.interval = std::clamp(interval, std::chrono::milliseconds{100}, std::chrono::milliseconds{10'000});
    ++configuration_.revision;
    configuration_wake_.notify_all();
}

void SensorWorker::SetSuspended(const bool suspended) noexcept {
    const std::scoped_lock lock(configuration_mutex_);
    configuration_.suspended = suspended;
    ++configuration_.revision;
    configuration_wake_.notify_all();
}

bool SensorWorker::Running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

PrivilegedSensorStatus SensorWorker::PrivilegedStatus() const noexcept {
    return privileged_status_.load(std::memory_order_acquire);
}

void SensorWorker::ConfigureFps(
    const bool enabled,
    const bool game_only,
    const std::uint32_t refresh_interval_ms,
    const std::uint32_t smoothing_interval_ms) noexcept {
    const std::scoped_lock lock(configuration_mutex_);
    configuration_.fps_enabled = enabled;
    configuration_.fps_game_only = game_only;
    configuration_.fps_refresh_interval_ms = std::clamp(refresh_interval_ms, 50U, 500U);
    configuration_.fps_smoothing_interval_ms = std::clamp(smoothing_interval_ms, 250U, 1'250U);
    ++configuration_.revision;
    configuration_wake_.notify_all();
}

void SensorWorker::ConfigureMinMaxReset(const bool on_game_launch, const std::uint32_t interval_minutes) noexcept {
    const std::scoped_lock lock(configuration_mutex_);
    configuration_.reset_min_max_on_game_launch = on_game_launch;
    configuration_.reset_min_max_interval_minutes = std::min(interval_minutes, 10'080U);
    ++configuration_.revision;
    configuration_wake_.notify_all();
}

void SensorWorker::RequestMinMaxReset() noexcept {
    reset_history_requested_.store(true, std::memory_order_release);
}

void SensorWorker::Run(const std::stop_token stop_token, std::chrono::milliseconds interval) noexcept {
    if (workspace_ == nullptr) {
        running_.store(false, std::memory_order_release);
        return;
    }
    static_cast<void>(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL));
    std::uint64_t sequence = 0;
    if (mode_ == SensorWorkerMode::native) static_cast<void>(nvidia_provider_.Initialize());
    if (mode_ == SensorWorkerMode::native) static_cast<void>(amd_gpu_provider_.Initialize());
    if (mode_ == SensorWorkerMode::native) {
        ResetSnapshot(workspace_->bridge_probe);
        if (privileged_bridge_.Collect(workspace_->bridge_probe)) {
            privileged_status_.store(PrivilegedSensorStatus::service_connected, std::memory_order_release);
        } else if (IsProcessElevated() && privileged_collector_.Initialize(resources_)) {
            privileged_status_.store(PrivilegedSensorStatus::direct_access, std::memory_order_release);
        } else {
            privileged_status_.store(PrivilegedSensorStatus::unavailable, std::memory_order_release);
        }
    }
    ResetSnapshot(workspace_->last_hardware_snapshot);
    auto next_hardware_refresh = std::chrono::steady_clock::time_point{};
    auto last_history_reset = std::chrono::steady_clock::now();
    bool previous_game_target_active{};
    std::uint64_t applied_revision = UINT64_MAX;
    Configuration configuration;

    while (!stop_token.stop_requested()) {
        {
            std::unique_lock lock(configuration_mutex_);
            configuration = configuration_;
        }
        if (configuration.revision != applied_revision) {
            interval = configuration.interval;
            next_hardware_refresh = {};
            applied_revision = configuration.revision;
        }
        if (configuration.suspended) {
            if (mode_ == SensorWorkerMode::native) {
                static_cast<void>(privileged_bridge_.SetFpsTarget(0U, configuration.fps_smoothing_interval_ms, static_cast<std::uint32_t>(interval.count())));
            }
            std::unique_lock lock(configuration_mutex_);
            configuration_wake_.wait(lock, stop_token, [this] { return !configuration_.suspended; });
            continue;
        }
        auto& snapshot = workspace_->publish_snapshot;
        ResetSnapshot(snapshot);
        bool frame_rate_available{};
        bool game_target_active{};
        if (mode_ == SensorWorkerMode::native) {
            const auto game = configuration.fps_enabled ? FindGameProcess(GetCurrentProcessId(), 0U, !configuration.fps_game_only) : GameProcess{};
            game_target_active = game.process_id != 0U;
            static_cast<void>(privileged_bridge_.SetFpsTarget(
                game.process_id,
                configuration.fps_smoothing_interval_ms,
                static_cast<std::uint32_t>(interval.count())));
            const auto now = std::chrono::steady_clock::now();
            auto& hardware = workspace_->last_hardware_snapshot;
            if (hardware.sequence == 0U || now >= next_hardware_refresh) {
                CollectNative(++sequence, hardware);
                next_hardware_refresh = now + interval;
            } else {
                ++sequence;
            }
            CopySnapshot(hardware, snapshot);
            snapshot.sequence = sequence;
            if (configuration.fps_enabled) {
                const auto new_end = std::remove_if(snapshot.sensors.begin(), snapshot.sensors.begin() + snapshot.count, [](const SensorValue& sensor) {
                    return sensor.kind == SensorKind::frame_rate;
                });
                snapshot.count = static_cast<std::uint32_t>(new_end - snapshot.sensors.begin());
                frame_rate_available = privileged_bridge_.CollectFrameRates(snapshot);
            }
        } else {
            CollectSynthetic(++sequence, snapshot);
        }
        const auto history_now = std::chrono::steady_clock::now();
        const auto periodic_reset = configuration.reset_min_max_interval_minutes != 0U
            && history_now - last_history_reset >= std::chrono::minutes{configuration.reset_min_max_interval_minutes};
        const auto game_launch_reset = configuration.reset_min_max_on_game_launch && game_target_active && !previous_game_target_active;
        if (reset_history_requested_.exchange(false, std::memory_order_acq_rel) || periodic_reset || game_launch_reset) {
            history_ = {};
            history_count_ = 0U;
            last_history_reset = history_now;
        }
        previous_game_target_active = game_target_active;
        {
            const std::scoped_lock lock(configuration_mutex_);
            if (configuration_.suspended || stop_token.stop_requested()) continue;
        }
        ApplyHistory(snapshot);
        store_.Publish(snapshot);
        if (callback_ != nullptr) callback_(callback_context_, sequence);

        const auto publish_interval = mode_ == SensorWorkerMode::native
            ? SelectSensorPublishInterval(interval, configuration.fps_refresh_interval_ms, configuration.fps_enabled, configuration.fps_game_only, frame_rate_available)
            : interval;
        std::unique_lock lock(configuration_mutex_);
        static_cast<void>(configuration_wake_.wait_for(lock, stop_token, publish_interval,
            [this, applied_revision] { return configuration_.revision != applied_revision; }));
    }
    if (mode_ == SensorWorkerMode::native) {
        static_cast<void>(privileged_bridge_.SetFpsTarget(
            0U,
            configuration.fps_smoothing_interval_ms,
            static_cast<std::uint32_t>(interval.count())));
    }
    storage_provider_.Reset();
    nvidia_provider_.Close();
    amd_gpu_provider_.Close();
    privileged_collector_.Close();
    privileged_bridge_.Close();
    running_.store(false, std::memory_order_release);
}

void SensorWorker::CollectNative(const std::uint64_t sequence, SensorSnapshot& snapshot) noexcept {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER started{};
    LARGE_INTEGER finished{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);

    ResetSnapshot(snapshot);
    snapshot.sequence = sequence;
    snapshot.captured_qpc = static_cast<std::uint64_t>(started.QuadPart);
    system_provider_.Collect(snapshot);
    storage_provider_.Collect(snapshot);
    nvidia_provider_.Collect(snapshot);
    amd_gpu_provider_.Collect(snapshot);
    if (privileged_bridge_.Collect(snapshot)) {
        privileged_status_.store(PrivilegedSensorStatus::service_connected, std::memory_order_release);
    } else if (privileged_collector_.Available()) {
        privileged_collector_.Collect(snapshot);
        privileged_status_.store(PrivilegedSensorStatus::direct_access, std::memory_order_release);
    } else {
        privileged_status_.store(PrivilegedSensorStatus::unavailable, std::memory_order_release);
    }

    QueryPerformanceCounter(&finished);
    const auto ticks = static_cast<std::uint64_t>(finished.QuadPart - started.QuadPart);
    snapshot.collection_microseconds = frequency.QuadPart > 0
        ? static_cast<std::uint32_t>((ticks * 1'000'000ULL) / static_cast<std::uint64_t>(frequency.QuadPart))
        : 0U;
}

void SensorWorker::ApplyHistory(SensorSnapshot& snapshot) noexcept {
    for (std::uint32_t sensor_index = 0; sensor_index < snapshot.count; ++sensor_index) {
        auto& sensor = snapshot.sensors[sensor_index];
        auto existing = std::find_if(history_.begin(), history_.begin() + history_count_, [&](const HistoryEntry& history) { return history.id == sensor.id; });
        if (existing == history_.begin() + history_count_) {
            if (history_count_ >= history_.size()) continue;
            existing = history_.begin() + history_count_++;
            *existing = HistoryEntry{sensor.id, sensor.current, sensor.current};
        } else {
            existing->minimum = std::min(existing->minimum, sensor.current);
            existing->maximum = std::max(existing->maximum, sensor.current);
        }
        sensor.minimum = existing->minimum;
        sensor.maximum = existing->maximum;
    }
}

void SensorWorker::CollectSynthetic(const std::uint64_t sequence, SensorSnapshot& snapshot) noexcept {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER started{};
    LARGE_INTEGER finished{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);

    const auto phase = static_cast<double>(sequence) * 0.12;
    const auto cpu_temperature = 52.0 + std::sin(phase) * 3.2;
    const auto gpu_temperature = 44.0 + std::sin(phase * 0.73) * 2.1;
    const auto gpu_memory_temperature = 56.0 + std::sin(phase * 0.61) * 2.8;
    const auto cpu_usage = 11.0 + (std::sin(phase * 1.4) + 1.0) * 9.0;
    const auto gpu_usage = 7.0 + (std::sin(phase * 1.1) + 1.0) * 6.0;

    ResetSnapshot(snapshot);
    snapshot.sequence = sequence;
    snapshot.captured_qpc = static_cast<std::uint64_t>(started.QuadPart);
    snapshot.sensors[0] = MakeSensor(1, SensorKind::temperature, SensorUnit::celsius, L"Core (Tctl/Tdie)", L"AMD Ryzen CPU", cpu_temperature, 38.4, 67.2);
    snapshot.sensors[1] = MakeSensor(2, SensorKind::temperature, SensorUnit::celsius, L"GPU Core temperature", L"NVIDIA GeForce GPU", gpu_temperature, 34.1, 61.0);
    snapshot.sensors[2] = MakeSensor(3, SensorKind::temperature, SensorUnit::celsius, L"GPU Memory Junction", L"NVIDIA GeForce GPU", gpu_memory_temperature, 42.0, 72.3);
    snapshot.sensors[3] = MakeSensor(4, SensorKind::utilization, SensorUnit::percent, L"CPU Total", L"AMD Ryzen CPU", cpu_usage, 0.4, 72.1);
    snapshot.sensors[4] = MakeSensor(5, SensorKind::utilization, SensorUnit::percent, L"GPU Core", L"NVIDIA GeForce GPU", gpu_usage, 0.0, 98.0);
    snapshot.sensors[5] = MakeSensor(6, SensorKind::clock, SensorUnit::megahertz, L"CPU Core Average", L"AMD Ryzen CPU", 4875.0 + std::sin(phase) * 125.0, 550.0, 5200.0);
    snapshot.sensors[6] = MakeSensor(7, SensorKind::power, SensorUnit::watts, L"CPU Package", L"AMD Ryzen CPU", 48.0 + std::sin(phase * 0.8) * 7.0, 18.0, 142.0);
    snapshot.count = 7;

    QueryPerformanceCounter(&finished);
    const auto ticks = static_cast<std::uint64_t>(finished.QuadPart - started.QuadPart);
    snapshot.collection_microseconds = frequency.QuadPart > 0
        ? static_cast<std::uint32_t>((ticks * 1'000'000ULL) / static_cast<std::uint64_t>(frequency.QuadPart))
        : 0U;
}

} // namespace hardwarescope
