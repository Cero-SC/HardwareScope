#include "hardwarescope/pawn_io.hpp"

#include <shlobj.h>

#include <array>
#include <filesystem>

namespace hardwarescope {
namespace {

std::filesystem::path ExecutableDirectory() noexcept {
    std::array<wchar_t, 32'768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size()) return {};
    return std::filesystem::path{path.data()}.parent_path();
}

std::filesystem::path PawnIoInstallLibrary() noexcept {
    PWSTR program_files = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &program_files))) return {};
    std::filesystem::path path{program_files};
    CoTaskMemFree(program_files);
    path /= L"PawnIO";
    path /= L"PawnIOLib.dll";
    return path;
}

HMODULE LoadExplicitLibrary(const std::filesystem::path& path) noexcept {
    if (path.empty()) return nullptr;
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

} // namespace

PawnIoExecutor::~PawnIoExecutor() {
    Close();
}

bool PawnIoExecutor::LoadLibraryFunctions() noexcept {
    if (library_ != nullptr) return true;
    library_ = LoadExplicitLibrary(ExecutableDirectory() / L"PawnIOLib.dll");
    if (library_ == nullptr) library_ = LoadExplicitLibrary(PawnIoInstallLibrary());
    if (library_ == nullptr) {
        last_error_ = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }

    version_function_ = reinterpret_cast<VersionFunction>(GetProcAddress(library_, "pawnio_version"));
    open_function_ = reinterpret_cast<OpenFunction>(GetProcAddress(library_, "pawnio_open"));
    load_function_ = reinterpret_cast<LoadFunction>(GetProcAddress(library_, "pawnio_load"));
    execute_function_ = reinterpret_cast<ExecuteFunction>(GetProcAddress(library_, "pawnio_execute"));
    close_function_ = reinterpret_cast<CloseFunction>(GetProcAddress(library_, "pawnio_close"));
    if (version_function_ == nullptr || open_function_ == nullptr || load_function_ == nullptr || execute_function_ == nullptr || close_function_ == nullptr) {
        last_error_ = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        Close();
        return false;
    }
    ULONG version{};
    last_error_ = version_function_(&version);
    if (FAILED(last_error_)) {
        Close();
        return false;
    }
    version_ = version;
    return true;
}

bool PawnIoExecutor::LoadModuleFromResource(const HINSTANCE resources, const int resource_id) noexcept {
    Close();
    if (!LoadLibraryFunctions()) return false;
    const auto resource = FindResourceW(resources, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (resource == nullptr) {
        last_error_ = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }
    const auto loaded_resource = LoadResource(resources, resource);
    const auto* bytes = static_cast<const UCHAR*>(LockResource(loaded_resource));
    const auto size = SizeofResource(resources, resource);
    if (loaded_resource == nullptr || bytes == nullptr || size == 0U) {
        last_error_ = HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);
        return false;
    }
    last_error_ = open_function_(&executor_);
    if (FAILED(last_error_) || executor_ == nullptr) {
        executor_ = nullptr;
        return false;
    }
    last_error_ = load_function_(executor_, bytes, size);
    if (FAILED(last_error_)) {
        static_cast<void>(close_function_(executor_));
        executor_ = nullptr;
        return false;
    }
    return true;
}

bool PawnIoExecutor::CheckRuntime() noexcept {
    Close();
    if (!LoadLibraryFunctions()) return false;
    last_error_ = open_function_(&executor_);
    const auto ready = SUCCEEDED(last_error_) && executor_ != nullptr;
    Close();
    return ready;
}

bool PawnIoExecutor::Execute(
    const char* const function,
    const std::span<const std::uint64_t> input,
    const std::span<std::uint64_t> output,
    std::size_t& returned) noexcept {
    returned = 0U;
    if (executor_ == nullptr || execute_function_ == nullptr || function == nullptr) {
        last_error_ = E_HANDLE;
        return false;
    }
    last_error_ = execute_function_(
        executor_,
        function,
        input.empty() ? nullptr : input.data(),
        input.size(),
        output.empty() ? nullptr : output.data(),
        output.size(),
        &returned);
    return SUCCEEDED(last_error_);
}

void PawnIoExecutor::Close() noexcept {
    if (executor_ != nullptr && close_function_ != nullptr) static_cast<void>(close_function_(executor_));
    executor_ = nullptr;
    version_ = 0U;
    version_function_ = nullptr;
    open_function_ = nullptr;
    load_function_ = nullptr;
    execute_function_ = nullptr;
    close_function_ = nullptr;
    if (library_ != nullptr) FreeLibrary(library_);
    library_ = nullptr;
}

} // namespace hardwarescope
