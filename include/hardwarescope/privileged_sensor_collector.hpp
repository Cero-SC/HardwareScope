#pragma once

#include "hardwarescope/amd_zen_provider.hpp"
#include "hardwarescope/ddr5_temperature_provider.hpp"
#include "hardwarescope/nct6687_provider.hpp"
#include "hardwarescope/sensor_snapshot.hpp"

#include <windows.h>

namespace hardwarescope {

void AppendAmdZenTemperatures(SensorSnapshot& snapshot, const AmdZenTemperatures& temperatures,
    const wchar_t* hardware) noexcept;

class PrivilegedSensorCollector final {
public:
    PrivilegedSensorCollector() = default;
    ~PrivilegedSensorCollector();

    PrivilegedSensorCollector(const PrivilegedSensorCollector&) = delete;
    PrivilegedSensorCollector& operator=(const PrivilegedSensorCollector&) = delete;

    [[nodiscard]] bool Initialize(HINSTANCE resources) noexcept;
    void Close() noexcept;
    void Collect(SensorSnapshot& snapshot) noexcept;

    [[nodiscard]] bool Available() const noexcept;

private:
    AmdZenProvider amd_provider_{};
    Ddr5TemperatureProvider ddr5_provider_{};
    Nct6687Provider motherboard_provider_{};
};

} // namespace hardwarescope
