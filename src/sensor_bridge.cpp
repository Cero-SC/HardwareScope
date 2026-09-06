#include "hardwarescope/sensor_bridge.hpp"
#include "hardwarescope/monitoring_control.hpp"

#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace hardwarescope {
namespace {

static_assert(std::is_trivially_copyable_v<SensorValue>);

std::wstring TokenUserSid(const HANDLE token) {
    DWORD bytes{};
    static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0U, &bytes));
    if (bytes == 0U) return {};
    std::vector<std::byte> storage(bytes);
    if (!GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes)) return {};
    const auto user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    LPWSTR sid{};
    if (!ConvertSidToStringSidW(user->User.Sid, &sid)) return {};
    std::wstring result{sid};
    LocalFree(sid);
    return result;
}

std::wstring CurrentUserSid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    const auto sid = TokenUserSid(token);
    CloseHandle(token);
    return sid;
}

} // namespace

SensorBridgePublisher::~SensorBridgePublisher() {
    Close();
}

bool SensorBridgePublisher::Initialize() noexcept {
    Close();
    try {
        std::array<wchar_t, 32'768U> module{};
        const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (length == 0U || length >= module.size()) return false;
        expected_control_server_ = (std::filesystem::path{module.data()}.parent_path() / L"HardwareScope.exe").native();
    } catch (...) { return false; }
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GR;;;BA)(A;;GR;;;BU)",
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    mapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        &attributes,
        PAGE_READWRITE,
        0U,
        static_cast<DWORD>(sizeof(SharedSensorSnapshot)),
        kSensorBridgeName);
    LocalFree(descriptor);
    if (mapping_ == nullptr) return false;
    shared_ = static_cast<SharedSensorSnapshot*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0U, 0U, sizeof(SharedSensorSnapshot)));
    if (shared_ == nullptr) {
        Close();
        return false;
    }
    std::memset(shared_, 0, sizeof(*shared_));
    shared_->magic = kSensorBridgeMagic;
    shared_->version = kSensorBridgeVersion;
    return true;
}

void SensorBridgePublisher::Publish(const SensorSnapshot& snapshot) noexcept {
    if (shared_ == nullptr) return;
    static_cast<void>(InterlockedIncrement64(&shared_->sequence));
    MemoryBarrier();
    shared_->captured_qpc = snapshot.captured_qpc;
    shared_->collection_microseconds = snapshot.collection_microseconds;
    shared_->count = std::min<std::uint32_t>(snapshot.count, static_cast<std::uint32_t>(shared_->sensors.size()));
    std::copy_n(snapshot.sensors.begin(), shared_->count, shared_->sensors.begin());
    MemoryBarrier();
    static_cast<void>(InterlockedIncrement64(&shared_->sequence));
}

void SensorBridgePublisher::PollFpsControl() noexcept {
    SharedFpsControl request{};
    if (ReadMonitoringControl(pending_control_pipe_, kFpsControlPipeName, expected_control_server_, &request, sizeof(request))
        && request.magic == kSensorBridgeMagic && request.version == kSensorBridgeVersion) {
        requested_fps_target_ = static_cast<std::uint32_t>(request.target_process_id);
        requested_fps_smoothing_ = std::clamp(static_cast<std::uint32_t>(request.smoothing_milliseconds), 250U, 1'250U);
        requested_hardware_polling_interval_ = std::clamp(
            static_cast<std::uint32_t>(request.hardware_polling_milliseconds), 100U, 10'000U);
        last_control_request_ = std::chrono::steady_clock::now();
    } else if (std::chrono::steady_clock::now() - last_control_request_ >
        std::chrono::milliseconds{std::max(15'000U, requested_hardware_polling_interval_ * 3U + 2'000U)}) {
        // A crashed/closed UI must not leave the service capturing a game forever.
        requested_fps_target_ = 0U;
        requested_hardware_polling_interval_ = 750U;
    }
}

std::uint32_t SensorBridgePublisher::RequestedFpsTarget() noexcept {
    PollFpsControl();
    return requested_fps_target_;
}

std::uint32_t SensorBridgePublisher::RequestedFpsSmoothing() noexcept {
    return requested_fps_smoothing_;
}

std::uint32_t SensorBridgePublisher::RequestedHardwarePollingInterval() noexcept {
    return requested_hardware_polling_interval_;
}

void SensorBridgePublisher::Close() noexcept {
    if (pending_control_pipe_ != nullptr) CloseHandle(pending_control_pipe_);
    pending_control_pipe_ = nullptr;
    if (shared_ != nullptr) UnmapViewOfFile(shared_);
    if (mapping_ != nullptr) CloseHandle(mapping_);
    shared_ = nullptr;
    mapping_ = nullptr;
    requested_fps_target_ = 0U;
    requested_fps_smoothing_ = 500U;
    requested_hardware_polling_interval_ = 750U;
    last_control_request_ = {};
}

SensorBridgeClient::~SensorBridgeClient() {
    Close();
}

bool SensorBridgeClient::Connect() noexcept {
    if (shared_ != nullptr) return true;
    const auto now = std::chrono::steady_clock::now();
    if (last_connect_attempt_ != std::chrono::steady_clock::time_point{}
        && now - last_connect_attempt_ < std::chrono::seconds{1}) return false;
    last_connect_attempt_ = now;
    mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kSensorBridgeName);
    if (mapping_ == nullptr) return false;
    shared_ = static_cast<const SharedSensorSnapshot*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0U, 0U, sizeof(SharedSensorSnapshot)));
    if (shared_ == nullptr || shared_->magic != kSensorBridgeMagic || shared_->version != kSensorBridgeVersion) {
        Close();
        return false;
    }
    return true;
}

