// NCT6687D-R register locations and formulas are derived from
// LibreHardwareMonitor v0.9.6 (MPL-2.0).

#include "hardwarescope/nct6687_provider.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <span>
#include <string_view>
#include <utility>

namespace hardwarescope {
namespace {

constexpr int kLpcIoResource = 202;
constexpr std::uint8_t kChipIdRegister = 0x20U;
constexpr std::uint8_t kChipRevisionRegister = 0x21U;
constexpr std::uint8_t kDeviceSelectRegister = 0x07U;
constexpr std::uint8_t kBaseAddressRegister = 0x60U;
constexpr std::uint8_t kHardwareMonitorLogicalDevice = 0x0BU;
constexpr std::uint8_t kExpectedChipId = 0xD5U;
constexpr std::uint8_t kExpectedRevision = 0x92U;
constexpr std::uint16_t kConfigurationPorts[2]{0x2EU, 0x4EU};
constexpr std::uint16_t kPageOffset = 0x04U;
constexpr std::uint16_t kIndexOffset = 0x05U;
constexpr std::uint16_t kDataOffset = 0x06U;
constexpr std::uint8_t kPageSelect = 0xFFU;
constexpr auto kRefreshInterval = std::chrono::seconds{1};

bool ContainsInsensitive(const std::wstring_view text, const std::wstring_view search) noexcept {
    if (search.empty() || search.size() > text.size()) return false;
    for (std::size_t index = 0U; index + search.size() <= text.size(); ++index) {
        if (_wcsnicmp(text.data() + index, search.data(), search.size()) == 0) return true;
    }
    return false;
}

std::wstring ReadBiosString(const wchar_t* const name) {
    DWORD type{};
    DWORD bytes{};
    constexpr auto path = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path, name, RRF_RT_REG_SZ, &type, nullptr, &bytes) != ERROR_SUCCESS
        || bytes < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path, name, RRF_RT_REG_SZ, &type, value.data(), &bytes) != ERROR_SUCCESS) return {};
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

