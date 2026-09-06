#include "hardwarescope/file_verification.hpp"

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace {

std::optional<std::uint64_t> Number(const std::wstring_view text) noexcept {
    wchar_t* end{};
    const auto value = _wcstoui64(text.data(), &end, 10);
    if (end == text.data() + text.size()) return value;
    return std::nullopt;
}

bool Relaunch(const std::filesystem::path& application) noexcept {
    const auto directory = application.parent_path();
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"open";
    launch.lpFile = application.c_str();
    launch.lpDirectory = directory.c_str();
    launch.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&launch) != FALSE;
}

struct RelaunchOnFailure final {
    const std::filesystem::path& application;
    bool dismissed{};

    ~RelaunchOnFailure() {
        if (!dismissed) static_cast<void>(Relaunch(application));
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argument_count{};
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr) return 10;
    struct Arguments final { LPWSTR* value{}; ~Arguments() { if (value != nullptr) LocalFree(value); } } owned{arguments};
    if (argument_count != 11
        || std::wstring_view{arguments[1]} != L"--apply"
        || std::wstring_view{arguments[3]} != L"--size"
        || std::wstring_view{arguments[5]} != L"--sha256"
        || std::wstring_view{arguments[7]} != L"--wait-pid"
        || std::wstring_view{arguments[9]} != L"--relaunch") return 11;

    const std::filesystem::path installer{arguments[2]};
    const auto size = Number(arguments[4]);
    const std::wstring_view wide_hash{arguments[6]};
    const auto process_id = Number(arguments[8]);
    const std::filesystem::path application{arguments[10]};
    if (!size || !process_id || wide_hash.size() != 64U || *process_id > UINT32_MAX) return 12;
    std::string hash;
    hash.reserve(wide_hash.size());
    for (const auto character : wide_hash) {
        if (character > 0x7FU) return 12;
        hash.push_back(static_cast<char>(character));
    }

    if (const auto process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(*process_id)); process != nullptr) {
        const auto wait = WaitForSingleObject(process, 120'000U);
        CloseHandle(process);
        if (wait != WAIT_OBJECT_0) return 13;
    }
    RelaunchOnFailure recovery{application};
    hardwarescope::VerifiedFile verified_installer;
    if (!verified_installer.Open(installer, *size, hash)) return 14;

    const auto installer_directory = installer.parent_path();
    SHELLEXECUTEINFOW setup{};
    setup.cbSize = sizeof(setup);
    setup.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    setup.lpVerb = L"runas";
    setup.lpFile = installer.c_str();
    setup.lpParameters = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
    setup.lpDirectory = installer_directory.c_str();
    setup.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&setup) || setup.hProcess == nullptr) return 15;
    const auto setup_wait = WaitForSingleObject(setup.hProcess, INFINITE);
    DWORD setup_exit_code{ERROR_GEN_FAILURE};
    static_cast<void>(GetExitCodeProcess(setup.hProcess, &setup_exit_code));
    CloseHandle(setup.hProcess);
    if (setup_wait != WAIT_OBJECT_0 || setup_exit_code != ERROR_SUCCESS) return 16;
    verified_installer.Close();
    static_cast<void>(DeleteFileW(installer.c_str()));
    if (!Relaunch(application)) return 17;
    recovery.dismissed = true;
    return 0;
}
