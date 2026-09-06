#include "hardwarescope/privileged_sensor_collector.hpp"
#include "hardwarescope/presentmon_fps_runner.hpp"
#include "hardwarescope/sensor_bridge.hpp"
#include "hardwarescope/service_path.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <memory>
#include <new>
#include <string_view>

namespace {

constexpr wchar_t kServiceName[] = L"HardwareScopeSensorService";
constexpr wchar_t kServiceDisplayName[] = L"HardwareScope Sensor Service";
SERVICE_STATUS_HANDLE service_status_handle{};
SERVICE_STATUS service_status{};
HANDLE stop_event{};

void SetServiceState(const DWORD state, const DWORD error = NO_ERROR) noexcept {
    service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service_status.dwCurrentState = state;
    service_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0U;
    service_status.dwWin32ExitCode = error;
    service_status.dwCheckPoint = 0U;
    service_status.dwWaitHint = 0U;
    if (service_status_handle != nullptr) static_cast<void>(SetServiceStatus(service_status_handle, &service_status));
}

DWORD WINAPI ServiceControl(const DWORD control, const DWORD, void*, void*) noexcept {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        SetServiceState(SERVICE_STOP_PENDING);
        if (stop_event != nullptr) SetEvent(stop_event);
    }
    return NO_ERROR;
}

int RunCollector(const HINSTANCE resources, HANDLE const requested_stop, const DWORD maximum_runtime_ms) noexcept {
    hardwarescope::SensorBridgePublisher publisher;
    if (!publisher.Initialize()) return static_cast<int>(GetLastError());
    hardwarescope::PrivilegedSensorCollector collector;
    hardwarescope::PresentMonFpsRunner fps_runner;
    static_cast<void>(collector.Initialize(resources));
    const std::unique_ptr<hardwarescope::SensorSnapshot> snapshot(new (std::nothrow) hardwarescope::SensorSnapshot{});
    if (snapshot == nullptr) return static_cast<int>(ERROR_OUTOFMEMORY);
    const auto started = std::chrono::steady_clock::now();
    auto next_hardware_refresh = std::chrono::steady_clock::time_point{};
    const std::unique_ptr<hardwarescope::SensorSnapshot> hardware(new (std::nothrow) hardwarescope::SensorSnapshot{});
    if (hardware == nullptr) return static_cast<int>(ERROR_OUTOFMEMORY);
    std::uint64_t sequence{};
    while (true) {
        LARGE_INTEGER frequency{};
        LARGE_INTEGER before{};
        LARGE_INTEGER after{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&before);
        const auto requested_target = publisher.RequestedFpsTarget();
        const auto requested_smoothing = publisher.RequestedFpsSmoothing();
        const auto requested_hardware_interval = publisher.RequestedHardwarePollingInterval();
        const auto now = std::chrono::steady_clock::now();
        if (hardware->sequence == 0U || now >= next_hardware_refresh) {
            hardwarescope::ResetSnapshot(*hardware);
            hardware->sequence = sequence + 1U;
            hardware->captured_qpc = static_cast<std::uint64_t>(before.QuadPart);
            collector.Collect(*hardware);
            next_hardware_refresh = now + std::chrono::milliseconds{requested_hardware_interval};
        }
        hardwarescope::CopySnapshot(*hardware, *snapshot);
        snapshot->sequence = ++sequence;
        snapshot->captured_qpc = static_cast<std::uint64_t>(before.QuadPart);
        fps_runner.SetTarget(requested_target, requested_smoothing);
        const auto fps = fps_runner.Snapshot();
        if (fps.available && snapshot->count < snapshot->sensors.size()) {
            auto& sensor = snapshot->sensors[snapshot->count++];
            sensor.id = hardwarescope::kFpsSensorId;
            sensor.kind = hardwarescope::SensorKind::frame_rate;
            sensor.unit = hardwarescope::SensorUnit::frames_per_second;
            sensor.available = true;
            static_cast<void>(wcsncpy_s(sensor.name.data(), sensor.name.size(), L"Frame rate", _TRUNCATE));
            static_cast<void>(wcsncpy_s(sensor.hardware.data(), sensor.hardware.size(), fps.application.data(), _TRUNCATE));
            sensor.current = fps.frames_per_second;
            sensor.minimum = fps.frames_per_second;
            sensor.maximum = fps.frames_per_second;

            if (fps.one_percent_low_frames_per_second != 0U && snapshot->count < snapshot->sensors.size()) {
                auto& low = snapshot->sensors[snapshot->count++];
                low.id = hardwarescope::kFpsOnePercentLowSensorId;
                low.kind = hardwarescope::SensorKind::frame_rate;
                low.unit = hardwarescope::SensorUnit::frames_per_second;
                low.available = true;
                static_cast<void>(wcsncpy_s(low.name.data(), low.name.size(), L"1% low frame rate", _TRUNCATE));
                static_cast<void>(wcsncpy_s(low.hardware.data(), low.hardware.size(), fps.application.data(), _TRUNCATE));
                low.current = fps.one_percent_low_frames_per_second;
                low.minimum = fps.one_percent_low_frames_per_second;
                low.maximum = fps.one_percent_low_frames_per_second;
            }
            if (fps.frame_time_milliseconds > 0.0 && snapshot->count < snapshot->sensors.size()) {
                auto& frame_time = snapshot->sensors[snapshot->count++];
                frame_time.id = hardwarescope::kFpsFrameTimeSensorId;
                frame_time.kind = hardwarescope::SensorKind::frame_rate;
                frame_time.unit = hardwarescope::SensorUnit::milliseconds;
                frame_time.available = true;
                static_cast<void>(wcsncpy_s(frame_time.name.data(), frame_time.name.size(), L"Frame time", _TRUNCATE));
                static_cast<void>(wcsncpy_s(frame_time.hardware.data(), frame_time.hardware.size(), fps.application.data(), _TRUNCATE));
                frame_time.current = fps.frame_time_milliseconds;
                frame_time.minimum = fps.frame_time_milliseconds;
                frame_time.maximum = fps.frame_time_milliseconds;
            }
        }
        QueryPerformanceCounter(&after);
        if (frequency.QuadPart > 0) {
            snapshot->collection_microseconds = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(after.QuadPart - before.QuadPart) * 1'000'000ULL)
                / static_cast<std::uint64_t>(frequency.QuadPart));
        }
        publisher.Publish(*snapshot);
        const auto after_publish = std::chrono::steady_clock::now();
        auto wait_milliseconds = 100U;
        if (requested_target != 0U) {
            wait_milliseconds = 50U;
        } else if (next_hardware_refresh > after_publish) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(next_hardware_refresh - after_publish).count() + 1;
            wait_milliseconds = static_cast<DWORD>(std::clamp<std::int64_t>(remaining, 1, 100));
        }
        const auto wait = requested_stop == nullptr ? WAIT_TIMEOUT : WaitForSingleObject(requested_stop, wait_milliseconds);
        if (wait == WAIT_OBJECT_0) break;
        if (maximum_runtime_ms != INFINITE
            && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds{maximum_runtime_ms}) break;
        if (requested_stop == nullptr) Sleep(wait_milliseconds);
    }
    collector.Close();
    fps_runner.Stop();
    return 0;
}