void AppendSensor(
    std::array<SensorValue, 32U>& sensors,
    std::size_t& count,
    const std::uint64_t id,
    const SensorKind kind,
    const SensorUnit unit,
    const wchar_t* const name,
    const wchar_t* const hardware,
    const double current) noexcept {
    if (count >= sensors.size()) return;
    auto& sensor = sensors[count++];
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

bool IsNct6687DrBoardIdentity(const std::wstring_view manufacturer, const std::wstring_view product) noexcept {
    const auto is_msi = ContainsInsensitive(manufacturer, L"Micro-Star") || ContainsInsensitive(manufacturer, L"MSI");
    return is_msi && (ContainsInsensitive(product, L"B840")
        || ContainsInsensitive(product, L"B850")
        || ContainsInsensitive(product, L"X870")
        || ContainsInsensitive(product, L"Z890"));
}

Nct6687Provider::~Nct6687Provider() {
    Close();
}

bool Nct6687Provider::ExecuteOne(const char* const function, const std::uint64_t input, std::uint64_t& output) noexcept {
    const std::array<std::uint64_t, 1U> inputs{input};
    std::array<std::uint64_t, 1U> outputs{};
    std::size_t returned{};
    if (!pawn_io_.Execute(function, inputs, outputs, returned) || returned != 1U) return false;
    output = outputs[0];
    return true;
}

bool Nct6687Provider::ExecuteNone(const char* const function) noexcept {
    std::size_t returned{};
    return pawn_io_.Execute(function, std::span<const std::uint64_t>{}, std::span<std::uint64_t>{}, returned);
}

bool Nct6687Provider::SelectSlot(const std::uint64_t slot) noexcept {
    const std::array<std::uint64_t, 1U> input{slot};
    std::size_t returned{};
    return pawn_io_.Execute("ioctl_select_slot", input, std::span<std::uint64_t>{}, returned);
}

bool Nct6687Provider::ReadConfigByte(const std::uint8_t address, std::uint8_t& value) noexcept {
    std::uint64_t output{};
    if (!ExecuteOne("ioctl_superio_inb", address, output)) return false;
    value = static_cast<std::uint8_t>(output);
    return true;
}

bool Nct6687Provider::ReadConfigWord(const std::uint8_t address, std::uint16_t& value) noexcept {
    std::uint64_t output{};
    if (!ExecuteOne("ioctl_superio_inw", address, output)) return false;
    value = static_cast<std::uint16_t>(output);
    return true;
}

bool Nct6687Provider::WriteConfigByte(const std::uint8_t address, const std::uint8_t value) noexcept {
    const std::array<std::uint64_t, 2U> input{address, value};
    std::size_t returned{};
    return pawn_io_.Execute("ioctl_superio_outb", input, std::span<std::uint64_t>{}, returned);
}

bool Nct6687Provider::ReadPort(const std::uint16_t port, std::uint8_t& value) noexcept {
    std::uint64_t output{};
    if (!ExecuteOne("ioctl_pio_inb", port, output)) return false;
    value = static_cast<std::uint8_t>(output);
    return true;
}

bool Nct6687Provider::WritePort(const std::uint16_t port, const std::uint8_t value) noexcept {
    const std::array<std::uint64_t, 2U> input{port, value};
    std::size_t returned{};
    return pawn_io_.Execute("ioctl_pio_outb", input, std::span<std::uint64_t>{}, returned);
}

bool Nct6687Provider::Initialize(const HINSTANCE resources) noexcept {
    Close();
    detected_chip_ids_ = {};
    detected_revisions_ = {};
    isa_mutex_ = CreateMutexW(nullptr, FALSE, L"Global\\Access_ISABUS.HTP.Method");
    if (isa_mutex_ == nullptr && GetLastError() == ERROR_ACCESS_DENIED) {
        isa_mutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_ISABUS.HTP.Method");
    }
    if (!pawn_io_.LoadModuleFromResource(resources, kLpcIoResource) || !AcquireMutex(100U)) {
        Close();
        return false;
    }

    for (std::uint64_t slot = 0U; slot < 2U && !available_; ++slot) {
        const auto config_port = kConfigurationPorts[static_cast<std::size_t>(slot)];
        if (!SelectSlot(slot)
            || !WritePort(config_port, 0x87U)
            || !WritePort(config_port, 0x87U)) {
            continue;
        }
        std::uint8_t chip_id{};
        std::uint8_t revision{};
        const auto chip_read = ReadConfigByte(kChipIdRegister, chip_id);
        const auto revision_read = ReadConfigByte(kChipRevisionRegister, revision);
        detected_chip_ids_[static_cast<std::size_t>(slot)] = chip_id;
        detected_revisions_[static_cast<std::size_t>(slot)] = revision;
        const auto is_nct6687 = chip_id == kExpectedChipId && revision == kExpectedRevision;
        const auto is_nct6799 = chip_id == 0xD8U && revision == 0x02U;
        if (!chip_read || !revision_read || (!is_nct6687 && !is_nct6799)) {
            static_cast<void>(WritePort(config_port, 0xAAU));
            continue;
        }
        if (!ExecuteNone("ioctl_find_bars")
            || !WriteConfigByte(kDeviceSelectRegister, kHardwareMonitorLogicalDevice)) {
            static_cast<void>(WritePort(config_port, 0xAAU));
            continue;
        }
        std::uint16_t address{};
        std::uint16_t verify{};
        const auto address_read = ReadConfigWord(kBaseAddressRegister, address);
        Sleep(1U);
        const auto verify_read = ReadConfigWord(kBaseAddressRegister, verify);
        static_cast<void>(WritePort(config_port, 0xAAU));
        if (!address_read || !verify_read || address != verify || address < 0x100U || (address & 0xF007U) != 0U) continue;
        base_port_ = address;
        ec_register_space_ = is_nct6687;
        nct6687_dr_layout_ = is_nct6687 && IsNct6687DrBoardIdentity(
            ReadBiosString(L"BaseBoardManufacturer"), ReadBiosString(L"BaseBoardProduct"));
        chip_name_ = is_nct6687
            ? (nct6687_dr_layout_ ? L"Nuvoton NCT6687D-R" : L"Nuvoton NCT6687D")
            : L"Nuvoton NCT6799D";
        available_ = true;
    }
    ReleaseMutex();
    if (!available_) {
        Close();
        return false;
    }
    Refresh();
    return true;
}

bool Nct6687Provider::AcquireMutex(const DWORD timeout_ms) noexcept {
    if (isa_mutex_ == nullptr) { mutex_owned_ = false; return false; }
    const auto result = WaitForSingleObject(isa_mutex_, timeout_ms);
    mutex_owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    return mutex_owned_;
}

void Nct6687Provider::ReleaseMutex() noexcept {
    if (isa_mutex_ != nullptr && mutex_owned_) static_cast<void>(::ReleaseMutex(isa_mutex_));
    mutex_owned_ = false;
}

bool Nct6687Provider::ReadEcByte(const std::uint16_t address, std::uint8_t& value) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{50};
    std::uint8_t access{};
    do {
        if (!ReadPort(static_cast<std::uint16_t>(base_port_ + kPageOffset), access)) return false;
        if (access == kPageSelect) break;
        Sleep(1U);
    } while (std::chrono::steady_clock::now() < deadline);
    if (access != kPageSelect && !WritePort(static_cast<std::uint16_t>(base_port_ + kPageOffset), kPageSelect)) return false;
    const auto page = static_cast<std::uint8_t>(address >> 8U);
    const auto index = static_cast<std::uint8_t>(address & 0xFFU);
    const auto success = WritePort(static_cast<std::uint16_t>(base_port_ + kPageOffset), page)
        && WritePort(static_cast<std::uint16_t>(base_port_ + kIndexOffset), index)
        && ReadPort(static_cast<std::uint16_t>(base_port_ + kDataOffset), value);
    static_cast<void>(WritePort(static_cast<std::uint16_t>(base_port_ + kPageOffset), kPageSelect));
    return success;
}

