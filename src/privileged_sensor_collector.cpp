#include "hardwarescope/privileged_sensor_collector.hpp"

#include <array>
#include <cwchar>

namespace hardwarescope {
namespace {

void AppendSensor(
    SensorSnapshot& snapshot,
    const std::uint64_t id,
    const wchar_t* const name,
    const wchar_t* const hardware,
    const double celsius) noexcept {
    if (snapshot.count >= snapshot.sensors.size()) return;
    auto& sensor = snapshot.sensors[snapshot.count++];
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

PrivilegedSensorCollector::~PrivilegedSensorCollector() {
    Close();
}

bool PrivilegedSensorCollector::Initialize(const HINSTANCE resources) noexcept {
    Close();
    const auto amd = amd_provider_.Initialize(resources);
    const auto motherboard = motherboard_provider_.Initialize(resources);
    const auto memory = ddr5_provider_.Initialize(resources);
    return amd || motherboard || memory;
}

void PrivilegedSensorCollector::Collect(SensorSnapshot& snapshot) noexcept {
    motherboard_provider_.Collect(snapshot);
    ddr5_provider_.Collect(snapshot);
    if (!amd_provider_.Available()) return;
    AmdZenTemperatures temperatures{};
    if (!amd_provider_.ReadTemperatures(temperatures)) return;
    const auto* const hardware = amd_provider_.ProcessorName().empty() ? L"AMD Ryzen CPU" : amd_provider_.ProcessorName().c_str();
    AppendAmdZenTemperatures(snapshot, temperatures, hardware);
}

void AppendAmdZenTemperatures(SensorSnapshot& snapshot, const AmdZenTemperatures& temperatures, const wchar_t* hardware) noexcept {
    AppendSensor(snapshot, 0x0100'0000'0000'0001ULL, L"Core (Tctl/Tdie)", hardware, temperatures.package_celsius);
    for (std::size_t index = 0U; index < temperatures.ccd_celsius.size(); ++index) {
        if (temperatures.ccd_celsius[index] <= 0.0) continue;
        std::array<wchar_t, 32U> name{};
        static_cast<void>(swprintf_s(name.data(), name.size(), L"CCD%zu (Tdie)", index + 1U));
        AppendSensor(snapshot, 0x0100'0000'0000'0100ULL + index, name.data(), hardware, temperatures.ccd_celsius[index]);
    }
}

void PrivilegedSensorCollector::Close() noexcept {
    amd_provider_.Close();
    ddr5_provider_.Close();
    motherboard_provider_.Close();
}

bool PrivilegedSensorCollector::Available() const noexcept {
    return amd_provider_.Available() || ddr5_provider_.Available() || motherboard_provider_.Available();
}

} // namespace hardwarescope
