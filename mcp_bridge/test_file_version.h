#pragma once

#include <nlohmann/json.hpp>
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace cortex::test::file_version {
using json = nlohmann::json;

// Fingerprint a module actually present in the OS module snapshot. Hash the
// entire on-disk image (not only its metadata), with a strict size limit.
// This version check is NOT a verification of in-memory patches or behavior.
inline json Hash(const std::string& utf8Path) {
    const auto unavailable = [](const char* reason) -> json {
        return {{"status", "unavailable"}, {"reason", reason}};
    };
    if (utf8Path.empty() || utf8Path.size() > 2048 ||
        utf8Path.size() >= 2 && (utf8Path[0] == '\\' || utf8Path[0] == '/'))
        return unavailable("invalid_module_path");
    try {
        const std::filesystem::path path = std::filesystem::u8path(utf8Path);
        std::error_code error;
        if (!path.is_absolute() || !std::filesystem::is_regular_file(path, error) || error)
            return unavailable("module_file_unavailable");
        const uint64_t length = std::filesystem::file_size(path, error);
        constexpr uint64_t kMaxFileBytes = 256ULL * 1024 * 1024;
        if (error || length == 0 || length > kMaxFileBytes)
            return unavailable("module_file_outside_hash_limit");
        const auto modified = std::filesystem::last_write_time(path, error);
        if (error) return unavailable("module_file_unavailable");
        std::ifstream file(path, std::ios::binary);
        if (!file) return unavailable("module_file_unreadable");

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE handle = nullptr;
        const auto cleanup = [&]() {
            if (handle) BCryptDestroyHash(handle);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        };
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            return unavailable("sha256_unavailable");
        ULONG objectSize = 0, resultSize = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                &resultSize, 0) < 0 ||
            objectSize == 0 || objectSize > 65536) {
            cleanup();
            return unavailable("sha256_unavailable");
        }
        std::vector<UCHAR> object(objectSize);
        if (BCryptCreateHash(algorithm, &handle, object.data(),
                objectSize, nullptr, 0, 0) < 0) {
            cleanup();
            return unavailable("sha256_unavailable");
        }
        std::array<char, 64 * 1024> block{};
        uint64_t left = length;
        bool ok = true;
        while (left > 0) {
            const size_t count = static_cast<size_t>((std::min)(
                left, static_cast<uint64_t>(block.size())));
            file.read(block.data(), static_cast<std::streamsize>(count));
            if (file.gcount() != static_cast<std::streamsize>(count) ||
                BCryptHashData(handle,
                    reinterpret_cast<PUCHAR>(block.data()),
                    static_cast<ULONG>(count), 0) < 0) {
                ok = false; break;
            }
            left -= count;
        }
        std::array<UCHAR, 32> digest{};
        if (ok && BCryptFinishHash(handle, digest.data(),
                static_cast<ULONG>(digest.size()), 0) < 0)
            ok = false;
        cleanup();
        if (!ok || file.peek() != std::char_traits<char>::eof())
            return unavailable("module_file_changed_while_hashing");
        if (std::filesystem::file_size(path, error) != length || error ||
            std::filesystem::last_write_time(path, error) != modified || error)
            return unavailable("module_file_changed_while_hashing");
        static constexpr char hex[] = "0123456789abcdef";
        std::string sha;
        sha.reserve(64);
        for (UCHAR b : digest) {
            sha.push_back(hex[b >> 4]);
            sha.push_back(hex[b & 0x0f]);
        }
        return {{"status", "hashed"}, {"sha256", std::move(sha)},
                {"file_size", length}};
    } catch (const std::exception&) {
        return unavailable("module_file_unavailable");
    }
}
} // namespace cortex::test::file_version
