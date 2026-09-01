#include "project.h"
#include "../memory/memory.h"
#include "../process/modules.h"

#include <windows.h>
#include <mutex>
#include <fstream>
#include <sstream>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <algorithm>

namespace project {

namespace {

std::mutex g_mutex;
json g_data;
std::string g_path;
bool g_initialized = false;

HMODULE OwnModule() {
    HMODULE h = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&OwnModule), &h);
    return h;
}

std::string ProcessBaseName() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string full(path);
    size_t slash = full.find_last_of("\\/");
    std::string file = slash == std::string::npos ? full : full.substr(slash + 1);
    size_t dot = file.find_last_of('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

std::string HexAddr(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

uintptr_t ParseHexAddr(const std::string& s) {
    return static_cast<uintptr_t>(std::stoull(s, nullptr, 0));
}

bool WriteFileDurably(const std::string& path, std::string_view data) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t writtenTotal = 0;
    bool ok = true;
    while (writtenTotal < data.size()) {
        DWORD chunk = static_cast<DWORD>((std::min)(data.size() - writtenTotal,
                                                    static_cast<size_t>(0x7fffffff)));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + writtenTotal, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        writtenTotal += written;
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return ok;
}

bool SaveToDisk() {
    const std::string tmp = g_path + ".tmp";
    const std::string backup = g_path + ".bak";
    const std::string serialized = g_data.dump(2);
    if (!WriteFileDurably(tmp, serialized)) return false;

    if (GetFileAttributesA(g_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(backup.c_str());
        if (ReplaceFileA(g_path.c_str(), tmp.c_str(), backup.c_str(), REPLACEFILE_WRITE_THROUGH,
                         nullptr, nullptr)) return true;
        CopyFileA(g_path.c_str(), backup.c_str(), FALSE);
    }
    if (MoveFileExA(tmp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileA(tmp.c_str());
    return false;
}

json DefaultSkeleton() {
    return json{{"schema_version", 2}, {"addresses", json::object()}, {"pointer_paths", json::object()}, {"notes", json::array()},
                {"freezes", json::array()}, {"struct_defs", json::array()}, {"re_facts", json::object()},
                {"object_tracks", json::array()}, {"breakpoint_templates", json::array()}};
}

bool LoadJsonFile(const std::string& path, json& out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    try { f >> out; return out.is_object(); } catch (...) { return false; }
}

void MigrateAndRepair(json& data) {
    int version = data.value("schema_version", 0);
    if (version < 1) version = 1; // v0 had the same fields, only no explicit version.
    data["schema_version"] = version;
    if (!data.contains("addresses") || !data["addresses"].is_object()) data["addresses"] = json::object();
    if (!data.contains("pointer_paths") || !data["pointer_paths"].is_object()) data["pointer_paths"] = json::object();
    if (!data.contains("notes") || !data["notes"].is_array()) data["notes"] = json::array();
    if (!data.contains("freezes") || !data["freezes"].is_array()) data["freezes"] = json::array();
    if (!data.contains("struct_defs") || !data["struct_defs"].is_array()) data["struct_defs"] = json::array();
    if (!data.contains("re_facts") || !data["re_facts"].is_object()) data["re_facts"] = json::object();
    if (!data.contains("object_tracks") || !data["object_tracks"].is_array()) data["object_tracks"] = json::array();
    if (!data.contains("breakpoint_templates") || !data["breakpoint_templates"].is_array()) data["breakpoint_templates"] = json::array();
    if (version < 2) data["schema_version"] = 2;
}

} // namespace

void Init(const std::string& directory) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return;

    char dllPath[MAX_PATH] = {};
    GetModuleFileNameA(OwnModule(), dllPath, MAX_PATH);
    std::string dll(dllPath);
    size_t slash = dll.find_last_of("\\/");
    std::string dir = slash == std::string::npos ? "." : dll.substr(0, slash);
    const std::string projectsDir = directory.empty() ? dir + "\\cortex_projects" : directory;
    std::error_code directoryError;
    std::filesystem::create_directories(std::filesystem::path(projectsDir), directoryError);

    g_path = (std::filesystem::path(projectsDir) / (ProcessBaseName() + ".json")).string();

    if (!LoadJsonFile(g_path, g_data) && !LoadJsonFile(g_path + ".bak", g_data)) {
        g_data = DefaultSkeleton();
    }
    MigrateAndRepair(g_data);

    g_initialized = true;
}

json GetAll() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_data;
}

bool SetAddress(const std::string& name, uintptr_t address, const std::string& type, const std::string& notes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    json before = g_data;
    g_data["addresses"][name] = {{"address", HexAddr(address)}, {"type", type}, {"notes", notes}};
    if (SaveToDisk()) return true;
    g_data = std::move(before);
    return false;
}

bool RemoveAddress(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& addrs = g_data["addresses"];
    if (!addrs.contains(name)) return false;
    json before = g_data;
    addrs.erase(name);
    if (SaveToDisk()) return true;
    g_data = std::move(before);
    return false;
}

bool SetPointerPath(const std::string& name, const std::string& moduleName, int64_t baseOffset,
                     const std::vector<int64_t>& offsets, const std::string& finalType, const std::string& notes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    json before = g_data;
    g_data["pointer_paths"][name] = {
        {"module", moduleName}, {"base_offset", baseOffset}, {"offsets", offsets},
        {"final_type", finalType}, {"notes", notes},
    };
    if (SaveToDisk()) return true;
    g_data = std::move(before);
    return false;
}

bool RemovePointerPath(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& paths = g_data["pointer_paths"];
    if (!paths.contains(name)) return false;
    json before = g_data;
    paths.erase(name);
    if (SaveToDisk()) return true;
    g_data = std::move(before);
    return false;
}

std::optional<uintptr_t> ResolvePointerPath(const std::string& name) {
    json entry;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& paths = g_data["pointer_paths"];
        if (!paths.contains(name)) return std::nullopt;
        entry = paths[name];
    }

    std::string moduleName = entry.value("module", "");
    int64_t baseOffset = entry.value("base_offset", 0LL);
    std::vector<int64_t> offsets = entry.value("offsets", std::vector<int64_t>{});

    uintptr_t moduleBase = 0;
    if (moduleName.empty()) {
        auto mods = process::ListModules();
        if (mods.empty()) return std::nullopt;
        moduleBase = mods[0].base; // first-listed module is the main executable
    } else {
        for (const auto& m : process::ListModules()) {
            if (m.name == moduleName) { moduleBase = m.base; break; }
        }
        if (moduleBase == 0) return std::nullopt;
    }

    uintptr_t addr = static_cast<uintptr_t>(static_cast<int64_t>(moduleBase) + baseOffset);
#ifdef _WIN64
    constexpr size_t kPtrSize = 8;
#else
    constexpr size_t kPtrSize = 4;
#endif
    for (int64_t off : offsets) {
        std::vector<uint8_t> buf;
        if (!memory::ReadBytes(addr, kPtrSize, buf)) return std::nullopt;
#ifdef _WIN64
        uint64_t ptr;
        memcpy(&ptr, buf.data(), kPtrSize);
        addr = static_cast<uintptr_t>(static_cast<int64_t>(ptr) + off);
#else
        uint32_t ptr;
        memcpy(&ptr, buf.data(), kPtrSize);
        addr = static_cast<uintptr_t>(static_cast<int64_t>(ptr) + off);
#endif
    }
    return addr;
}

int AddNote(const std::string& text, const std::vector<std::string>& tags) {
    std::lock_guard<std::mutex> lock(g_mutex);
    json before = g_data;
    int id = 1;
    for (const auto& n : g_data["notes"]) {
        int existing = n.value("id", 0);
        if (existing >= id) id = existing + 1;
    }
    g_data["notes"].push_back({{"id", id}, {"text", text}, {"tags", tags}});
    if (SaveToDisk()) return id;
    g_data = std::move(before);
    return -1;
}

bool RemoveNote(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& notes = g_data["notes"];
    for (auto it = notes.begin(); it != notes.end(); ++it) {
        if (it->value("id", 0) == id) {
            json before = g_data;
            notes.erase(it);
            if (SaveToDisk()) return true;
            g_data = std::move(before);
            return false;
        }
    }
    return false;
}

json GetFreezes() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_data["freezes"];
}

