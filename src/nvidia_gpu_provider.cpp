#include "hardwarescope/nvidia_gpu_provider.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <new>
#include <string_view>

namespace hardwarescope {
namespace {

constexpr int kNvOk = 0;
constexpr std::uint32_t kNvInitializeId = 0x0150E828U;
constexpr std::uint32_t kNvUnloadId = 0xD22BDD7EU;
constexpr std::uint32_t kNvEnumPhysicalGpusId = 0xE5AC921FU;
constexpr std::uint32_t kNvGpuGetFullNameId = 0xCEEE8E9FU;
constexpr std::uint32_t kNvGpuGetBusIdId = 0x1BE0B8E5U;
constexpr std::uint32_t kNvGpuGetThermalSettingsId = 0xE3640A56U;
constexpr std::uint32_t kNvGpuGetThermalSensorsId = 0x65FE3AADU;
constexpr std::uint32_t kNvGpuGetDynamicPstatesInfoExId = 0x60DED2EDU;
constexpr std::uint32_t kNvGpuGetAllClockFrequenciesId = 0xDCB616C3U;
constexpr std::uint32_t kNvGpuClientFanCoolersGetStatusId = 0x35AED5E8U;
constexpr std::uint32_t kNvThermalTargetAll = 15U;

template <typename Structure>
constexpr std::uint32_t NvVersion(const std::uint32_t version) noexcept {
    return static_cast<std::uint32_t>(sizeof(Structure)) | (version << 16U);
}

struct NvThermalSensor final {
    std::int32_t controller{};
    std::uint32_t default_minimum{};
    std::uint32_t default_maximum{};
    std::uint32_t current{};
    std::int32_t target{};
};

struct NvThermalSettings final {
    std::uint32_t version{};
    std::uint32_t count{};
    std::array<NvThermalSensor, 3U> sensors{};
};

struct NvThermalSensors final {
    std::uint32_t version{};
    std::uint32_t mask{};
    std::array<std::int32_t, 8U> reserved{};
    std::array<std::int32_t, 32U> temperatures{};
};

struct NvDynamicPState final {
    std::uint32_t present{};
    std::int32_t percentage{};
};

struct NvDynamicPStatesInfo final {
    std::uint32_t version{};
    std::uint32_t flags{};
    std::array<NvDynamicPState, 8U> utilization{};
};

struct NvClockDomain final {
    std::uint32_t flags{};
    std::uint32_t frequency_khz{};
};

struct NvClockFrequencies final {
    std::uint32_t version{};
    std::uint32_t reserved{};
    std::array<NvClockDomain, 32U> clocks{};
};

struct NvFanStatusItem final {
    std::uint32_t cooler_id{};
    std::uint32_t current_rpm{};
    std::uint32_t current_minimum_level{};
    std::uint32_t current_maximum_level{};
    std::uint32_t current_level{};
    std::array<std::uint32_t, 8U> reserved{};
};

struct NvFanStatus final {
    std::uint32_t version{};
    std::uint32_t count{};
    std::array<std::uint64_t, 4U> reserved{};
    std::array<NvFanStatusItem, 32U> items{};
};

static_assert(sizeof(NvThermalSettings) == 68U);
static_assert(sizeof(NvThermalSensors) == 168U);
static_assert(sizeof(NvDynamicPStatesInfo) == 72U);
static_assert(sizeof(NvClockFrequencies) == 264U);
static_assert(sizeof(NvFanStatus) == 1'704U);

using NvInitializeFunction = int(__cdecl*)();
using NvEnumPhysicalGpusFunction = int(__cdecl*)(void**, int*);
using NvGpuGetFullNameFunction = int(__cdecl*)(void*, char*);
using NvGpuGetBusIdFunction = int(__cdecl*)(void*, unsigned int*);
using NvGpuGetThermalSettingsFunction = int(__cdecl*)(void*, int, NvThermalSettings*);
using NvGpuGetThermalSensorsFunction = int(__cdecl*)(void*, NvThermalSensors*);
using NvGpuGetDynamicPstatesFunction = int(__cdecl*)(void*, NvDynamicPStatesInfo*);
using NvGpuGetClockFrequenciesFunction = int(__cdecl*)(void*, NvClockFrequencies*);
using NvGpuGetFanStatusFunction = int(__cdecl*)(void*, NvFanStatus*);

struct NvmlMemory final {
    std::uint64_t total{};
    std::uint64_t free{};
    std::uint64_t used{};
};

using NvmlInitializeFunction = int(__cdecl*)();
using NvmlShutdownFunction = int(__cdecl*)();
using NvmlGetDeviceCountFunction = int(__cdecl*)(unsigned int*);
using NvmlGetDeviceHandleByIndexFunction = int(__cdecl*)(unsigned int, void**);
using NvmlGetDeviceHandleByPciBusIdFunction = int(__cdecl*)(const char*, void**);
using NvmlGetMemoryInfoFunction = int(__cdecl*)(void*, NvmlMemory*);
using NvmlGetPowerUsageFunction = int(__cdecl*)(void*, unsigned int*);

std::wstring WidenGpuName(const char* const name) {
    const auto length = strnlen_s(name, 64U);
    if (length == 0U) return L"NVIDIA GPU";
    const auto wide_length = MultiByteToWideChar(CP_UTF8, 0, name, static_cast<int>(length), nullptr, 0);
    if (wide_length <= 0) return L"NVIDIA GPU";
    std::wstring result(static_cast<std::size_t>(wide_length), L'\0');
    static_cast<void>(MultiByteToWideChar(CP_UTF8, 0, name, static_cast<int>(length), result.data(), wide_length));
    return result;
}

bool ContainsInsensitive(const std::wstring_view text, const std::wstring_view search) noexcept {
    if (search.empty() || search.size() > text.size()) return false;
    for (std::size_t index = 0U; index + search.size() <= text.size(); ++index) {
        if (_wcsnicmp(text.data() + index, search.data(), search.size()) == 0) return true;
    }
    return false;
}

void AppendSensor(
    SensorSnapshot& snapshot,
    const std::uint64_t id,
    const SensorKind kind,
    const SensorUnit unit,
    const wchar_t* const name,
    const wchar_t* const hardware,
    const double current) noexcept {
    if (snapshot.count >= snapshot.sensors.size()) return;
    auto& sensor = snapshot.sensors[snapshot.count++];
    sensor.id = id;
    sensor.kind = kind;
    sensor.unit = unit;
    sensor.available = true;
    static_cast<void>(wcsncpy_s(sensor.name.data(), sensor.name.size(), name, _TRUNCATE));
    static_cast<void>(wcsncpy_s(sensor.hardware.data(), sensor.hardware.size(), hardware, _TRUNCATE));
    sensor.current = current;
    sensor.minimum = current;
    sensor.maximum = current;
}

} // namespace

bool IsPlausibleGpuTemperature(const double celsius) noexcept {
    return std::isfinite(celsius) && celsius >= -20.0 && celsius <= 125.0;
}

std::array<char, 16U> FormatNvmlPciBusId(const std::uint32_t bus_id) noexcept {
    std::array<char, 16U> result{};
    if (bus_id > 0xFFU) return result;
    static_cast<void>(sprintf_s(result.data(), result.size(), "0000:%02X:00.0", bus_id));
    return result;
}

NvidiaGpuProvider::~NvidiaGpuProvider() {
    Close();
}

FARPROC NvidiaGpuProvider::Interface(const std::uint32_t id) const noexcept {
    return query_interface_ == nullptr ? nullptr : reinterpret_cast<FARPROC>(query_interface_(id));
}

bool NvidiaGpuProvider::Initialize() noexcept {
    if (initialized_) return Available();
    initialized_ = true;
    module_ = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module_ == nullptr) return false;
    query_interface_ = reinterpret_cast<QueryInterfaceFunction>(GetProcAddress(module_, "nvapi_QueryInterface"));
    if (query_interface_ == nullptr) {
        Close();
        initialized_ = true;
        return false;
    }

