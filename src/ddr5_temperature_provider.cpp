// The SMBus protocol and DDR5 register interpretation are derived from
// RAMSPDToolkit (MPL-2.0), revision 3b47b960e0830fef344624ad5e389675d5f0a1ce.
// HardwareScope intentionally performs reads only. It never changes an SPD page
// or writes any other DIMM register.

#include "hardwarescope/ddr5_temperature_provider.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <span>

namespace hardwarescope {
namespace {

constexpr int kSmbusPiix4Resource = 203;
constexpr auto kRefreshInterval = std::chrono::seconds{2};
constexpr std::uint8_t kFirstSpdAddress = 0x50U;
constexpr std::uint8_t kLastSpdAddress = 0x57U;
constexpr std::uint8_t kRead = 1U;
constexpr std::uint64_t kSmbusByteData = 2U;
constexpr std::uint64_t kSmbusWordData = 3U;
constexpr std::uint8_t kVirtualPageRegister = 0x0BU;
constexpr std::uint8_t kDeviceCapabilityRegister = 0x05U;
constexpr std::uint8_t kThermalSensorEnabledRegister = 0x1AU;
constexpr std::uint8_t kTemperatureRegister = 0x31U;

HANDLE CreateOrOpenWorldMutex(const wchar_t* const name) noexcept {
    auto handle = CreateMutexW(nullptr, FALSE, name);
    if (handle == nullptr && GetLastError() == ERROR_ACCESS_DENIED) {
        handle = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
    }
    return handle;
}

void AppendSensor(
    std::array<SensorValue, 16U>& sensors,
    std::size_t& count,
    const std::uint64_t id,
    const wchar_t* const name,
    const wchar_t* const hardware,
    const double celsius) noexcept {
    if (count >= sensors.size()) return;
    auto& sensor = sensors[count++];
    sensor.id = id;
    sensor.kind = SensorKind::temperature;
    sensor.unit = SensorUnit::celsius;
    sensor.available = true;
    static_cast<void>(wcsncpy_s(sensor.name.data(), sensor.name.size(), name, _TRUNCATE));
    static_cast<void>(wcsncpy_s(sensor.hardware.data(), sensor.hardware.size(), hardware, _TRUNCATE));
    sensor.current = celsius;
    sensor.minimum = celsius;
    sensor.maximum = celsius;
}

} // namespace

double ConvertDdr5TemperatureRaw(std::uint16_t raw) noexcept {
    if ((raw & 0x1000U) != 0U) {
        raw = static_cast<std::uint16_t>(raw & ~0x1000U);
        return static_cast<double>(raw) * 0.0625 - 256.0;
    }
    return static_cast<double>(raw) * 0.0625;
}

bool IsPlausibleDdr5Temperature(const double celsius) noexcept {
    return std::isfinite(celsius) && celsius >= -20.0 && celsius <= 125.0;
}

Ddr5TemperatureProvider::~Ddr5TemperatureProvider() {
    Close();
}

bool Ddr5TemperatureProvider::SelectPiix4Port(Bus& bus, const std::uint64_t port) noexcept {
    const std::array<std::uint64_t, 1U> input{port};
    std::array<std::uint64_t, 1U> output{};
    std::size_t returned{};
    return bus.executor.Execute("ioctl_piix4_port_sel", input, output, returned) && returned >= 1U;
}

bool Ddr5TemperatureProvider::ReadByte(
    Bus& bus,
    const std::uint8_t address,
    const std::uint8_t command,
    std::uint8_t& value) noexcept {
    const std::array<std::uint64_t, 9U> input{address, kRead, command, kSmbusByteData};
    std::array<std::uint64_t, 1U> output{};
    std::size_t returned{};
    if (!bus.executor.Execute("ioctl_smbus_xfer", input, output, returned) || returned < 1U) return false;
    value = static_cast<std::uint8_t>(output[0]);
    return true;
}

bool Ddr5TemperatureProvider::ReadWord(
    Bus& bus,
    const std::uint8_t address,
    const std::uint8_t command,
    std::uint16_t& value) noexcept {
    const std::array<std::uint64_t, 9U> input{address, kRead, command, kSmbusWordData};
    std::array<std::uint64_t, 2U> output{};
    std::size_t returned{};
    if (!bus.executor.Execute("ioctl_smbus_xfer", input, output, returned) || returned < 1U) return false;
    value = static_cast<std::uint16_t>(output[0]);
    return true;
}

bool Ddr5TemperatureProvider::AcquireMutex(HANDLE const mutex, bool& owned, const DWORD timeout_ms) noexcept {
    if (mutex == nullptr) { owned = false; return false; }
    const auto result = WaitForSingleObject(mutex, timeout_ms);
    owned = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    return owned;
}

void Ddr5TemperatureProvider::ReleaseMutex(HANDLE const mutex, bool& owned) noexcept {
    if (mutex != nullptr && owned) static_cast<void>(::ReleaseMutex(mutex));
    owned = false;
}

bool Ddr5TemperatureProvider::Initialize(const HINSTANCE resources) noexcept {
    Close();
    smbus_mutex_ = CreateOrOpenWorldMutex(L"Global\\Access_SMBUS.HTP.Method");
    pci_mutex_ = CreateOrOpenWorldMutex(L"Global\\Access_PCI");
    if (!AcquireMutex(smbus_mutex_, smbus_mutex_owned_, 250U)) {
        last_error_ = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        Close();
        return false;
    }
    if (!AcquireMutex(pci_mutex_, pci_mutex_owned_, 250U)) {
        last_error_ = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        ReleaseMutex(smbus_mutex_, smbus_mutex_owned_);
        Close();
        return false;
    }

    for (std::size_t index = 0U; index < buses_.size(); ++index) {
        auto& bus = buses_[index];
        if (bus.executor.LoadModuleFromResource(resources, kSmbusPiix4Resource)
            && SelectPiix4Port(bus, index)) {
            bus.available = true;
            available_ = true;
        } else {
            last_error_ = bus.executor.LastError();
            bus.executor.Close();
        }
    }
    ReleaseMutex(pci_mutex_, pci_mutex_owned_);
    ReleaseMutex(smbus_mutex_, smbus_mutex_owned_);
    if (!available_) {
        Close();
        return false;
    }
    last_error_ = S_OK;
    Refresh();
    return true;
}

void Ddr5TemperatureProvider::Refresh() noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (last_refresh_.time_since_epoch().count() != 0 && now - last_refresh_ < kRefreshInterval) return;
    last_refresh_ = now;
    // Cached values are valid only until the next attempted measurement.
    cached_sensor_count_ = 0U;
    if (!AcquireMutex(smbus_mutex_, smbus_mutex_owned_, 100U)) return;

