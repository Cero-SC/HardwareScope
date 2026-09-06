#pragma once

#include <windows.h>
#include <cstddef>
#include <string_view>

namespace hardwarescope {

// Reads one already queued message; never lends the caller's security token
// to the server and never waits for a desktop process to send data.
// pending_pipe belongs to the caller; retain across polls and close at shutdown.
[[nodiscard]] bool ReadMonitoringControl(HANDLE& pending_pipe, const wchar_t* pipe_name,
    std::wstring_view expected_server, void* message, DWORD message_size) noexcept;

} // namespace hardwarescope
