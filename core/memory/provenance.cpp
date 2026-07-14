#include "provenance.h"

#include <windows.h>
#include <algorithm>
#include <limits>
#include <map>
#include <mutex>

namespace provenance {
namespace {

std::mutex g_mutex;
std::map<uint64_t, Range> g_ranges;
uint64_t g_nextId = 1;
bool g_moduleRegistered = false;

bool Overlaps(uintptr_t a, size_t as, uintptr_t b, size_t bs) {
    if (as == 0 || bs == 0) return false;
    const uintptr_t max = (std::numeric_limits<uintptr_t>::max)();
    const uintptr_t ae = as > max - a ? max : a + as;
    const uintptr_t be = bs > max - b ? max : b + bs;
    return a < be && b < ae;
}

} // namespace

uint64_t Register(uintptr_t base, size_t size, const std::string& owner,
                  const std::string& label, bool transient) {
    if (base == 0 || size == 0) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t id = g_nextId++;
    g_ranges.emplace(id, Range{id, base, size, owner, label, transient});
    return id;
}

bool Unregister(uint64_t id) {
    if (id == 0) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_ranges.erase(id) != 0;
}

bool Resize(uint64_t id, uintptr_t base, size_t size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_ranges.find(id);
    if (it == g_ranges.end()) return false;
    it->second.base = base;
    it->second.size = size;
    return true;
}

void EnsureCoreModuleRegistered() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_moduleRegistered) return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&EnsureCoreModuleRegistered), &module) || !module) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0) return;
    const uint64_t id = g_nextId++;
    g_ranges.emplace(id, Range{id, reinterpret_cast<uintptr_t>(module),
                               nt->OptionalHeader.SizeOfImage, "cortex", "core_module", false});
    g_moduleRegistered = true;
}

bool Contains(uintptr_t address, size_t size) {
    EnsureCoreModuleRegistered();
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& item : g_ranges) {
        if (Overlaps(address, size, item.second.base, item.second.size)) return true;
    }
    return false;
}

std::vector<Range> List() {
    EnsureCoreModuleRegistered();
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Range> out;
    out.reserve(g_ranges.size());
    for (const auto& item : g_ranges) out.push_back(item.second);
    return out;
}

ScopedRange::ScopedRange(uintptr_t base, size_t size, const std::string& owner, const std::string& label)
    : id_(Register(base, size, owner, label, true)) {}

ScopedRange::~ScopedRange() { Unregister(id_); }

ScopedRange::ScopedRange(ScopedRange&& other) noexcept : id_(other.id_) { other.id_ = 0; }

ScopedRange& ScopedRange::operator=(ScopedRange&& other) noexcept {
    if (this == &other) return *this;
    Unregister(id_);
    id_ = other.id_;
    other.id_ = 0;
    return *this;
}

void ScopedRange::Reset(uintptr_t base, size_t size) {
    if (base == 0 || size == 0) {
        Unregister(id_);
        id_ = 0;
    } else if (id_ == 0) {
        id_ = Register(base, size, "scanner", "scratch", true);
    } else {
        Resize(id_, base, size);
    }
}

} // namespace provenance