    std::array<SensorValue, 16U> refreshed{};
    std::size_t refreshed_count{};
    for (std::size_t bus_index = 0U; bus_index < buses_.size(); ++bus_index) {
        auto& bus = buses_[bus_index];
        if (!bus.available) continue;
        for (std::uint8_t address = kFirstSpdAddress; address <= kLastSpdAddress; ++address) {
            std::uint8_t page{};
            std::uint8_t device_most{};
            std::uint8_t device_least{};
            std::uint8_t capability{};
            std::uint8_t enabled{};
            std::uint16_t raw_temperature{};
            if (!ReadByte(bus, address, kVirtualPageRegister, page) || (page & 0x07U) != 0U
                || !ReadByte(bus, address, 0x00U, device_most) || device_most != 0x51U
                || !ReadByte(bus, address, 0x01U, device_least) || device_least != 0x18U
                || !ReadByte(bus, address, kDeviceCapabilityRegister, capability) || (capability & 0x02U) == 0U
                || !ReadByte(bus, address, kThermalSensorEnabledRegister, enabled) || enabled != 0U
                || !ReadWord(bus, address, kTemperatureRegister, raw_temperature)) {
                continue;
            }
            const auto celsius = ConvertDdr5TemperatureRaw(raw_temperature);
            if (!IsPlausibleDdr5Temperature(celsius)) continue;

            std::array<wchar_t, 48U> name{};
            std::array<wchar_t, 96U> hardware{};
            static_cast<void>(swprintf_s(name.data(), name.size(), L"DIMM P%zu-%u temperature", bus_index, static_cast<unsigned>(address - kFirstSpdAddress)));
            static_cast<void>(swprintf_s(hardware.data(), hardware.size(), L"DDR5 module at SMBus 0x%02X", static_cast<unsigned>(address)));
            const auto id = 0x0400'0000'0000'0000ULL
                | (static_cast<std::uint64_t>(bus_index) << 8U)
                | static_cast<std::uint64_t>(address);
            AppendSensor(refreshed, refreshed_count, id, name.data(), hardware.data(), celsius);
        }
    }
    ReleaseMutex(smbus_mutex_, smbus_mutex_owned_);
    cached_sensors_ = refreshed;
    cached_sensor_count_ = refreshed_count;
}

void Ddr5TemperatureProvider::Collect(SensorSnapshot& snapshot) noexcept {
    if (!available_) return;
    Refresh();
    const auto available_slots = snapshot.sensors.size() - std::min<std::size_t>(snapshot.count, snapshot.sensors.size());
    const auto copy_count = std::min(cached_sensor_count_, available_slots);
    std::copy_n(cached_sensors_.begin(), copy_count, snapshot.sensors.begin() + snapshot.count);
    snapshot.count += static_cast<std::uint32_t>(copy_count);
}

void Ddr5TemperatureProvider::Close() noexcept {
    ReleaseMutex(pci_mutex_, pci_mutex_owned_);
    ReleaseMutex(smbus_mutex_, smbus_mutex_owned_);
    for (auto& bus : buses_) {
        bus.executor.Close();
        bus.available = false;
    }
    if (pci_mutex_ != nullptr) CloseHandle(pci_mutex_);
    if (smbus_mutex_ != nullptr) CloseHandle(smbus_mutex_);
    pci_mutex_ = nullptr;
    smbus_mutex_ = nullptr;
    cached_sensors_ = {};
    cached_sensor_count_ = 0U;
    last_refresh_ = {};
    available_ = false;
}

} // namespace hardwarescope
