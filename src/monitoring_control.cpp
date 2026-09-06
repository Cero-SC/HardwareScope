#include "hardwarescope/monitoring_control.hpp"

#include <array>

namespace hardwarescope {

bool ReadMonitoringControl(HANDLE& pending_pipe, const wchar_t* const pipe_name,
    const std::wstring_view expected_server, void* const message, const DWORD message_size) noexcept {
    struct Close final { HANDLE value; ~Close() { if (value != nullptr) CloseHandle(value); } };
    if (pending_pipe == nullptr) {
        const auto pipe = CreateFileW(pipe_name, GENERIC_READ | FILE_WRITE_ATTRIBUTES, 0U, nullptr,
            OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return false;
        Close close_pipe{pipe};
        DWORD server_id{};
        if (!GetNamedPipeServerProcessId(pipe, &server_id)) return false;
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server_id);
        if (process == nullptr) return false;
        Close close_process{process};
        std::array<wchar_t, 32'768U> path{};
        auto length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process, 0U, path.data(), &length)
            || length != expected_server.size()
            || _wcsnicmp(path.data(), expected_server.data(), length) != 0) return false;

        DWORD mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) return false;
        pending_pipe = pipe;
        close_pipe.value = nullptr;
    }
    DWORD available{};
    if (!PeekNamedPipe(pending_pipe, nullptr, 0U, nullptr, nullptr, &available)) {
        CloseHandle(pending_pipe);
        pending_pipe = nullptr;
        return false;
    }
    if (available == 0U) return false;
    Close close_pipe{pending_pipe};
    pending_pipe = nullptr;
    if (available != message_size) return false;
    DWORD read{};
    return ReadFile(close_pipe.value, message, message_size, &read, nullptr) && read == message_size;
}

} // namespace hardwarescope
