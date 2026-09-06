// AMD temperature formulas and register locations are derived from
// LibreHardwareMonitor v0.9.6 (MPL-2.0) and Linux k10temp documentation.

#include "hardwarescope/amd_zen_provider.hpp"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace hardwarescope {
namespace {

constexpr int kAmdFamily17Resource = 201;
constexpr std::uint32_t kTemperatureControl = 0x00059800U;
constexpr std::uint32_t kZen2Ccd1Temperature = 0x00059954U;
constexpr std::uint32_t kZen45Ccd1Temperature = 0x00059B08U;
constexpr std::uint32_t kTemperatureOffsetFlag = 0x00080000U;

std::string CpuVendor() noexcept {
    std::array<int, 4> registers{};
    __cpuid(registers.data(), 0);
    std::array<char, 13> vendor{};
    std::memcpy(vendor.data(), &registers[1], 4U);
    std::memcpy(vendor.data() + 4U, &registers[3], 4U);
    std::memcpy(vendor.data() + 8U, &registers[2], 4U);
    return vendor.data();
}

std::wstring CpuName() noexcept {
    std::array<int, 4> registers{};
    __cpuid(registers.data(), static_cast<int>(0x80000000U));
    if (static_cast<std::uint32_t>(registers[0]) < 0x80000004U) return L"AMD Ryzen CPU";
    std::array<char, 49> name{};
    for (std::uint32_t leaf = 0; leaf < 3U; ++leaf) {
        __cpuid(registers.data(), static_cast<int>(0x80000002U + leaf));
        std::memcpy(name.data() + leaf * 16U, registers.data(), 16U);
    }
    std::string_view text{name.data()};
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1U);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1U);
    return std::wstring{text.begin(), text.end()};
}

} // namespace

AmdZenProvider::~AmdZenProvider() {
    Close();
}

bool AmdZenProvider::Initialize(const HINSTANCE resources) noexcept {
    Close();
    if (CpuVendor() != "AuthenticAMD") return false;
    std::array<int, 4> registers{};
    __cpuid(registers.data(), 1);
    const auto signature = static_cast<std::uint32_t>(registers[0]);
    const auto base_family = (signature >> 8U) & 0xFU;
    const auto base_model = (signature >> 4U) & 0xFU;
    const auto extended_family = (signature >> 20U) & 0xFFU;
    const auto extended_model = (signature >> 16U) & 0xFU;
    family_ = base_family == 0xFU ? base_family + extended_family : base_family;
    model_ = family_ >= 0xFU ? base_model | (extended_model << 4U) : base_model;
    processor_name_ = CpuName();
    if (family_ < 0x17U) return false;

    pci_mutex_ = CreateMutexW(nullptr, FALSE, L"Global\\Access_PCI");
    if (pci_mutex_ == nullptr && GetLastError() == ERROR_ACCESS_DENIED) {
        pci_mutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_PCI");
    }
    if (pci_mutex_ == nullptr || !pawn_io_.LoadModuleFromResource(resources, kAmdFamily17Resource)) {
        Close();
        return false;
    }
    available_ = true;
    return true;
}

bool AmdZenProvider::AcquirePciMutex(const DWORD timeout_ms) noexcept {
    if (pci_mutex_ == nullptr) { mutex_owned_ = false; return false; }
    const auto result = WaitForSingleObject(pci_mutex_, timeout_ms);
    mutex_owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    return mutex_owned_;
}

void AmdZenProvider::ReleasePciMutex() noexcept {
    if (pci_mutex_ != nullptr && mutex_owned_) static_cast<void>(ReleaseMutex(pci_mutex_));
    mutex_owned_ = false;
}

bool AmdZenProvider::ReadSmn(const std::uint32_t offset, std::uint32_t& value) noexcept {
    const std::array<std::uint64_t, 1> input{offset};
    std::array<std::uint64_t, 1> output{};
    std::size_t returned{};
    if (!pawn_io_.Execute("ioctl_read_smn", input, output, returned) || returned != 1U) return false;
    value = static_cast<std::uint32_t>(output[0]);
    return true;
}

bool AmdZenProvider::ReadTemperatures(AmdZenTemperatures& temperatures) noexcept {
    temperatures = {};
    if (!available_ || !AcquirePciMutex(50U)) return false;
    std::uint32_t raw{};
    const bool package_read = ReadSmn(kTemperatureControl, raw);
    if (package_read) {
        auto package = static_cast<double>(raw >> 21U) * 0.125;
        if ((raw & kTemperatureOffsetFlag) != 0U) package -= 49.0;
        if (std::isfinite(package) && package > 0.0 && package < 125.0) temperatures.package_celsius = package;
    }

    if (model_ == 0x61U || model_ == 0x44U || model_ == 0x71U || model_ == 0x21U || model_ == 0x31U) {
        const auto base = model_ == 0x61U || model_ == 0x44U ? kZen45Ccd1Temperature : kZen2Ccd1Temperature;
        for (std::uint32_t index = 0; index < temperatures.ccd_celsius.size(); ++index) {
            std::uint32_t ccd_raw{};
            if (!ReadSmn(base + index * 4U, ccd_raw)) continue;
            ccd_raw &= 0xFFFU;
            const auto ccd = (static_cast<double>(ccd_raw) * 125.0 - 305'000.0) * 0.001;
            if (ccd_raw > 0U && std::isfinite(ccd) && ccd > 0.0 && ccd < 125.0) {
                temperatures.ccd_celsius[index] = ccd;
                ++temperatures.ccd_count;
            }
        }
    }
    ReleasePciMutex();
    return package_read && temperatures.package_celsius > 0.0;
}

void AmdZenProvider::Close() noexcept {
    ReleasePciMutex();
    available_ = false;
    pawn_io_.Close();
    if (pci_mutex_ != nullptr) CloseHandle(pci_mutex_);
    pci_mutex_ = nullptr;
}

} // namespace hardwarescope
