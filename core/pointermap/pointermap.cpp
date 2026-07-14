#include "pointermap.h"
#include "../config.h"
#include "../memory/scan.h"

#include <windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace pointermap {
namespace {

std::string ProcessName() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string name(path);
    const size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.erase(dot);
    return name;
}

std::string SafeName(const std::string& input) {
    std::string out;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_') out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string Directory() {
    const std::string projects = config::GetModuleDir() + "\\cortex_projects";
    CreateDirectoryA(projects.c_str(), nullptr);
    const std::string process = projects + "\\" + ProcessName() + "_pointermaps";
    CreateDirectoryA(process.c_str(), nullptr);
    return process;
}

std::string PathFor(const std::string& name) { return Directory() + "\\" + SafeName(name) + ".json"; }

std::string Hex(uintptr_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

uintptr_t ParseAddress(const json& value) {
    if (value.is_string()) return static_cast<uintptr_t>(std::stoull(value.get<std::string>(), nullptr, 0));
    return static_cast<uintptr_t>(value.get<uint64_t>());
}

std::string Key(const Path& path) {
    std::ostringstream out;
    out << path.module << ':' << path.baseOffset;
    for (int64_t offset : path.offsets) out << ':' << offset;
    return out.str();
}

double Score(const Path& path, unsigned sessionCount) {
    double score = 45.0; // every stored path is module rooted
    score += (std::min)(35.0, static_cast<double>(sessionCount) * 12.0);
    score += (std::max)(0.0, 20.0 - static_cast<double>(path.offsets.size()) * 3.0);
    return (std::min)(100.0, score);
}

bool ReadJson(const std::string& path, json& data) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    try { in >> data; return data.is_object(); } catch (...) { return false; }
}

bool WriteJson(const std::string& path, const json& data) {
    const std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << data.dump(2);
    out.flush();
    if (!out.good()) return false;
    out.close();
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

} // namespace

bool Capture(const std::string& name, uintptr_t target, int maxDepth, uint32_t maxOffset,
             MapInfo& out, std::string& error) {
    const std::string safe = SafeName(name);
    if (safe.empty() || safe != name) { error = "invalid_name"; return false; }
    bool truncated = false;
    const auto found = memscan::PointerScan(target, maxDepth, maxOffset, truncated);
    json paths = json::array();
    for (const auto& path : found) {
        paths.push_back({{"module", path.module}, {"base_offset", path.base_offset}, {"offsets", path.offsets}});
    }
    const uint64_t created = GetTickCount64();
    json data{{"schema_version", 1}, {"name", safe}, {"process", ProcessName()},
              {"target", Hex(target)}, {"created_ms", created}, {"max_depth", maxDepth},
              {"max_offset", maxOffset}, {"truncated", truncated}, {"paths", paths}};
    if (!WriteJson(PathFor(safe), data)) { error = "write_failed"; return false; }
    out = {safe, target, created, found.size(), truncated};
    return true;
}

bool Load(const std::string& name, MapInfo& info, std::vector<Path>& paths, std::string& error) {
    json data;
    if (!ReadJson(PathFor(name), data)) { error = "pointermap_not_found"; return false; }
    try {
        info.name = data.at("name").get<std::string>();
        info.target = ParseAddress(data.at("target"));
        info.createdMs = data.value("created_ms", uint64_t{0});
        info.truncated = data.value("truncated", false);
        paths.clear();
        for (const auto& item : data.at("paths")) {
            Path path;
            path.module = item.at("module").get<std::string>();
            path.baseOffset = item.at("base_offset").get<int64_t>();
            path.offsets = item.at("offsets").get<std::vector<int64_t>>();
            path.score = Score(path, 1);
            paths.push_back(std::move(path));
        }
        info.pathCount = paths.size();
        return true;
    } catch (...) { error = "invalid_pointermap"; return false; }
}

std::vector<MapInfo> List() {
    std::vector<MapInfo> out;
    WIN32_FIND_DATAA fd{};
    HANDLE find = FindFirstFileA((Directory() + "\\*.json").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        std::string file = fd.cFileName;
        if (file.size() <= 5) continue;
        std::string name = file.substr(0, file.size() - 5);
        MapInfo info{};
        std::vector<Path> ignored;
        std::string error;
        if (Load(name, info, ignored, error)) out.push_back(info);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    std::sort(out.begin(), out.end(), [](const MapInfo& a, const MapInfo& b) { return a.createdMs > b.createdMs; });
    return out;
}

bool Intersect(const std::vector<std::string>& names, std::vector<Path>& paths, std::string& error) {
    if (names.size() < 2) { error = "need_at_least_two_pointermaps"; return false; }
    std::map<std::string, std::pair<Path, unsigned>> counts;
    for (const auto& name : names) {
        MapInfo info{};
        std::vector<Path> current;
        if (!Load(name, info, current, error)) return false;
        std::set<std::string> seen;
        for (const auto& path : current) {
            const std::string key = Key(path);
            if (!seen.insert(key).second) continue;
            auto& entry = counts[key];
            if (entry.second == 0) entry.first = path;
            entry.second++;
        }
    }
    paths.clear();
    for (auto& item : counts) {
        if (item.second.second != names.size()) continue;
        item.second.first.sessions = item.second.second;
        item.second.first.score = Score(item.second.first, item.second.second);
        paths.push_back(std::move(item.second.first));
    }
    std::sort(paths.begin(), paths.end(), [](const Path& a, const Path& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.offsets.size() < b.offsets.size();
    });
    return true;
}

bool Remove(const std::string& name) { return DeleteFileA(PathFor(name).c_str()) != FALSE; }

} // namespace pointermap
