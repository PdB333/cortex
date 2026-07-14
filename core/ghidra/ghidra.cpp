#include "ghidra.h"
#include "../config.h"
#include "../debugger/debugger.h"
#include "../process/modules.h"
#include "../project/project.h"
#include "../struct/structs.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace ghidra {
namespace {

std::string SafeName(const std::string& input) {
    std::string out;
    for (unsigned char c : input) if (std::isalnum(c) || c == '-' || c == '_') out.push_back(static_cast<char>(c));
    return out;
}

std::string Hex(uintptr_t value) { std::ostringstream out; out << "0x" << std::hex << value; return out.str(); }

uintptr_t Address(const json& value) {
    if (value.is_string()) return static_cast<uintptr_t>(std::stoull(value.get<std::string>(), nullptr, 0));
    return static_cast<uintptr_t>(value.get<uint64_t>());
}

bool Write(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    return out.good();
}

const char* ImportScript = R"PY(# Cortex runtime annotation importer for Ghidra
#@category Cortex
import json
from ghidra.program.model.symbol import SourceType
from ghidra.program.model.data import StructureDataType, ByteDataType, IntegerDataType, FloatDataType, PointerDataType

f = askFile("Select a Cortex runtime export", "Import")
data = json.load(open(f.absolutePath, "r"))
image = currentProgram.getImageBase().getOffset()
runtime_main = int(data["modules"][0]["base"], 0) if data.get("modules") else image

for item in data.get("addresses", []):
    runtime = int(item["address"], 0)
    addr = toAddr(image + runtime - runtime_main)
    name = item.get("name", "cortex_%x" % runtime)
    createLabel(addr, name, True, SourceType.USER_DEFINED)
    if item.get("notes"):
        setEOLComment(addr, item["notes"])

dtm = currentProgram.getDataTypeManager()
for item in data.get("structs", []):
    struct = StructureDataType(item["name"], 0)
    cursor = 0
    for field in item.get("fields", []):
        off = int(field["offset"])
        while cursor < off:
            struct.add(ByteDataType.dataType, 1, None, None); cursor += 1
        typ = field.get("type", "bytes")
        dt = PointerDataType.dataType if typ in ("pointer", "vtable") else FloatDataType.dataType if typ == "float" else IntegerDataType.dataType
        struct.add(dt, dt.getLength(), field["name"], "Cortex inferred")
        cursor += dt.getLength()
    dtm.addDataType(struct, None)

print("Cortex import complete: %d addresses, %d structures" % (len(data.get("addresses", [])), len(data.get("structs", []))))
)PY";

} // namespace

bool ExportRuntime(const std::string& requestedName, std::string& jsonPath, std::string& scriptPath, std::string& error) {
    std::string name = SafeName(requestedName);
    if (name.empty()) name = "runtime_" + std::to_string(GetTickCount64());
    const std::string dir = config::GetModuleDir() + "\\cortex_ghidra";
    CreateDirectoryA(dir.c_str(), nullptr);
    jsonPath = dir + "\\" + name + ".json";
    scriptPath = dir + "\\CortexImport.py";

    json modules = json::array();
    for (const auto& module : process::ListModules())
        modules.push_back({{"name",module.name},{"base",Hex(module.base)},{"size",module.size}});

    json addresses = json::array();
    const json projectData = project::GetAll();
    const json addressDefinitions = projectData.value("addresses", json::object());
    for (auto it = addressDefinitions.begin(); it != addressDefinitions.end(); ++it) {
        addresses.push_back({{"name",it.key()},{"address",it.value().value("address",std::string("0x0"))},
                             {"type",it.value().value("type",std::string())},
                             {"notes",it.value().value("notes",std::string())}});
    }

    json definitions = json::array();
    for (const auto& definition : structs::List()) {
        json fields = json::array();
        for (const auto& field : definition.fields)
            fields.push_back({{"name",field.name},{"offset",field.offset},{"type",field.type},{"count",field.count}});
        definitions.push_back({{"name",definition.name},{"fields",fields}});
    }

    json traces = json::array();
    for (const auto& info : dbg::ListTraces()) {
        std::vector<std::pair<uintptr_t,uint64_t>> coverage;
        dbg::GetTraceCoverage(info.id, coverage);
        json hits = json::array();
        for (const auto& entry : coverage) hits.push_back({{"address",Hex(entry.first)},{"hits",entry.second}});
        traces.push_back({{"id",info.id},{"thread_id",info.threadId},{"coverage",hits}});
    }

    json document{{"schema_version",1},{"exported_ms",GetTickCount64()},{"pointer_size",sizeof(uintptr_t)},
                  {"modules",modules},{"addresses",addresses},{"pointer_paths",projectData.value("pointer_paths",json::object())},
                  {"structs",definitions},{"traces",traces}};
    if (!Write(jsonPath, document.dump(2))) { error = "export_write_failed"; return false; }
    if (!Write(scriptPath, ImportScript)) { error = "script_write_failed"; return false; }
    return true;
}

bool ImportAnnotations(const json& document, size_t& imported, std::string& error) {
    if (!document.is_object() || !document.contains("addresses") || !document["addresses"].is_array()) {
        error = "addresses_array_required"; return false;
    }
    imported = 0;
    for (const auto& item : document["addresses"]) {
        try {
            if (project::SetAddress(item.at("name").get<std::string>(), Address(item.at("address")),
                                    item.value("type",std::string()), item.value("notes",std::string()))) imported++;
        } catch (...) { /* keep importing independent annotations */ }
    }
    return true;
}

} // namespace ghidra