bool Nct6687Provider::ReadBankedByte(const std::uint16_t address, std::uint8_t& value) noexcept {
    const auto bank = static_cast<std::uint8_t>(address >> 8U);
    const auto index = static_cast<std::uint8_t>(address & 0xFFU);
    return WritePort(static_cast<std::uint16_t>(base_port_ + 0x05U), 0x4EU)
        && WritePort(static_cast<std::uint16_t>(base_port_ + 0x06U), bank)
        && WritePort(static_cast<std::uint16_t>(base_port_ + 0x05U), index)
        && ReadPort(static_cast<std::uint16_t>(base_port_ + 0x06U), value);
}

void Nct6687Provider::Refresh() noexcept {
    // A failed refresh must not republish the preceding measurement as live.
    cached_sensor_count_ = 0U;
    last_refresh_ = std::chrono::steady_clock::now();
    if (!available_ || !AcquireMutex(50U)) return;
    if (!ec_register_space_) {
        constexpr std::array<std::pair<std::uint32_t, const wchar_t*>, 2U> temperatures{{
            {0x075U, L"CPU Socket temperature"},
            {0x077U, L"Motherboard temperature"}}};
        for (std::size_t index = 0U; index < temperatures.size(); ++index) {
            std::uint8_t whole{};
            std::uint8_t fraction{};
            if (!ReadBankedByte(static_cast<std::uint16_t>(temperatures[index].first), whole)
                || !ReadBankedByte(static_cast<std::uint16_t>(temperatures[index].first + 1U), fraction)) {
                continue;
            }
            const auto signed_whole = static_cast<std::int8_t>(whole);
            const auto celsius = static_cast<double>(signed_whole) + (((fraction >> 7U) & 1U) != 0U ? 0.5 : 0.0);
            if (celsius <= 0.0 || celsius > 125.0) continue;
            AppendSensor(cached_sensors_, cached_sensor_count_, 0x0500'0000'0000'0000ULL | index, SensorKind::temperature, SensorUnit::celsius, temperatures[index].second, chip_name_.c_str(), celsius);
        }
        constexpr std::array<std::uint16_t, 7U> fan_registers{0x4B0U, 0x4B2U, 0x4B4U, 0x4B6U, 0x4B8U, 0x4BAU, 0x4CCU};
        for (std::size_t index = 0U; index < fan_registers.size(); ++index) {
            std::uint8_t high{};
            std::uint8_t low{};
            if (!ReadBankedByte(fan_registers[index], high)
                || !ReadBankedByte(static_cast<std::uint16_t>(fan_registers[index] + 1U), low)) {
                continue;
            }
            const auto count = (static_cast<std::uint32_t>(high) << 5U) | (low & 0x1FU);
            if (count < 0x15U || count >= 0x1FFFU) continue;
            const auto rpm = 1'350'000.0 / static_cast<double>(count);
            if (rpm <= 0.0 || rpm > 20'000.0) continue;
            std::array<wchar_t, 32U> name{};
            static_cast<void>(swprintf_s(name.data(), name.size(), L"Motherboard Fan #%zu", index + 1U));
            AppendSensor(cached_sensors_, cached_sensor_count_, 0x0500'0000'0000'0100ULL | index, SensorKind::fan, SensorUnit::revolutions_per_minute, name.data(), chip_name_.c_str(), rpm);
        }
        ReleaseMutex();
        last_refresh_ = std::chrono::steady_clock::now();
        return;
    }
    constexpr std::array<std::pair<std::uint32_t, const wchar_t*>, 11U> temperatures{{
        {0x100U, L"CPU Core temperature"},
        {0x102U, L"System temperature"},
        {0x104U, L"VRM MOS temperature"},
        {0x106U, L"Chipset temperature"},
        {0x108U, L"CPU Socket temperature"},
        {0x10AU, L"PCIe #1 temperature"},
        {0x10CU, L"M.2 #1 temperature"},
        {0x10EU, L"PCIe #1 auxiliary temperature"},
        {0x110U, L"PCIe #2 temperature"},
        {0x112U, L"M.2 #1 auxiliary temperature"},
        {0x114U, L"M.2 #4 temperature"}}};
    const auto temperature_count = nct6687_dr_layout_ ? 7U : temperatures.size();
    for (std::size_t index = 0U; index < temperature_count; ++index) {
        std::uint8_t whole{};
        std::uint8_t fraction{};
        if (!ReadEcByte(static_cast<std::uint16_t>(temperatures[index].first), whole)
            || !ReadEcByte(static_cast<std::uint16_t>(temperatures[index].first + 1U), fraction)) {
            continue;
        }
        const auto signed_whole = static_cast<std::int8_t>(whole);
        const auto celsius = static_cast<double>(signed_whole) + (((fraction >> 7U) & 1U) != 0U ? 0.5 : 0.0);
        if (celsius <= 0.0 || celsius > 125.0) continue;
        AppendSensor(cached_sensors_, cached_sensor_count_, 0x0500'0000'0000'0000ULL | index, SensorKind::temperature, SensorUnit::celsius, temperatures[index].second, chip_name_.c_str(), celsius);
    }

    struct FanDescriptor final { std::uint16_t address; std::uint16_t id; const wchar_t* name; };
    constexpr std::array<FanDescriptor, 11U> dr_fans{{
        {0x140U, 0U, L"CPU Fan"},
        {0x142U, 1U, L"Pump Fan #1"},
        {0x144U, 2U, L"Chipset Fan"},
        {0x146U, 3U, L"EZ-Connect Fan"},
        {0x15EU, 4U, L"System Fan #1"},
        {0x15CU, 5U, L"System Fan #2"},
        {0x15AU, 6U, L"System Fan #3"},
        {0x158U, 7U, L"System Fan #4"},
        {0x156U, 8U, L"System Fan #5"},
        {0x154U, 9U, L"System Fan #6"},
        {0x152U, 10U, L"System Fan #7"}}};
    constexpr std::array<std::uint16_t, 17U> generic_fan_registers{
        0x140U, 0x142U, 0x144U, 0x146U, 0x148U, 0x14AU, 0x14CU, 0x14EU, 0x150U,
        0x152U, 0x154U, 0x156U, 0x158U, 0x15AU, 0x15CU, 0x15EU, 0x852U};
    const auto append_fan = [&](const std::uint16_t address, const std::uint16_t id, const wchar_t* const name, const bool count_encoded) {
        std::uint8_t high{};
        std::uint8_t low{};
        if (!ReadEcByte(address, high) || !ReadEcByte(static_cast<std::uint16_t>(address + 1U), low)) return;
        double rpm{};
        if (count_encoded) {
            const auto count = (static_cast<std::uint32_t>(high) << 5U) | (low & 0x1FU);
            if (count < 0x15U || count >= 0x1FFFU) return;
            rpm = 1'350'000.0 / static_cast<double>(count);
        } else {
            rpm = static_cast<double>((static_cast<std::uint16_t>(high) << 8U) | low);
        }
        if (rpm <= 0.0 || rpm > 20'000.0) return;
        AppendSensor(cached_sensors_, cached_sensor_count_, 0x0500'0000'0000'0100ULL | id, SensorKind::fan, SensorUnit::revolutions_per_minute, name, chip_name_.c_str(), rpm);
    };
    if (nct6687_dr_layout_) {
        for (const auto& fan : dr_fans) append_fan(fan.address, fan.id, fan.name, false);
    } else {
        for (std::size_t index = 0U; index < generic_fan_registers.size(); ++index) {
            std::array<wchar_t, 32U> name{};
            static_cast<void>(swprintf_s(name.data(), name.size(), L"Motherboard Fan #%zu", index + 1U));
            append_fan(generic_fan_registers[index], static_cast<std::uint16_t>(index), name.data(), index + 1U == generic_fan_registers.size());
        }
    }
    ReleaseMutex();
    last_refresh_ = std::chrono::steady_clock::now();
}

void Nct6687Provider::Collect(SensorSnapshot& snapshot) noexcept {
    if (!available_) return;
    const auto now = std::chrono::steady_clock::now();
    if (last_refresh_ == std::chrono::steady_clock::time_point{} || now - last_refresh_ >= kRefreshInterval) Refresh();
    for (std::size_t index = 0U; index < cached_sensor_count_ && snapshot.count < snapshot.sensors.size(); ++index) {
        snapshot.sensors[snapshot.count++] = cached_sensors_[index];
    }
}

void Nct6687Provider::Close() noexcept {
    ReleaseMutex();
    pawn_io_.Close();
    if (isa_mutex_ != nullptr) CloseHandle(isa_mutex_);
    isa_mutex_ = nullptr;
    base_port_ = 0U;
    chip_name_.clear();
    cached_sensors_ = {};
    cached_sensor_count_ = 0U;
    last_refresh_ = {};
    available_ = false;
    ec_register_space_ = false;
    nct6687_dr_layout_ = false;
}

} // namespace hardwarescope
