#include "hardwarescope/file_verification.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <vector>
#include <algorithm>

namespace hardwarescope {
namespace {

struct Algorithm final {
    BCRYPT_ALG_HANDLE value{};
    ~Algorithm() { if (value != nullptr) BCryptCloseAlgorithmProvider(value, 0U); }
};

struct Hash final {
    BCRYPT_HASH_HANDLE value{};
    ~Hash() { if (value != nullptr) BCryptDestroyHash(value); }
};

} // namespace

bool VerifiedFile::Open(const std::filesystem::path& path, const std::uint64_t expected_size, const std::string_view expected_sha256) noexcept {
    Close();
    if (expected_sha256.size() != 64U) return false;
    try {
        // Lock parent names from the volume root down, so moving/replacing the
        // staging directory cannot redirect ShellExecute after verification.
        const auto absolute = std::filesystem::absolute(path).lexically_normal();
        std::vector<std::filesystem::path> parents;
        for (auto parent = absolute.parent_path(); !parent.empty(); parent = parent.parent_path()) {
            parents.push_back(parent);
            if (parent == parent.root_path()) break;
        }
        struct Directories final {
            std::vector<HANDLE> handles;
            ~Directories() { for (const auto handle : handles) CloseHandle(handle); }
        } directories;
        directories.handles.reserve(parents.size());
        for (auto parent = parents.rbegin(); parent != parents.rend(); ++parent) {
            const auto directory = CreateFileW(parent->c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (directory == INVALID_HANDLE_VALUE) return false;
            directories.handles.push_back(directory);
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(directory, &info)
                || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) return false;
        }
        const auto handle = CreateFileW(absolute.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        struct FileCloser final { HANDLE value; ~FileCloser() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } file{handle};
        BY_HANDLE_FILE_INFORMATION information{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(handle, &information)
            || (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U
            || GetFileType(handle) != FILE_TYPE_DISK
            || !GetFileSizeEx(handle, &size) || size.QuadPart < 0
            || static_cast<std::uint64_t>(size.QuadPart) != expected_size) return false;

        Algorithm algorithm{};
        if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0U) < 0) return false;
        DWORD object_size{};
        DWORD copied{};
        if (BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0U) < 0) return false;
        std::vector<UCHAR> object(object_size);
        Hash hash{};
        if (BCryptCreateHash(algorithm.value, &hash.value, object.data(), static_cast<ULONG>(object.size()), nullptr, 0U, 0U) < 0) return false;
        std::array<UCHAR, 64U * 1024U> buffer{};
        for (;;) {
            DWORD read{};
            if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return false;
            if (read > 0U && BCryptHashData(hash.value, buffer.data(), static_cast<ULONG>(read), 0U) < 0) return false;
            if (read < buffer.size()) {
                break;
            }
        }
        std::array<UCHAR, 32U> digest{};
        if (BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0U) < 0) return false;
        constexpr char hex[] = "0123456789ABCDEF";
        std::array<char, 64U> actual{};
        for (std::size_t index = 0U; index < digest.size(); ++index) {
            actual[index * 2U] = hex[digest[index] >> 4U];
            actual[index * 2U + 1U] = hex[digest[index] & 0x0FU];
        }
        for (std::size_t index = 0U; index < actual.size(); ++index) {
            if (actual[index] != static_cast<char>(std::toupper(static_cast<unsigned char>(expected_sha256[index])))) return false;
        }
        file_ = handle;
        file.value = INVALID_HANDLE_VALUE;
        directories_ = std::move(directories.handles);
        return true;
    } catch (...) {
        return false;
    }
}

bool VerifyFileSha256(const std::filesystem::path& path, const std::uint64_t expected_size, const std::string_view expected_sha256) noexcept {
    VerifiedFile file;
    return file.Open(path, expected_size, expected_sha256);
}

} // namespace hardwarescope