void SetFreezes(const json& freezes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    json before = g_data;
    g_data["freezes"] = freezes;
    if (!SaveToDisk()) g_data = std::move(before);
}

json GetStructDefs() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_data["struct_defs"];
}

void SetStructDefs(const json& defs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    json before = g_data;
    g_data["struct_defs"] = defs;
    if (!SaveToDisk()) g_data = std::move(before);
}

json GetReFacts() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_data["re_facts"];
}

bool SetReFact(const std::string& key, const json& value) {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    json before = g_data;
    g_data["re_facts"][key] = value;
    if (SaveToDisk()) return true;
    g_data = std::move(before);
    return false;
}

bool RemoveReFact(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_data["re_facts"].contains(key)) return false;
    json before = g_data;
    g_data["re_facts"].erase(key);
    if (SaveToDisk()) return true;
    g_data = std::move(before);
    return false;
}

json GetObjectTracks() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_data["object_tracks"];
}

bool SetObjectTracks(const json& tracks) {
    if (!tracks.is_array()) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    json before = g_data;
    g_data["object_tracks"] = tracks;
    if (SaveToDisk()) return true;
    g_data = std::move(before);
    return false;
}

json GetBreakpointTemplates() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_data["breakpoint_templates"];
}

bool SetBreakpointTemplates(const json& templates) {
    if (!templates.is_array()) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    json before = g_data;
    g_data["breakpoint_templates"] = templates;
    if (SaveToDisk()) return true;
    g_data = std::move(before);
    return false;
}
} // namespace project

