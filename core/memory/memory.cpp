#include "memory.h"
#include "range.h"
#include <windows.h>
#include <cstring>

namespace memory {

bool ReadBytes(uintptr_t address, size_t size, std::vector<uint8_t>& out) {
    if (!IsValidRange(address, size)) return false;
    out.resize(size);
    SIZE_T bytesRead = 0;
    BOOL ok = ReadProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<LPCVOID>(address),
        out.data(),
        size,
        &bytesRead);
    if (!ok || bytesRead != size) {
        out.clear();
        return false;
    }
    return true;
}

bool WriteBytes(uintptr_t address, const std::vector<uint8_t>& data) {
    if (!IsValidRange(address, data.size())) return false;

    // Memory protecting the region might not be writable (e.g. .text);
    // temporarily relax it, write, then restore.
    DWORD oldProtect = 0;
    BOOL protectedOk = VirtualProtect(reinterpret_cast<LPVOID>(address), data.size(),
                                       PAGE_EXECUTE_READWRITE, &oldProtect);

    SIZE_T bytesWritten = 0;
    BOOL ok = WriteProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<LPVOID>(address),
        data.data(),
        data.size(),
        &bytesWritten);

    if (protectedOk) {
        DWORD tmp;
        VirtualProtect(reinterpret_cast<LPVOID>(address), data.size(), oldProtect, &tmp);
    }

    const bool complete = ok && bytesWritten == data.size();
    if (complete) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), data.size());
    return complete;
}

std::optional<std::string> ReadString(uintptr_t address, size_t max_len) {
    if (!IsValidRange(address, max_len)) return std::nullopt;
    std::vector<uint8_t> buf;
    if (!ReadBytes(address, max_len, buf)) return std::nullopt;
    size_t len = 0;
    while (len < buf.size() && buf[len] != 0) ++len;
    return std::string(reinterpret_cast<char*>(buf.data()), len);
}

} // namespace memory