bool SensorBridgeClient::Collect(SensorSnapshot& destination) noexcept {
    if (!Connect()) return false;
    if (copy_buffer_ == nullptr) copy_buffer_.reset(new (std::nothrow) SensorSnapshot{});
    if (copy_buffer_ == nullptr) return false;
    auto& copy = *copy_buffer_;
    ResetSnapshot(copy);
    bool copied{};
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto before = shared_->sequence;
        if ((before & 1) != 0) continue;
        MemoryBarrier();
        copy.captured_qpc = shared_->captured_qpc;
        copy.collection_microseconds = shared_->collection_microseconds;
        copy.count = std::min<std::uint32_t>(shared_->count, static_cast<std::uint32_t>(copy.sensors.size()));
        std::copy_n(shared_->sensors.begin(), copy.count, copy.sensors.begin());
        MemoryBarrier();
        const auto after = shared_->sequence;
        if (before == after && (after & 1) == 0) {
            copied = true;
            break;
        }
    }
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if (!copied || copy.captured_qpc == 0U || frequency.QuadPart <= 0
        || static_cast<std::uint64_t>(now.QuadPart) < copy.captured_qpc
        || static_cast<std::uint64_t>(now.QuadPart) - copy.captured_qpc > static_cast<std::uint64_t>(frequency.QuadPart) * 5U) {
        Close();
        return false;
    }
    const auto slots = destination.sensors.size() - std::min<std::size_t>(destination.count, destination.sensors.size());
    const auto count = std::min<std::size_t>(copy.count, slots);
    std::copy_n(copy.sensors.begin(), count, destination.sensors.begin() + destination.count);
    destination.count += static_cast<std::uint32_t>(count);
    return true;
}

bool SensorBridgeClient::CollectFrameRate(SensorValue& destination) noexcept {
    SensorSnapshot snapshot{};
    if (!Collect(snapshot)) return false;
    const auto end = snapshot.sensors.begin() + snapshot.count;
    const auto frame = std::find_if(snapshot.sensors.begin(), end, [](const SensorValue& sensor) {
        return sensor.available && sensor.kind == SensorKind::frame_rate;
    });
    if (frame == end) return false;
    destination = *frame;
    return true;
}

bool SensorBridgeClient::CollectFrameRates(SensorSnapshot& destination) noexcept {
    SensorSnapshot snapshot{};
    if (!Collect(snapshot)) return false;
    bool copied{};
    for (std::uint32_t index{}; index < snapshot.count && destination.count < destination.sensors.size(); ++index) {
        const auto& sensor = snapshot.sensors[index];
        if (!sensor.available || sensor.kind != SensorKind::frame_rate) continue;
        destination.sensors[destination.count++] = sensor;
        copied = true;
    }
    return copied;
}

bool SensorBridgeClient::SetFpsTarget(
    const std::uint32_t process_id,
    const std::uint32_t smoothing_milliseconds,
    const std::uint32_t hardware_polling_milliseconds) noexcept {
    if (!Connect()) return false;
    if (control_pipe_ == nullptr) {
        const auto user_sid = CurrentUserSid();
        if (user_sid.empty()) return false;
        const auto sddl = L"D:(A;;GA;;;SY)(A;;GA;;;" + user_sid + L")";
        PSECURITY_DESCRIPTOR descriptor{};
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        control_pipe_ = CreateNamedPipeW(
            kFpsControlPipeName,
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1U,
            sizeof(SharedFpsControl),
            sizeof(SharedFpsControl),
            100U,
            &attributes);
        LocalFree(descriptor);
        if (control_pipe_ == INVALID_HANDLE_VALUE) {
            control_pipe_ = nullptr;
            return false;
        }
    }
    auto connected = ConnectNamedPipe(control_pipe_, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
    if (!connected && GetLastError() == ERROR_NO_DATA) {
        static_cast<void>(DisconnectNamedPipe(control_pipe_));
        connected = ConnectNamedPipe(control_pipe_, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
    }
    if (!connected) {
        const auto error = GetLastError();
        if (error == ERROR_NO_DATA) static_cast<void>(DisconnectNamedPipe(control_pipe_));
        return error == ERROR_PIPE_LISTENING || error == ERROR_NO_DATA;
    }
    SharedFpsControl request{};
    request.magic = kSensorBridgeMagic;
    request.version = kSensorBridgeVersion;
    request.target_process_id = static_cast<LONG>(process_id);
    request.smoothing_milliseconds = static_cast<LONG>(std::clamp(smoothing_milliseconds, 250U, 1'250U));
    request.hardware_polling_milliseconds = static_cast<LONG>(std::clamp(hardware_polling_milliseconds, 100U, 10'000U));
    DWORD written{};
    const auto success = WriteFile(control_pipe_, &request, sizeof(request), &written, nullptr) && written == sizeof(request);
    // Keep queued bytes alive until the service reads and closes its endpoint.
    // DisconnectNamedPipe immediately after WriteFile discards unread messages.
    if (!success) static_cast<void>(DisconnectNamedPipe(control_pipe_));
    return success;
}

void SensorBridgeClient::Close() noexcept {
    if (control_pipe_ != nullptr) CloseHandle(control_pipe_);
    if (shared_ != nullptr) UnmapViewOfFile(shared_);
    if (mapping_ != nullptr) CloseHandle(mapping_);
    shared_ = nullptr;
    mapping_ = nullptr;
    control_pipe_ = nullptr;
}

} // namespace hardwarescope
