#include "infer.h"
#include "../memory/memory.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>

namespace structinfer {
namespace {

#ifdef _WIN64
constexpr size_t kPointerSize = 8;
#else
constexpr size_t kPointerSize = 4;
#endif

std::string Hex(uint64_t value) { std::ostringstream out; out << "0x" << std::hex << value; return out.str(); }

bool IsReadablePointer(uintptr_t value) {
    if (!value) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(reinterpret_cast<LPCVOID>(value), &mbi, sizeof(mbi)) == sizeof(mbi) &&
           mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && (mbi.Protect & 0xFF) != PAGE_NOACCESS;
}

bool IsExecutablePointer(uintptr_t value) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!value || VirtualQuery(reinterpret_cast<LPCVOID>(value), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return mbi.State == MEM_COMMIT && (mbi.Protect & exec) != 0;
}

bool IsVtable(uintptr_t value) {
    std::vector<uint8_t> bytes;
    if (!IsReadablePointer(value) || !memory::ReadBytes(value, kPointerSize, bytes)) return false;
    uintptr_t first = 0; memcpy(&first, bytes.data(), kPointerSize);
    return IsExecutablePointer(first);
}

bool PlausibleFloat(float value) {
    if (!std::isfinite(value) || std::fabs(value) > 100000000.0f) return false;
    return value == 0.0f || std::fabs(value) >= 0.000000000001f;
}

std::string FieldName(size_t offset, const std::string& type) {
    std::ostringstream out;
    if (type == "vtable") return "vtable";
    out << "field_" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << offset;
    return out.str();
}

} // namespace

bool Infer(const std::vector<uintptr_t>& instances, size_t size, std::vector<FieldGuess>& fields,
           std::string& error) {
    if (instances.empty()) { error = "no_instances"; return false; }
    if (size < 4 || size > 1024 * 1024) { error = "invalid_size"; return false; }
    std::vector<std::vector<uint8_t>> data;
    for (uintptr_t address : instances) {
        std::vector<uint8_t> bytes;
        if (!memory::ReadBytes(address, size, bytes)) { error = "read_failed_at_" + Hex(address); return false; }
        data.push_back(std::move(bytes));
    }

    fields.clear();
    for (size_t offset = 0; offset + 4 <= size;) {
        std::string type = "i32";
        size_t fieldSize = 4;
        double confidence = 0.58;
        std::vector<std::string> reasons;
        std::vector<std::string> values;
        std::set<std::string> distinct;

        if (offset % kPointerSize == 0 && offset + kPointerSize <= size) {
            size_t pointerCount = 0, vtableCount = 0;
            for (const auto& bytes : data) {
                uintptr_t value = 0; memcpy(&value, bytes.data() + offset, kPointerSize);
                pointerCount += IsReadablePointer(value) ? 1 : 0;
                vtableCount += IsVtable(value) ? 1 : 0;
            }
            if (vtableCount == data.size()) {
                type = "vtable"; fieldSize = kPointerSize; confidence = 0.99;
                reasons.push_back("every instance points to a table whose first entry is executable");
            } else if (pointerCount * 100 >= data.size() * 70) {
                type = "pointer"; fieldSize = kPointerSize; confidence = 0.90;
                reasons.push_back("most instance values point to committed readable memory");
            }
        }

        if (type == "i32" && offset % 4 == 0) {
            auto vectorPlausible = [&](size_t components) {
                if (offset + components * 4 > size) return false;
                size_t usefulComponents = 0;
                std::vector<bool> useful(components, false);
                for (const auto& bytes : data) {
                    for (size_t c = 0; c < components; ++c) {
                        float value = 0; memcpy(&value, bytes.data() + offset + c * 4, 4);
                        if (!PlausibleFloat(value)) return false;
                        useful[c] = useful[c] || std::fabs(value) > 0.0001f;
                    }
                }
                for (bool value : useful) usefulComponents += value ? 1 : 0;
                const size_t required = components == 16 ? 8 : components == 4 ? 3 : 2;
                return usefulComponents >= required;
            };
            if (vectorPlausible(16)) {
                type = "matrix4"; fieldSize = 64; confidence = 0.82;
                reasons.push_back("sixteen consecutive plausible floating-point components");
            } else if (vectorPlausible(4)) {
                type = "vec4"; fieldSize = 16; confidence = 0.78;
                reasons.push_back("four consecutive plausible floating-point components");
            } else if (vectorPlausible(3)) {
                type = "vec3"; fieldSize = 12; confidence = 0.76;
                reasons.push_back("three consecutive plausible floating-point components");
            } else {
                bool floats = true, anyUseful = false;
                for (const auto& bytes : data) {
                    float value = 0; memcpy(&value, bytes.data() + offset, 4);
                    floats = floats && PlausibleFloat(value);
                    anyUseful = anyUseful || std::fabs(value) > 0.0001f;
                }
                if (floats && anyUseful) {
                    type = "float"; confidence = 0.78;
                    reasons.push_back("all instances contain finite non-denormal float values");
                } else {
                    bool boolean = true;
                    for (const auto& bytes : data) {
                        uint32_t value = 0; memcpy(&value, bytes.data() + offset, 4);
                        boolean = boolean && (value == 0 || value == 1);
                    }
                    if (boolean) { type = "u32"; confidence = 0.72; reasons.push_back("all values are boolean-like (0/1)"); }
                    else reasons.push_back("generic aligned 32-bit integer candidate");
                }
            }
        }

        for (const auto& bytes : data) {
            std::ostringstream value;
            value << std::hex << std::setfill('0');
            for (size_t i = 0; i < fieldSize && offset + i < bytes.size(); ++i)
                value << std::setw(2) << static_cast<unsigned>(bytes[offset + i]);
            values.push_back(value.str());
            distinct.insert(value.str());
        }
        const bool constant = distinct.size() == 1;
        reasons.push_back(constant ? "constant across supplied instances" : "varies across supplied instances");
        fields.push_back({offset, fieldSize, type, FieldName(offset, type), confidence, constant,
                          distinct.size(), reasons, values});
        offset += fieldSize;
    }
    return true;
}

} // namespace structinfer
