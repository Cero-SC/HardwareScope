#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <optional>
#include <thread>

namespace hardwarescope {

struct PresentMonCsvSample final {
    double milliseconds{};
    bool new_stream{};
    std::uint64_t present_tick_milliseconds{};
    std::uint64_t present_qpc{};
};

// Bounded stream selection; statistics never combine different swap chains.
class PresentMonCsvStream final {
public:
    [[nodiscard]] std::optional<PresentMonCsvSample> Consume(
        std::string_view line, std::uint32_t process_id, std::uint64_t now,
        std::uint64_t qpc_now = 0U, std::uint64_t qpc_frequency = 0U) noexcept;
private:
    struct Stream { std::uint64_t id{}; std::uint32_t count{}; };
    std::array<Stream, 8> streams_{};
    std::size_t interval_column_{std::string_view::npos};
    std::size_t process_column_{std::string_view::npos};
    std::size_t chain_column_{std::string_view::npos};
    std::size_t qpc_column_{std::string_view::npos};
    std::uint64_t selected_{};
    std::uint64_t window_tick_{};
    std::uint64_t last_selected_tick_{};
    bool reset_pending_{true};
};

struct PresentMonFpsReading final {
    bool available{};
    std::uint32_t frames_per_second{};
    std::uint32_t one_percent_low_frames_per_second{};
    double frame_time_milliseconds{};
    std::uint64_t frame_qpc{};
    std::uint64_t low_frame_qpc{};
    std::size_t low_interval_count{};
    std::uint32_t process_id{};
    std::array<wchar_t, 64U> application{};
};

class PresentMonFpsRunner final {
public:
    PresentMonFpsRunner();
    ~PresentMonFpsRunner();

    PresentMonFpsRunner(const PresentMonFpsRunner&) = delete;
    PresentMonFpsRunner& operator=(const PresentMonFpsRunner&) = delete;

    void SetTarget(std::uint32_t process_id, std::uint32_t smoothing_milliseconds) noexcept;
    [[nodiscard]] PresentMonFpsReading Snapshot() const noexcept;
    void Stop() noexcept;

private:
    void Start(std::uint32_t process_id) noexcept;
    void ReadOutput(std::stop_token token, HANDLE pipe, std::uint32_t process_id) noexcept;
    void RecordInterval(double milliseconds, std::uint32_t process_id, bool new_stream, std::uint64_t present_tick, std::uint64_t present_qpc) noexcept;
    [[nodiscard]] std::wstring RuntimePath() const;
    static void CleanupOrphanedSessions() noexcept;

    mutable std::mutex mutex_;
    std::jthread reader_thread_{};
    HANDLE process_{};
    HANDLE pipe_{};
    std::atomic<std::uint32_t> target_process_id_{};
    std::atomic<std::uint32_t> smoothing_milliseconds_{500U};
    ULONGLONG next_start_attempt_tick_{};
    ULONGLONG next_flush_tick_{};
    static constexpr std::size_t kMaximumIntervals = 16'384U;
    static constexpr double kHistoryMilliseconds = 60'000.0;

    std::array<double, kMaximumIntervals> intervals_{};
    std::size_t interval_first_{};
    std::size_t interval_count_{};
    double interval_total_{};
    ULONGLONG last_frame_tick_{};
    std::uint64_t last_frame_qpc_{};
    mutable ULONGLONG last_percentile_tick_{};
    mutable std::uint32_t cached_one_percent_low_{};
    mutable std::uint64_t cached_low_frame_qpc_{};
    mutable std::size_t cached_low_interval_count_{};
    mutable std::array<double, kMaximumIntervals> percentile_scratch_{};
    std::array<wchar_t, 64U> application_{};
};

[[nodiscard]] std::uint32_t CalculateOnePercentLowFps(double* intervals, std::size_t count) noexcept;

} // namespace hardwarescope
