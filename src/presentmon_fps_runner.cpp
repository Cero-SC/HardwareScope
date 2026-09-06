#include "hardwarescope/presentmon_fps_runner.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <evntrace.h>
#include <filesystem>
#include <string_view>
#include <vector>

namespace hardwarescope {
namespace {

constexpr wchar_t kSessionName[] = L"HardwareScopePresentMon";
constexpr std::wstring_view kLegacySessionPrefix = L"HardwareScopeNativeFPS-";
constexpr ULONG kTraceQueryCapacity = 64U;
constexpr std::size_t kTraceTextCharacters = 1'024U;

using TraceStorage = std::array<std::byte,
    sizeof(EVENT_TRACE_PROPERTIES) + (kTraceTextCharacters * 2U * sizeof(wchar_t))>;

EVENT_TRACE_PROPERTIES* InitializeTraceStorage(TraceStorage& storage) noexcept {
    storage.fill(std::byte{});
    auto* const properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(storage.size());
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    properties->LogFileNameOffset = sizeof(EVENT_TRACE_PROPERTIES)
        + static_cast<ULONG>(kTraceTextCharacters * sizeof(wchar_t));
    return properties;
}

void StopTraceSession(const std::wstring_view name) noexcept {
    if (name.empty()) return;
    TraceStorage storage{};
    auto* const properties = InitializeTraceStorage(storage);
    std::wstring terminated{name};
    static_cast<void>(ControlTraceW(0U, terminated.data(), properties, EVENT_TRACE_CONTROL_STOP));
}

std::wstring ProcessName(const std::uint32_t process_id) {
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) return L"Game";
    std::array<wchar_t, 32'768U> path{};
    DWORD length = static_cast<DWORD>(path.size());
    const auto success = QueryFullProcessImageNameW(process, 0U, path.data(), &length);
    CloseHandle(process);
    if (!success) return L"Game";
    const std::filesystem::path value{std::wstring_view{path.data(), length}};
    return value.filename().wstring();
}

struct CsvColumns {
    std::array<std::string_view, 64> values{};
    std::size_t count{};
    std::size_t size() const noexcept { return count; }
    std::string_view operator[](std::size_t index) const noexcept { return values[index]; }
};

CsvColumns Columns(const std::string_view line) noexcept {
    CsvColumns result;
    std::size_t begin{};
    bool quoted{};
    for (std::size_t index{}; index <= line.size(); ++index) {
        if (index < line.size() && line[index] == '"') quoted = !quoted;
        if (index != line.size() && (line[index] != ',' || quoted)) continue;
        if (quoted || result.count == result.values.size()) return {};
        auto value = line.substr(begin, index - begin);
        if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
            value.remove_prefix(1U); value.remove_suffix(1U);
        }
        result.values[result.count++] = value;
        begin = index + 1U;
    }
    return result;
}

bool EqualsInsensitive(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index{}; index < left.size(); ++index) {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (static_cast<unsigned char>(std::tolower(a)) != static_cast<unsigned char>(std::tolower(b))) return false;
    }
    return true;
}

double Number(const std::string_view text) noexcept {
    double value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : 0.0;
}

} // namespace

