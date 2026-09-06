#include "hardwarescope/graph_model.hpp"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <limits>

namespace hardwarescope {
namespace {

const SensorValue* FindSensor(const SensorSnapshot& snapshot, const std::uint64_t id) noexcept {
    for (std::uint32_t index{}; index < snapshot.count; ++index) {
        if (snapshot.sensors[index].id == id) return &snapshot.sensors[index];
    }
    return nullptr;
}

GraphRange Padded(const double minimum, const double maximum) noexcept {
    const auto span = std::max(0.001, maximum - minimum);
    const auto padding = std::max(0.5, span * 0.08);
    return GraphRange{minimum - padding, maximum + padding};
}

} // namespace

double GraphSeries::Sample(const std::size_t chronological_index) const noexcept {
    if (chronological_index >= count) return 0.0;
    return samples[(first + chronological_index) % samples.size()];
}

std::uint64_t GraphSeries::Timestamp(const std::size_t chronological_index) const noexcept {
    if (chronological_index >= count) return 0U;
    return timestamps_milliseconds[(first + chronological_index) % timestamps_milliseconds.size()];
}

void GraphHistory::Configure(const AppSettings& settings) noexcept {
    const auto next_count = std::min<std::size_t>(settings.osd_graph_sensor_count, configured_ids_.size());
    bool sources_changed = next_count != series_count_;
    for (std::size_t index{}; index < configured_ids_.size(); ++index) {
        const auto next = index < next_count ? settings.osd_graph_sensor_ids[index] : 0U;
        sources_changed = sources_changed || configured_ids_[index] != next;
        configured_ids_[index] = next;
    }
    const bool timing_changed = history_seconds_ != settings.osd_graph_history_seconds
        || refresh_milliseconds_ != settings.osd_graph_refresh_interval_ms;
    const bool scale_changed = scale_mode_ != settings.osd_graph_scale_mode
        || custom_minimum_ != settings.osd_graph_custom_minimum
        || custom_maximum_ != settings.osd_graph_custom_maximum;
    series_count_ = next_count;
    history_seconds_ = settings.osd_graph_history_seconds;
    refresh_milliseconds_ = settings.osd_graph_refresh_interval_ms;
    scale_mode_ = settings.osd_graph_scale_mode;
    custom_minimum_ = settings.osd_graph_custom_minimum;
    custom_maximum_ = settings.osd_graph_custom_maximum;
    if (sources_changed || timing_changed) Clear();
    if (sources_changed || timing_changed || scale_changed) adaptive_initialized_ = false;
}

void GraphHistory::Update(const SensorSnapshot& snapshot, const std::uint64_t tick_milliseconds) noexcept {
    if (paused_ || series_count_ == 0U) return;
    latest_tick_ = tick_milliseconds;
    for (auto& series : series_) {
        while (series.count != 0U && tick_milliseconds >= series.Timestamp(0U)
            && tick_milliseconds - series.Timestamp(0U) >= static_cast<std::uint64_t>(history_seconds_) * 1'000U) {
            series.first = (series.first + 1U) % series.samples.size();
            --series.count;
        }
    }
    if (snapshot.sequence == 0U || snapshot.sequence == last_snapshot_sequence_) return;
    last_snapshot_sequence_ = snapshot.sequence;
    if (last_sample_tick_ != 0U && tick_milliseconds - last_sample_tick_ < refresh_milliseconds_) return;
    last_sample_tick_ = tick_milliseconds;
    const auto desired = GraphSeries::kMaximumSamples;
    const auto* reference = FindSensor(snapshot, configured_ids_[0]);

    for (std::size_t index{}; index < series_count_; ++index) {
        auto& series = series_[index];
        const auto* sensor = FindSensor(snapshot, configured_ids_[index]);
        if (sensor == nullptr || !sensor->available || !std::isfinite(sensor->current)) {
            series.available = false;
            series.gap_pending = true;
            continue;
        }
        if (reference == nullptr) {
            // The reference may be omitted on a failed provider refresh. Keep
            // sibling history until its unit can be checked again, with a gap.
            series.available = false;
            series.gap_pending = true;
            continue;
        }
        if (sensor->unit != reference->unit) {
            series = {}; // imported IDs cannot bypass the same-unit graph contract
            continue;
        }
        if (series.sensor_id != sensor->id) {
            series = {};
            series.sensor_id = sensor->id;
        }
        series.unit = sensor->unit;
        series.current = sensor->current;
        series.available = true;
        static_cast<void>(wcsncpy_s(series.name.data(), series.name.size(), sensor->name.data(), _TRUNCATE));
        while (series.count >= desired) {
            series.first = (series.first + 1U) % series.samples.size();
            --series.count;
        }
        const auto insert = (series.first + series.count) % series.samples.size();
        series.samples[insert] = sensor->current;
        series.timestamps_milliseconds[insert] = tick_milliseconds;
        series.breaks[insert] = series.gap_pending;
        series.gap_pending = false;
        ++series.count;
    }
}

void GraphHistory::Clear() noexcept {
    series_ = {};
    last_snapshot_sequence_ = 0U;
    last_sample_tick_ = 0U;
    latest_tick_ = 0U;
    adaptive_initialized_ = false;
}

std::size_t GraphHistory::DesiredSampleCount() const noexcept {
    const auto count = static_cast<std::size_t>(history_seconds_) * 1'000U / std::max(1U, refresh_milliseconds_);
    return std::clamp<std::size_t>(count, 2U, GraphSeries::kMaximumSamples);
}

GraphRange GraphHistory::FixedRange(const SensorUnit unit) const noexcept {
    switch (unit) {
    case SensorUnit::percent: return {0.0, 100.0};
    case SensorUnit::celsius: return {20.0, 100.0};
    case SensorUnit::milliseconds: return {0.0, 33.34};
    case SensorUnit::frames_per_second: return {0.0, 240.0};
    case SensorUnit::megahertz: return {0.0, 6'000.0};
    case SensorUnit::revolutions_per_minute: return {0.0, 5'000.0};
    case SensorUnit::watts: return {0.0, 500.0};
    case SensorUnit::volts: return {0.0, 2.0};
    case SensorUnit::megabytes: return {0.0, 32'768.0};
    }
    return {0.0, 100.0};
}

GraphRange GraphHistory::ObservedRange(const std::uint32_t view_seconds) const noexcept {
    auto minimum = std::numeric_limits<double>::infinity();
    auto maximum = -std::numeric_limits<double>::infinity();
    for (std::size_t series_index{}; series_index < series_count_; ++series_index) {
        const auto& series = series_[series_index];
        for (std::size_t sample{}; sample < series.count; ++sample) {
            if (latest_tick_ >= series.Timestamp(sample)
                && latest_tick_ - series.Timestamp(sample) > static_cast<std::uint64_t>(view_seconds) * 1'000U) continue;
            const auto value = series.Sample(sample);
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
    }
    return std::isfinite(minimum) && std::isfinite(maximum) ? Padded(minimum, maximum) : GraphRange{0.0, 100.0};
}

GraphRange GraphHistory::Range(const std::uint32_t view_seconds) noexcept {
    if (scale_mode_ == GraphScaleMode::custom) return {custom_minimum_, custom_maximum_};
    SensorUnit unit = SensorUnit::percent;
    for (std::size_t index{}; index < series_count_; ++index) {
        if (series_[index].available || series_[index].count != 0U) {
            unit = series_[index].unit;
            break;
        }
    }
    if (scale_mode_ == GraphScaleMode::fixed) return FixedRange(unit);

    const auto observed = ObservedRange(view_seconds == 0U ? history_seconds_ : view_seconds);
    if (!adaptive_initialized_) {
        adaptive_range_ = observed;
        adaptive_initialized_ = true;
    } else {
        if (observed.minimum < adaptive_range_.minimum) adaptive_range_.minimum = observed.minimum;
        else adaptive_range_.minimum += (observed.minimum - adaptive_range_.minimum) * 0.04;
        if (observed.maximum > adaptive_range_.maximum) adaptive_range_.maximum = observed.maximum;
        else adaptive_range_.maximum += (observed.maximum - adaptive_range_.maximum) * 0.04;
    }
    if (adaptive_range_.maximum - adaptive_range_.minimum < 0.001) adaptive_range_.maximum = adaptive_range_.minimum + 1.0;
    return adaptive_range_;
}

const wchar_t* GraphUnitSuffix(const SensorUnit unit) noexcept {
    switch (unit) {
    case SensorUnit::celsius: return L"\u00b0C";
    case SensorUnit::percent: return L"%";
    case SensorUnit::megahertz: return L"MHz";
    case SensorUnit::revolutions_per_minute: return L"RPM";
    case SensorUnit::watts: return L"W";
    case SensorUnit::volts: return L"V";
    case SensorUnit::megabytes: return L"MB";
    case SensorUnit::frames_per_second: return L"FPS";
    case SensorUnit::milliseconds: return L"ms";
    }
    return L"";
}

} // namespace hardwarescope