    const auto initialize = reinterpret_cast<NvInitializeFunction>(Interface(kNvInitializeId));
    const auto enumerate = reinterpret_cast<NvEnumPhysicalGpusFunction>(Interface(kNvEnumPhysicalGpusId));
    const auto get_name = reinterpret_cast<NvGpuGetFullNameFunction>(Interface(kNvGpuGetFullNameId));
    const auto get_bus_id = reinterpret_cast<NvGpuGetBusIdFunction>(Interface(kNvGpuGetBusIdId));
    if (initialize == nullptr || enumerate == nullptr || get_name == nullptr || initialize() != kNvOk) {
        Close();
        initialized_ = true;
        return false;
    }

    unload_ = Interface(kNvUnloadId);
    get_thermal_settings_ = Interface(kNvGpuGetThermalSettingsId);
    get_thermal_sensors_ = Interface(kNvGpuGetThermalSensorsId);
    get_dynamic_pstates_ = Interface(kNvGpuGetDynamicPstatesInfoExId);
    get_clock_frequencies_ = Interface(kNvGpuGetAllClockFrequenciesId);
    get_fan_status_ = Interface(kNvGpuClientFanCoolersGetStatusId);

    std::array<void*, kMaximumGpus> handles{};
    int count{};
    if (enumerate(handles.data(), &count) != kNvOk || count <= 0) return false;
    gpu_count_ = std::min<std::size_t>(static_cast<std::size_t>(count), gpus_.size());
    for (std::size_t index = 0U; index < gpu_count_; ++index) {
        auto& gpu = gpus_[index];
        gpu.handle = handles[index];
        std::array<char, 64U> name{};
        gpu.name = get_name(gpu.handle, name.data()) == kNvOk ? WidenGpuName(name.data()) : L"NVIDIA GPU";
        gpu.has_pci_bus_id = get_bus_id != nullptr && get_bus_id(gpu.handle, &gpu.pci_bus_id) == kNvOk
            && gpu.pci_bus_id <= 0xFFU;
        gpu.rtx_50_series = ContainsInsensitive(gpu.name, L"RTX 50");

        if (get_thermal_sensors_ != nullptr) {
            const auto get_sensors = reinterpret_cast<NvGpuGetThermalSensorsFunction>(get_thermal_sensors_);
            bool found_sensor = false;
            for (std::uint32_t bit = 0U; bit < 32U; ++bit) {
                NvThermalSensors sensors{};
                sensors.version = NvVersion<NvThermalSensors>(2U);
                sensors.mask = 1U << bit;
                if (get_sensors(gpu.handle, &sensors) == kNvOk) {
                    found_sensor = true;
                    gpu.thermal_mask = bit == 31U ? 0xFFFF'FFFFU : ((1U << (bit + 1U)) - 1U);
                } else {
                    break;
                }
            }
            if (!found_sensor) gpu.thermal_mask = 0U;
        }

        if (get_clock_frequencies_ != nullptr) {
            const auto get_clocks = reinterpret_cast<NvGpuGetClockFrequenciesFunction>(get_clock_frequencies_);
            for (std::uint32_t version = 1U; version <= 3U; ++version) {
                NvClockFrequencies clocks{};
                clocks.version = NvVersion<NvClockFrequencies>(version);
                if (get_clocks(gpu.handle, &clocks) == kNvOk) {
                    gpu.clock_version = version;
                    break;
                }
            }
        }
    }