std::optional<PresentMonCsvSample> PresentMonCsvStream::Consume(
    const std::string_view line, const std::uint32_t process_id, const std::uint64_t now) noexcept {
    const auto columns = Columns(line);
    if (interval_column_ == std::string_view::npos) {
        for (std::size_t index{}; index < columns.size(); ++index) {
            if (EqualsInsensitive(columns[index], "MsBetweenPresents")) interval_column_ = index;
            if (EqualsInsensitive(columns[index], "ProcessID")) process_column_ = index;
            if (EqualsInsensitive(columns[index], "SwapChainAddress")) chain_column_ = index;
        }
        if (process_column_ == std::string_view::npos || chain_column_ == std::string_view::npos)
            interval_column_ = std::string_view::npos;
        return std::nullopt;
    }
    if (std::max({interval_column_, process_column_, chain_column_}) >= columns.size()) return std::nullopt;
    if (Number(columns[process_column_]) != static_cast<double>(process_id)) return std::nullopt;
    const auto milliseconds = Number(columns[interval_column_]);
    // Keep real hitches. Intervals exceeding the entire 60s history are capture
    // discontinuities and reset the stream rather than silently bias its lows.
    if (!std::isfinite(milliseconds) || milliseconds <= 0.05) return std::nullopt;
    auto chain_text = columns[chain_column_];
    int base = 10;
    if (chain_text.starts_with("0x") || chain_text.starts_with("0X")) { chain_text.remove_prefix(2); base = 16; }
    std::uint64_t chain{};
    const auto parsed = std::from_chars(chain_text.data(), chain_text.data() + chain_text.size(), chain, base);
    if (parsed.ec != std::errc{} || parsed.ptr != chain_text.data() + chain_text.size() || chain == 0U) return std::nullopt;
    if (milliseconds > 60'000.0) {
        if (chain == selected_) reset_pending_ = true;
        return std::nullopt;
    }
    if (selected_ == 0U) { selected_ = chain; window_tick_ = now; }
    for (auto& stream : streams_) {
        if (stream.id == chain || stream.id == 0U) { stream.id = chain; ++stream.count; break; }
    }
    if (now - window_tick_ >= 1'000U) {
        const auto best = std::max_element(streams_.begin(), streams_.end(), [](const Stream& a, const Stream& b) { return a.count < b.count; });
        const auto active = std::find_if(streams_.begin(), streams_.end(), [this](const Stream& s) { return s.id == selected_; });
        const auto active_count = active == streams_.end() ? 0U : active->count;
        if (best->id != 0U && best->id != selected_ && best->count > active_count * 2U) {
            selected_ = best->id; reset_pending_ = true;
        }
        streams_ = {}; window_tick_ = now;
    }
    if (chain != selected_) {
        if (now - last_selected_tick_ <= 2'500U) return std::nullopt;
        selected_ = chain; reset_pending_ = true;
    }
    last_selected_tick_ = now;
    const auto reset = reset_pending_;
    reset_pending_ = false;
    return PresentMonCsvSample{milliseconds, reset};
}

std::uint32_t CalculateOnePercentLowFps(double* const intervals, const std::size_t count) noexcept {
    if (intervals == nullptr || count < 100U) return 0U;
    const auto slow_count = std::max<std::size_t>(1U, (count + 99U) / 100U);
    const auto slow_begin = intervals + (count - slow_count);
    std::nth_element(intervals, slow_begin, intervals + count);
    double total{};
    for (auto* value = slow_begin; value != intervals + count; ++value) total += *value;
    const auto average = total / static_cast<double>(slow_count);
    if (!std::isfinite(average) || average <= 0.05) return 0U;
    return static_cast<std::uint32_t>(std::clamp(std::llround(1'000.0 / average), 1LL, 9'999LL));
}

PresentMonFpsRunner::PresentMonFpsRunner() {
    CleanupOrphanedSessions();
}

PresentMonFpsRunner::~PresentMonFpsRunner() {
    Stop();
}

std::wstring PresentMonFpsRunner::RuntimePath() const {
    std::array<wchar_t, 32'768U> module{};
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length == 0U || length >= module.size()) return {};
    auto path = std::filesystem::path{std::wstring_view{module.data(), length}}.parent_path();
    path /= L"PresentMon.exe";
    return path.wstring();
}

void PresentMonFpsRunner::SetTarget(const std::uint32_t process_id, const std::uint32_t smoothing_milliseconds) noexcept {
    smoothing_milliseconds_.store(std::clamp(smoothing_milliseconds, 250U, 1'250U), std::memory_order_release);
    if (process_id == target_process_id_.load(std::memory_order_acquire)) {
        if (process_id == 0U) return;
        if (process_ != nullptr && WaitForSingleObject(process_, 0U) == WAIT_TIMEOUT) return;
        if (GetTickCount64() < next_start_attempt_tick_) return;
    }
    Stop();
    target_process_id_.store(process_id, std::memory_order_release);
    if (process_id != 0U) Start(process_id);
}

void PresentMonFpsRunner::Start(const std::uint32_t process_id) noexcept {
    next_start_attempt_tick_ = GetTickCount64() + 2'000U;
    const auto runtime = RuntimePath();
    if (runtime.empty() || GetFileAttributesW(runtime.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE read_pipe{};
    HANDLE write_pipe{};
    if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 64U * 1024U)) return;
    static_cast<void>(SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0U));
    const auto error_sink = CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring command = L"\"" + runtime + L"\" --process_id " + std::to_wstring(process_id)
        + L" --output_stdout --no_console_stats --qpc_time_ms --no_track_display --no_track_gpu --no_track_input"
          L" --terminate_on_proc_exit --stop_existing_session --session_name " + kSessionName;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_pipe;
    startup.hStdError = error_sink != INVALID_HANDLE_VALUE ? error_sink : write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const auto created = CreateProcessW(
        runtime.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &process);
    CloseHandle(write_pipe);
    if (error_sink != INVALID_HANDLE_VALUE) CloseHandle(error_sink);
    if (!created) {
        CloseHandle(read_pipe);
        return;
    }
    CloseHandle(process.hThread);
    process_ = process.hProcess;
    pipe_ = read_pipe;
    next_start_attempt_tick_ = 0U;
    {
        const std::scoped_lock lock(mutex_);
        const auto name = ProcessName(process_id);
        static_cast<void>(wcsncpy_s(application_.data(), application_.size(), name.c_str(), _TRUNCATE));
    }
    reader_thread_ = std::jthread([this, read_pipe, process_id](const std::stop_token token) { ReadOutput(token, read_pipe, process_id); });
}

void PresentMonFpsRunner::ReadOutput(const std::stop_token token, const HANDLE pipe, const std::uint32_t process_id) noexcept {
    std::array<wchar_t, 32'768U> diagnostic_path{};
    const auto diagnostic_length = GetEnvironmentVariableW(
        L"HARDWARESCOPE_FPS_DIAGNOSTIC", diagnostic_path.data(), static_cast<DWORD>(diagnostic_path.size()));
    const auto diagnostic = diagnostic_length > 0U && diagnostic_length < diagnostic_path.size()
        ? CreateFileW(diagnostic_path.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
              FILE_ATTRIBUTE_NORMAL, nullptr)
        : INVALID_HANDLE_VALUE;
    std::string raw_pending;
    std::string pending;
    pending.reserve(16U * 1024U);
    raw_pending.reserve(16U * 1024U);
    std::array<char, 8U * 1024U> buffer{};
    PresentMonCsvStream csv;
    enum class StreamEncoding : std::uint8_t { unknown, narrow, utf16_little_endian };
    auto encoding = StreamEncoding::unknown;
    while (!token.stop_requested()) {
        DWORD read{};
        if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0U) break;
        if (diagnostic != INVALID_HANDLE_VALUE) {
            DWORD written{};
            static_cast<void>(WriteFile(diagnostic, buffer.data(), read, &written, nullptr));
        }
        raw_pending.append(buffer.data(), read);
        if (encoding == StreamEncoding::unknown && raw_pending.size() >= 2U) {
            const auto first = static_cast<unsigned char>(raw_pending[0U]);
            const auto second = static_cast<unsigned char>(raw_pending[1U]);
            encoding = first == 0xFFU && second == 0xFEU
                ? StreamEncoding::utf16_little_endian : StreamEncoding::narrow;
            if (encoding == StreamEncoding::utf16_little_endian) raw_pending.erase(0U, 2U);
        }
        if (encoding == StreamEncoding::narrow) {
            pending.append(raw_pending);
            raw_pending.clear();
        } else if (encoding == StreamEncoding::utf16_little_endian) {
            const auto complete_bytes = raw_pending.size() & ~std::size_t{1U};
            for (std::size_t index{}; index < complete_bytes; index += 2U) {
                const auto low = static_cast<unsigned char>(raw_pending[index]);
                const auto high = static_cast<unsigned char>(raw_pending[index + 1U]);
                const auto value = static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8U));
                if (value == 0xFEFFU) continue;
                pending.push_back(value <= 0x7FU ? static_cast<char>(value) : '?');
            }
            raw_pending.erase(0U, complete_bytes);
        }
        std::size_t newline{};
        while ((newline = pending.find('\n')) != std::string::npos) {
            auto line = std::string_view{pending.data(), newline};
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
            if (const auto sample = csv.Consume(line, process_id, GetTickCount64()))
                RecordInterval(sample->milliseconds, process_id, sample->new_stream);
            pending.erase(0U, newline + 1U);
        }
        if (pending.size() > 64U * 1024U) break; // bounded malformed/unterminated line
    }
    if (diagnostic != INVALID_HANDLE_VALUE) CloseHandle(diagnostic);
}

