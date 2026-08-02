#include "symbols.h"

#include <dbghelp.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace symbols {
namespace {

std::mutex g_mutex;
bool g_initialized = false;
HANDLE g_process = GetCurrentProcess();
std::string g_searchPath;
std::unordered_map<uintptr_t, ModuleLoadInfo> g_modules;

std::string BaseName(const std::string& path) {
    const size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string DirectoryOf(const std::string& path) {
    const size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

void AppendSearchPath(std::string& target, const std::string& value) {
    if (value.empty()) return;
    size_t start = 0;
    while (start < value.size()) {
        size_t end = value.find(';', start);
        if (end == std::string::npos) end = value.size();
        std::string part = value.substr(start, end - start);
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.erase(part.begin());
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.pop_back();
        if (!part.empty()) {
            bool found = false;
            size_t existingStart = 0;
            while (existingStart < target.size()) {
                size_t existingEnd = target.find(';', existingStart);
                if (existingEnd == std::string::npos) existingEnd = target.size();
                if (_stricmp(target.substr(existingStart, existingEnd - existingStart).c_str(), part.c_str()) == 0) {
                    found = true;
                    break;
                }
                existingStart = existingEnd + 1;
            }
            if (!found) {
                if (!target.empty()) target.push_back(';');
                target += part;
            }
        }
        start = end + 1;
    }
}

bool ApplySearchPathLocked() {
    if (!g_initialized) return false;
    return SymSetSearchPath(g_process, g_searchPath.empty() ? nullptr : g_searchPath.c_str()) == TRUE;
}

const char* SymbolTypeName(SYM_TYPE type) {
    switch (type) {
        case SymNone: return "none";
        case SymCoff: return "coff";
        case SymCv: return "codeview";
        case SymPdb: return "pdb";
        case SymExport: return "export";
        case SymDeferred: return "deferred";
        case SymSym: return "sym";
        case SymDia: return "dia";
        case SymVirtual: return "virtual";
        default: return "unknown";
    }
}

std::string GuidToString(const GUID& guid) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer),
                  "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned long>(guid.Data1),
                  static_cast<unsigned>(guid.Data2),
                  static_cast<unsigned>(guid.Data3),
                  static_cast<unsigned>(guid.Data4[0]), static_cast<unsigned>(guid.Data4[1]),
                  static_cast<unsigned>(guid.Data4[2]), static_cast<unsigned>(guid.Data4[3]),
                  static_cast<unsigned>(guid.Data4[4]), static_cast<unsigned>(guid.Data4[5]),
                  static_cast<unsigned>(guid.Data4[6]), static_cast<unsigned>(guid.Data4[7]));
    return buffer;
}

struct MappedImage {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;

    ~MappedImage() {
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

bool MapImage(const std::string& path, MappedImage& image) {
    image.file = CreateFileA(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (image.file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(image.file, &size) || size.QuadPart <= 0) return false;
    image.size = static_cast<size_t>(size.QuadPart);
    image.mapping = CreateFileMappingA(image.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!image.mapping) return false;
    image.data = static_cast<const uint8_t*>(MapViewOfFile(image.mapping, FILE_MAP_READ, 0, 0, 0));
    return image.data != nullptr;
}

const uint8_t* RvaToPointer(const uint8_t* data, size_t size,
                            const IMAGE_SECTION_HEADER* sections, WORD sectionCount,
                            DWORD rva, size_t required) {
    for (WORD i = 0; i < sectionCount; ++i) {
        const IMAGE_SECTION_HEADER& section = sections[i];
        const DWORD virtualSize = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (rva < section.VirtualAddress || rva >= section.VirtualAddress + virtualSize) continue;
        const uint64_t offset = static_cast<uint64_t>(section.PointerToRawData) +
                                static_cast<uint64_t>(rva - section.VirtualAddress);
        if (offset + required <= size) return data + offset;
        return nullptr;
    }
    if (static_cast<uint64_t>(rva) + required <= size) return data + rva;
    return nullptr;
}

ModuleIdentity InspectMappedImage(const MappedImage& image) {
    ModuleIdentity out;
    if (!image.data || image.size < sizeof(IMAGE_DOS_HEADER)) return out;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return out;
    const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
    if (ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > image.size) return out;
    const uint8_t* nt = image.data + ntOffset;
    if (*reinterpret_cast<const DWORD*>(nt) != IMAGE_NT_SIGNATURE) return out;
    const auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(nt + sizeof(DWORD));
    const uint8_t* optional = reinterpret_cast<const uint8_t*>(fileHeader + 1);
    if (optional + fileHeader->SizeOfOptionalHeader > image.data + image.size) return out;

    DWORD debugRva = 0;
    DWORD debugSize = 0;
    const IMAGE_SECTION_HEADER* sections = nullptr;
    const WORD magic = *reinterpret_cast<const WORD*>(optional);
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (fileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) return out;
        const auto* header = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(optional);
        out.imageSize = header->SizeOfImage;
        out.preferredImageBase = header->ImageBase;
        if (header->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DEBUG) {
            debugRva = header->DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress;
            debugSize = header->DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size;
        }
        sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(optional + fileHeader->SizeOfOptionalHeader);
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        if (fileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32)) return out;
        const auto* header = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(optional);
        out.imageSize = header->SizeOfImage;
        out.preferredImageBase = header->ImageBase;
        if (header->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DEBUG) {
            debugRva = header->DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress;
            debugSize = header->DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size;
        }
        sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(optional + fileHeader->SizeOfOptionalHeader);
    } else {
        return out;
    }

    const uint8_t* sectionEnd = reinterpret_cast<const uint8_t*>(sections + fileHeader->NumberOfSections);
    if (sectionEnd > image.data + image.size) return out;
    out.validPe = true;
    out.timeDateStamp = fileHeader->TimeDateStamp;

    if (debugRva && debugSize >= sizeof(IMAGE_DEBUG_DIRECTORY)) {
        const uint8_t* debugData = RvaToPointer(image.data, image.size, sections,
                                                fileHeader->NumberOfSections,
                                                debugRva, debugSize);
        if (debugData) {
            const size_t count = debugSize / sizeof(IMAGE_DEBUG_DIRECTORY);
            const auto* directories = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(debugData);
            for (size_t i = 0; i < count; ++i) {
                const IMAGE_DEBUG_DIRECTORY& directory = directories[i];
                if (directory.Type != IMAGE_DEBUG_TYPE_CODEVIEW || directory.SizeOfData < 24) continue;
                const uint8_t* codeView = nullptr;
                if (directory.PointerToRawData &&
                    static_cast<uint64_t>(directory.PointerToRawData) + directory.SizeOfData <= image.size) {
                    codeView = image.data + directory.PointerToRawData;
                } else if (directory.AddressOfRawData) {
                    codeView = RvaToPointer(image.data, image.size, sections,
                                            fileHeader->NumberOfSections,
                                            directory.AddressOfRawData, directory.SizeOfData);
                }
                if (!codeView || std::memcmp(codeView, "RSDS", 4) != 0) continue;
                GUID guid{};
                DWORD age = 0;
                std::memcpy(&guid, codeView + 4, sizeof(guid));
                std::memcpy(&age, codeView + 20, sizeof(age));
                out.hasCodeView = true;
                out.pdbGuid = GuidToString(guid);
                out.pdbAge = age;
                const char* pdb = reinterpret_cast<const char*>(codeView + 24);
                const size_t maxPdb = directory.SizeOfData - 24;
                const size_t pdbLength = strnlen(pdb, maxPdb);
                out.pdbPathHint.assign(pdb, pdbLength);
                break;
            }
        }
    }

    char build[256]{};