void WINAPI ServiceMain(const DWORD, wchar_t**) noexcept {
    service_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, &ServiceControl, nullptr);
    if (service_status_handle == nullptr) return;
    SetServiceState(SERVICE_START_PENDING);
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
        SetServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }
    SetServiceState(SERVICE_RUNNING);
    const auto result = RunCollector(GetModuleHandleW(nullptr), stop_event, INFINITE);
    CloseHandle(stop_event);
    stop_event = nullptr;
    SetServiceState(SERVICE_STOPPED, static_cast<DWORD>(result));
}

int InstallService() noexcept {
    std::array<wchar_t, 32'768U> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size()) return static_cast<int>(GetLastError());
    const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) return static_cast<int>(GetLastError());
    const auto service_path = hardwarescope::QuoteServiceBinaryPath(std::wstring_view{path.data(), length});
    auto service = CreateServiceW(
        manager,
        kServiceName,
        kServiceDisplayName,
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        service_path.c_str(),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    if (service == nullptr && GetLastError() == ERROR_SERVICE_EXISTS) {
        service = OpenServiceW(manager, kServiceName, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START);
        if (service != nullptr) {
            if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, service_path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, kServiceDisplayName)) {
                const auto error = GetLastError();
                CloseServiceHandle(service);
                CloseServiceHandle(manager);
                return static_cast<int>(error);
            }
        }
    }
    if (service == nullptr) {
        const auto error = GetLastError();
        CloseServiceHandle(manager);
        return static_cast<int>(error);
    }
    SERVICE_DESCRIPTIONW description{const_cast<LPWSTR>(L"Provides read-only privileged hardware sensor data to HardwareScope.")};
    SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};
    static_cast<void>(ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description));
    static_cast<void>(ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed));
    if (!StartServiceW(service, 0U, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        const auto error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return static_cast<int>(error);
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return 0;
}

int UninstallService() noexcept {
    const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) return static_cast<int>(GetLastError());
    const auto service = OpenServiceW(manager, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (service == nullptr) {
        const auto error = GetLastError();
        CloseServiceHandle(manager);
        return error == ERROR_SERVICE_DOES_NOT_EXIST ? 0 : static_cast<int>(error);
    }
    SERVICE_STATUS status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        const auto error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return static_cast<int>(error);
    }
    bool stopped{};
    for (int attempt = 0; attempt < 100; ++attempt) {
        SERVICE_STATUS_PROCESS process_status{};
        DWORD needed{};
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&process_status), sizeof(process_status), &needed)) {
            const auto error = GetLastError();
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return static_cast<int>(error);
        }
        if (process_status.dwCurrentState == SERVICE_STOPPED) {
            stopped = true;
            break;
        }
        Sleep(100U);
    }
    if (!stopped) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return static_cast<int>(ERROR_SERVICE_REQUEST_TIMEOUT);
    }
    const auto deleted = DeleteService(service);
    const auto error = deleted ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return error == ERROR_SUCCESS || error == ERROR_SERVICE_MARKED_FOR_DELETE ? 0 : static_cast<int>(error);
}

} // namespace

int wmain(const int argument_count, wchar_t** arguments) {
    if (argument_count == 2 && std::wstring_view{arguments[1]} == L"--check-runtime") {
        hardwarescope::PawnIoExecutor runtime;
        return runtime.CheckRuntime() ? 0 : 1;
    }
    if (argument_count == 2 && std::wstring_view{arguments[1]} == L"--install") return InstallService();
    if (argument_count == 2 && std::wstring_view{arguments[1]} == L"--uninstall") return UninstallService();
    if (argument_count == 3 && std::wstring_view{arguments[1]} == L"--test-ms") {
        const auto duration = std::clamp<DWORD>(wcstoul(arguments[2], nullptr, 10), 500U, 60'000U);
        return RunCollector(GetModuleHandleW(nullptr), nullptr, duration);
    }
    SERVICE_TABLE_ENTRYW services[]{
        {const_cast<LPWSTR>(kServiceName), &ServiceMain},
        {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(services) ? 0 : static_cast<int>(GetLastError());
}