void PresentMonFpsRunner::RecordInterval(const double milliseconds, const std::uint32_t process_id, const bool new_stream) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds <= 0.05 || milliseconds > kHistoryMilliseconds
        || process_id != target_process_id_.load(std::memory_order_acquire)) return;
    const std::scoped_lock lock(mutex_);
    if (new_stream) {
        interval_first_ = interval_count_ = 0U;
        interval_total_ = 0.0;
        cached_one_percent_low_ = 0U;
        last_percentile_tick_ = 0U;
    }
    if (interval_count_ == intervals_.size()) {
        interval_total_ -= intervals_[interval_first_];
        interval_first_ = (interval_first_ + 1U) % intervals_.size();
        --interval_count_;
    }
    const auto insert = (interval_first_ + interval_count_) % intervals_.size();
    intervals_[insert] = milliseconds;
    interval_total_ += milliseconds;
    ++interval_count_;
    while (interval_count_ > 2U && interval_total_ - intervals_[interval_first_] >= kHistoryMilliseconds) {
        interval_total_ -= intervals_[interval_first_];
        interval_first_ = (interval_first_ + 1U) % intervals_.size();
        --interval_count_;
    }
    last_frame_tick_ = GetTickCount64();
}

PresentMonFpsReading PresentMonFpsRunner::Snapshot() const noexcept {
    PresentMonFpsReading reading{};
    std::size_t percentile_count{};
    const auto now = GetTickCount64();
    {
        const std::scoped_lock lock(mutex_);
        const auto target = target_process_id_.load(std::memory_order_acquire);
        if (target == 0U || interval_count_ < 2U || interval_total_ <= 0.05
            || now - last_frame_tick_ > 2'500U) return reading;

        const auto smoothing = static_cast<double>(smoothing_milliseconds_.load(std::memory_order_acquire));
        double smoothing_total{};
        std::size_t smoothing_count{};
        for (std::size_t offset{}; offset < interval_count_; ++offset) {
            const auto index = (interval_first_ + interval_count_ - 1U - offset) % intervals_.size();
            smoothing_total += intervals_[index];
            ++smoothing_count;
            if (smoothing_total >= smoothing) break;
        }
        if (smoothing_count == 0U || smoothing_total <= 0.05) return reading;
        reading.available = true;
        reading.frames_per_second = static_cast<std::uint32_t>(std::clamp(
            std::llround(1'000.0 * static_cast<double>(smoothing_count) / smoothing_total), 1LL, 9'999LL));
        reading.frame_time_milliseconds = intervals_[(interval_first_ + interval_count_ - 1U) % intervals_.size()];
        reading.one_percent_low_frames_per_second = cached_one_percent_low_;
        reading.process_id = target;
        reading.application = application_;
        if (last_percentile_tick_ == 0U || now - last_percentile_tick_ >= 1'000U) {
            percentile_count = interval_count_;
            for (std::size_t index{}; index < percentile_count; ++index) {
                percentile_scratch_[index] = intervals_[(interval_first_ + index) % intervals_.size()];
            }
            last_percentile_tick_ = now;
        }
        if (percentile_count != 0U) {
            // Once per second, bounded to kMaximumIntervals. Serialize scratch
            // and result publication with stream resets, not only PID changes.
            const auto low = CalculateOnePercentLowFps(percentile_scratch_.data(), percentile_count);
            cached_one_percent_low_ = low;
            reading.one_percent_low_frames_per_second = low;
        }
    }
    return reading;
}

void PresentMonFpsRunner::Stop() noexcept {
    target_process_id_.store(0U, std::memory_order_release);
    if (reader_thread_.joinable()) reader_thread_.request_stop();
    StopTraceSession(kSessionName);
    if (process_ != nullptr) {
        if (WaitForSingleObject(process_, 2'000U) == WAIT_TIMEOUT) {
            static_cast<void>(TerminateProcess(process_, 0U));
            static_cast<void>(WaitForSingleObject(process_, 2'000U));
            StopTraceSession(kSessionName);
        }
    }
    if (reader_thread_.joinable()) static_cast<void>(CancelSynchronousIo(reader_thread_.native_handle()));
    if (reader_thread_.joinable()) reader_thread_.join();
    if (pipe_ != nullptr) CloseHandle(pipe_);
    pipe_ = nullptr;
    if (process_ != nullptr) CloseHandle(process_);
    process_ = nullptr;
    const std::scoped_lock lock(mutex_);
    interval_first_ = 0U;
    interval_count_ = 0U;
    interval_total_ = 0.0;
    last_frame_tick_ = 0U;
    last_percentile_tick_ = 0U;
    cached_one_percent_low_ = 0U;
    application_ = {};
}

void PresentMonFpsRunner::CleanupOrphanedSessions() noexcept {
    std::array<TraceStorage, kTraceQueryCapacity> storage{};
    std::array<EVENT_TRACE_PROPERTIES*, kTraceQueryCapacity> properties{};
    for (std::size_t index{}; index < storage.size(); ++index) {
        properties[index] = InitializeTraceStorage(storage[index]);
    }
    ULONG count{};
    const auto result = QueryAllTracesW(properties.data(), static_cast<ULONG>(properties.size()), &count);
    if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA) {
        StopTraceSession(kSessionName);
        return;
    }
    const auto available = std::min<std::size_t>(count, properties.size());
    for (std::size_t index{}; index < available; ++index) {
        const auto* const name = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const std::byte*>(properties[index]) + properties[index]->LoggerNameOffset);
        const std::wstring_view value{name};
        if (value == kSessionName || value.starts_with(kLegacySessionPrefix)) StopTraceSession(value);
    }
}

} // namespace hardwarescope