    nvml_module_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (nvml_module_ != nullptr) {
        const auto nvml_initialize = reinterpret_cast<NvmlInitializeFunction>(GetProcAddress(nvml_module_, "nvmlInit_v2"));
        const auto nvml_get_count = reinterpret_cast<NvmlGetDeviceCountFunction>(GetProcAddress(nvml_module_, "nvmlDeviceGetCount_v2"));
        const auto nvml_get_handle = reinterpret_cast<NvmlGetDeviceHandleByIndexFunction>(GetProcAddress(nvml_module_, "nvmlDeviceGetHandleByIndex_v2"));
        const auto nvml_get_handle_by_bus = reinterpret_cast<NvmlGetDeviceHandleByPciBusIdFunction>(GetProcAddress(nvml_module_, "nvmlDeviceGetHandleByPciBusId_v2"));
        if (nvml_initialize != nullptr && nvml_get_count != nullptr && nvml_get_handle != nullptr && nvml_initialize() == kNvOk) {
            nvml_shutdown_ = GetProcAddress(nvml_module_, "nvmlShutdown");
            nvml_get_memory_info_ = GetProcAddress(nvml_module_, "nvmlDeviceGetMemoryInfo");
            nvml_get_power_usage_ = GetProcAddress(nvml_module_, "nvmlDeviceGetPowerUsage");
            unsigned int nvml_count{};
            if (nvml_get_count(&nvml_count) == kNvOk) {
                const auto mapped_count = std::min<std::size_t>(gpu_count_, nvml_count);
                for (std::size_t index = 0U; index < mapped_count; ++index) {
                    auto& gpu = gpus_[index];
                    bool mapped{};
                    if (gpu.has_pci_bus_id && nvml_get_handle_by_bus != nullptr) {
                        const auto bus_id = FormatNvmlPciBusId(gpu.pci_bus_id);
                        mapped = nvml_get_handle_by_bus(bus_id.data(), &gpu.nvml_handle) == kNvOk;
                    }
                    // Different APIs need not enumerate adapters in the same order.
                    if (!mapped) gpu.nvml_handle = nullptr;
                }
            }
        } else {
            FreeLibrary(nvml_module_);
            nvml_module_ = nullptr;
        }
    }
    return Available();
}

void NvidiaGpuProvider::Close() noexcept {
    if (nvml_module_ != nullptr) {
        if (nvml_shutdown_ != nullptr) static_cast<void>(reinterpret_cast<NvmlShutdownFunction>(nvml_shutdown_)());
        FreeLibrary(nvml_module_);
    }
    nvml_module_ = nullptr;
    nvml_shutdown_ = nullptr;
    nvml_get_memory_info_ = nullptr;
    nvml_get_power_usage_ = nullptr;
    if (module_ != nullptr) {
        if (unload_ != nullptr) static_cast<void>(reinterpret_cast<NvInitializeFunction>(unload_)());
        FreeLibrary(module_);
    }
    module_ = nullptr;
    query_interface_ = nullptr;
    unload_ = nullptr;
    get_thermal_settings_ = nullptr;
    get_thermal_sensors_ = nullptr;
    get_dynamic_pstates_ = nullptr;
    get_clock_frequencies_ = nullptr;
    get_fan_status_ = nullptr;
    gpus_ = {};
    gpu_count_ = 0U;
    if (refresh_buffer_ != nullptr) ResetSnapshot(*refresh_buffer_);
    last_refresh_ = {};
    initialized_ = false;
}

void NvidiaGpuProvider::Collect(SensorSnapshot& snapshot) noexcept {
    if (!initialized_) static_cast<void>(Initialize());
    if (!Available()) return;
    const auto append_cached = [&] {
        if (refresh_buffer_ == nullptr) return;
        for (std::uint32_t index = 0U; index < refresh_buffer_->count && snapshot.count < snapshot.sensors.size(); ++index) {
            snapshot.sensors[snapshot.count++] = refresh_buffer_->sensors[index];
        }
    };
    const auto now = std::chrono::steady_clock::now();
    if (last_refresh_ != std::chrono::steady_clock::time_point{} && now - last_refresh_ < std::chrono::milliseconds{250}) {
        append_cached();
        return;
    }
    if (refresh_buffer_ == nullptr) refresh_buffer_.reset(new (std::nothrow) SensorSnapshot{});
    if (refresh_buffer_ == nullptr) return;
    auto& refreshed = *refresh_buffer_;
    ResetSnapshot(refreshed);
    for (std::size_t gpu_index = 0U; gpu_index < gpu_count_; ++gpu_index) {
        const auto& gpu = gpus_[gpu_index];
        const auto id_base = 0x0200'0000'0000'0000ULL | (static_cast<std::uint64_t>(gpu_index) << 16U);

        double core_temperature = -1'000.0;
        if (get_thermal_settings_ != nullptr) {
            NvThermalSettings settings{};
            settings.version = NvVersion<NvThermalSettings>(2U);
            settings.count = static_cast<std::uint32_t>(settings.sensors.size());
            const auto get_settings = reinterpret_cast<NvGpuGetThermalSettingsFunction>(get_thermal_settings_);
            if (get_settings(gpu.handle, static_cast<int>(kNvThermalTargetAll), &settings) == kNvOk) {
                const auto count = std::min<std::size_t>(settings.count, settings.sensors.size());
                for (std::size_t index = 0U; index < count; ++index) {
                    if (settings.sensors[index].target == 1) {
                        core_temperature = static_cast<double>(settings.sensors[index].current);
                        break;
                    }
                }
            }
        }

        double memory_junction = -1'000.0;
        double hot_spot = -1'000.0;
        if (gpu.thermal_mask != 0U && get_thermal_sensors_ != nullptr) {
            NvThermalSensors sensors{};
            sensors.version = NvVersion<NvThermalSensors>(2U);
            sensors.mask = gpu.thermal_mask;
            const auto get_sensors = reinterpret_cast<NvGpuGetThermalSensorsFunction>(get_thermal_sensors_);
            if (get_sensors(gpu.handle, &sensors) == kNvOk) {
                if (gpu.rtx_50_series) {
                    core_temperature = static_cast<double>(sensors.temperatures[1]) / 256.0;
                    memory_junction = static_cast<double>(sensors.temperatures[2]) / 256.0;
                } else if (ContainsInsensitive(gpu.name, L"RTX 40")) {
                    hot_spot = static_cast<double>(sensors.temperatures[1]) / 256.0;
                    memory_junction = static_cast<double>(sensors.temperatures[7]) / 256.0;
                } else {
                    hot_spot = static_cast<double>(sensors.temperatures[1]) / 256.0;
                    memory_junction = static_cast<double>(sensors.temperatures[9]) / 256.0;
                }
            }
        }
        if (IsPlausibleGpuTemperature(core_temperature)) AppendSensor(refreshed, id_base | 0x0001U, SensorKind::temperature, SensorUnit::celsius, L"GPU Core temperature", gpu.name.c_str(), core_temperature);
        if (IsPlausibleGpuTemperature(memory_junction)) AppendSensor(refreshed, id_base | 0x0002U, SensorKind::temperature, SensorUnit::celsius, L"GPU Memory Junction temperature", gpu.name.c_str(), memory_junction);
        if (IsPlausibleGpuTemperature(hot_spot)) AppendSensor(refreshed, id_base | 0x0003U, SensorKind::temperature, SensorUnit::celsius, L"GPU Hot Spot temperature", gpu.name.c_str(), hot_spot);

        if (get_dynamic_pstates_ != nullptr) {
            NvDynamicPStatesInfo states{};
            states.version = NvVersion<NvDynamicPStatesInfo>(1U);
            const auto get_states = reinterpret_cast<NvGpuGetDynamicPstatesFunction>(get_dynamic_pstates_);
            if (get_states(gpu.handle, &states) == kNvOk) {
                constexpr std::array<const wchar_t*, 4U> names{L"GPU Core usage", L"GPU Memory controller usage", L"GPU Video engine usage", L"GPU Bus usage"};
                for (std::size_t index = 0U; index < names.size(); ++index) {
                    const auto& state = states.utilization[index];
                    if ((state.present & 1U) == 0U || state.percentage < 0 || state.percentage > 100) continue;
                    AppendSensor(refreshed, id_base | (0x0100U + index), SensorKind::utilization, SensorUnit::percent, names[index], gpu.name.c_str(), state.percentage);
                }
            }
        }

        if (gpu.clock_version != 0U && get_clock_frequencies_ != nullptr) {
            NvClockFrequencies clocks{};
            clocks.version = NvVersion<NvClockFrequencies>(gpu.clock_version);
            const auto get_clocks = reinterpret_cast<NvGpuGetClockFrequenciesFunction>(get_clock_frequencies_);
            if (get_clocks(gpu.handle, &clocks) == kNvOk) {
                constexpr std::array<std::pair<std::size_t, const wchar_t*>, 4U> clock_names{{
                    {0U, L"GPU Core clock"},
                    {4U, L"GPU Memory clock"},
                    {7U, L"GPU Shader clock"},
                    {8U, L"GPU Video clock"}}};
                for (std::size_t output_index = 0U; output_index < clock_names.size(); ++output_index) {
                    const auto [clock_index, name] = clock_names[output_index];
                    const auto& clock = clocks.clocks[clock_index];
                    if ((clock.flags & 1U) == 0U || clock.frequency_khz == 0U) continue;
                    AppendSensor(refreshed, id_base | (0x0200U + output_index), SensorKind::clock, SensorUnit::megahertz, name, gpu.name.c_str(), static_cast<double>(clock.frequency_khz) / 1'000.0);
                }
            }
        }

        if (get_fan_status_ != nullptr) {
            NvFanStatus status{};
            status.version = NvVersion<NvFanStatus>(1U);
            const auto get_fans = reinterpret_cast<NvGpuGetFanStatusFunction>(get_fan_status_);
            if (get_fans(gpu.handle, &status) == kNvOk) {
                const auto count = std::min<std::size_t>(status.count, status.items.size());
                for (std::size_t index = 0U; index < count; ++index) {
                    if (status.items[index].current_rpm > 20'000U) continue;
                    std::array<wchar_t, 32U> name{};
                    static_cast<void>(swprintf_s(name.data(), name.size(), L"GPU Fan #%zu", index + 1U));
                    AppendSensor(refreshed, id_base | (0x0300U + index), SensorKind::fan, SensorUnit::revolutions_per_minute, name.data(), gpu.name.c_str(), status.items[index].current_rpm);
                }
            }
        }

        if (gpu.nvml_handle != nullptr && nvml_get_memory_info_ != nullptr) {
            NvmlMemory memory{};
            const auto get_memory = reinterpret_cast<NvmlGetMemoryInfoFunction>(nvml_get_memory_info_);
            if (get_memory(gpu.nvml_handle, &memory) == kNvOk && memory.total > 0U && memory.used <= memory.total) {
                constexpr double bytes_per_megabyte = 1024.0 * 1024.0;
                AppendSensor(refreshed, id_base | 0x0400U, SensorKind::data, SensorUnit::megabytes, L"GPU Memory Used", gpu.name.c_str(), static_cast<double>(memory.used) / bytes_per_megabyte);
                AppendSensor(refreshed, id_base | 0x0401U, SensorKind::data, SensorUnit::megabytes, L"GPU Memory Total", gpu.name.c_str(), static_cast<double>(memory.total) / bytes_per_megabyte);
                AppendSensor(refreshed, id_base | 0x0402U, SensorKind::utilization, SensorUnit::percent, L"GPU Memory Usage", gpu.name.c_str(), static_cast<double>(memory.used) * 100.0 / static_cast<double>(memory.total));
            }
        }
        if (gpu.nvml_handle != nullptr && nvml_get_power_usage_ != nullptr) {
            unsigned int milliwatts{};
            const auto get_power = reinterpret_cast<NvmlGetPowerUsageFunction>(nvml_get_power_usage_);
            if (get_power(gpu.nvml_handle, &milliwatts) == kNvOk && milliwatts <= 2'000'000U) {
                AppendSensor(refreshed, id_base | 0x0500U, SensorKind::power, SensorUnit::watts, L"GPU Board Power", gpu.name.c_str(), static_cast<double>(milliwatts) / 1'000.0);
            }
        }
    }
    last_refresh_ = now;
    append_cached();
}

} // namespace hardwarescope
