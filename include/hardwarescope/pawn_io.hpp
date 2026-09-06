#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace hardwarescope {

class PawnIoExecutor final {
public:
    PawnIoExecutor() = default;
    ~PawnIoExecutor();

    PawnIoExecutor(const PawnIoExecutor&) = delete;
    PawnIoExecutor& operator=(const PawnIoExecutor&) = delete;

    [[nodiscard]] bool LoadModuleFromResource(HINSTANCE resources, int resource_id) noexcept;
    [[nodiscard]] bool CheckRuntime() noexcept;
    [[nodiscard]] bool Execute(
        const char* function,
        std::span<const std::uint64_t> input,
        std::span<std::uint64_t> output,
        std::size_t& returned) noexcept;
    void Close() noexcept;

    [[nodiscard]] bool Loaded() const noexcept { return executor_ != nullptr; }
    [[nodiscard]] std::uint32_t LibraryVersion() const noexcept { return version_; }
    [[nodiscard]] HRESULT LastError() const noexcept { return last_error_; }

private:
    using VersionFunction = HRESULT(STDAPICALLTYPE*)(PULONG);
    using OpenFunction = HRESULT(STDAPICALLTYPE*)(PHANDLE);
    using LoadFunction = HRESULT(STDAPICALLTYPE*)(HANDLE, const UCHAR*, SIZE_T);
    using ExecuteFunction = HRESULT(STDAPICALLTYPE*)(HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
    using CloseFunction = HRESULT(STDAPICALLTYPE*)(HANDLE);

    [[nodiscard]] bool LoadLibraryFunctions() noexcept;

    HMODULE library_{};
    HANDLE executor_{};
    VersionFunction version_function_{};
    OpenFunction open_function_{};
    LoadFunction load_function_{};
    ExecuteFunction execute_function_{};
    CloseFunction close_function_{};
    std::uint32_t version_{};
    HRESULT last_error_{E_FAIL};
};

} // namespace hardwarescope
