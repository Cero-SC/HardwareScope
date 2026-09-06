#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <windows.h>
#include <vector>

namespace hardwarescope {

class VerifiedFile final {
public:
    VerifiedFile() = default;
    ~VerifiedFile() { Close(); }
    VerifiedFile(const VerifiedFile&) = delete;
    VerifiedFile& operator=(const VerifiedFile&) = delete;
    [[nodiscard]] bool Open(const std::filesystem::path& path, std::uint64_t expected_size,
        std::string_view expected_sha256) noexcept;
    void Close() noexcept {
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        for (const auto directory : directories_) CloseHandle(directory);
        directories_.clear();
    }
private:
    HANDLE file_{INVALID_HANDLE_VALUE};
    std::vector<HANDLE> directories_;
};

[[nodiscard]] bool VerifyFileSha256(const std::filesystem::path& path, std::uint64_t expected_size, std::string_view expected_sha256) noexcept;

} // namespace hardwarescope
