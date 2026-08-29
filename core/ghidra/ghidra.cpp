#include "ghidra.h"
#include "../config.h"
#include "../debugger/debugger.h"
#include "../process/modules.h"
#include "../process/address.h"
#include "../project/project.h"
#include "../struct/structs.h"
#include "../re/re_tools.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <set>

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

def runtime_addr(value):
    runtime = int(value, 0) if isinstance(value, basestring) else int(value)
    return toAddr(image + runtime - runtime_main)

def safe_label(name):
    return "".join(c if c.isalnum() or c == '_' else '_' for c in name)

for item in data.get("addresses", []):
    addr = runtime_addr(item["address"])
    createLabel(addr, safe_label(item.get("name", "cortex_runtime")), True, SourceType.USER_DEFINED)
    note = item.get("notes", "")
    if note:
        setEOLComment(addr, note)

# Runtime-discovered C++ subobjects/vtables become labels/comments so a track
# made in Cortex remains understandable in the static database.
for obj in data.get("tracked_objects", []):
    obj_name = safe_label(obj.get("name", "tracked_object"))
    if obj.get("address"):
        try:
            setEOLComment(runtime_addr(obj["address"]), "Cortex tracked object: %s" % obj_name)
        except Exception:
            pass
    for sub in obj.get("subobjects", []):
        try:
            off = int(sub.get("offset", 0))
            vt = runtime_addr(sub["vtable"])
            createLabel(vt, "%s_sub_%X_vtable" % (obj_name, off), True, SourceType.USER_DEFINED)
            setEOLComment(vt, "Cortex runtime vtable; subobject +0x%X, this-adjust %s" % (off, sub.get("this_adjust", 0)))
        except Exception:
            pass

# Observed runtime call edges are kept as comments on the caller/callee. This
# does not invent a static reference when Ghidra cannot prove one.
for edge in data.get("runtime_xrefs", []):
    try:
        src = runtime_addr(edge["from"])
        dst_name = edge.get("to_name", edge.get("to", "?"))
        old = getEOLComment(src) or ""
        line = "Cortex observed call -> %s" % dst_name
        if line not in old:
            setEOLComment(src, (old + "\n" + line).strip())
    except Exception:
        pass

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
        struct.add(dt, dt.getLength(), field["name"], "Cortex runtime/project type")
        cursor += dt.getLength()
    dtm.addDataType(struct, None)

print("Cortex import complete: %d addresses, %d structures, %d tracked objects, %d runtime xrefs" %
      (len(data.get("addresses", [])), len(data.get("structs", [])), len(data.get("tracked_objects", [])), len(data.get("runtime_xrefs", []))))
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

    json trackedObjects = json::array();
    const json trackList = retools::ListTracks();
    for (const auto& row : trackList.value("tracks",json::array())) {
        json snapshot = retools::GetTrack(row.value("id",0));
        if (snapshot.value("ok",false)) trackedObjects.push_back(std::move(snapshot));
    }

    json runtimeXrefs = json::array();
    std::set<std::pair<uintptr_t,uintptr_t>> seenXrefs;
    for (const auto& bp : dbg::ListBreakpoints()) {
        std::vector<dbg::BpLogEntry> entries; uint64_t dropped=0,total=0;
        if (!dbg::GetBreakpointLogPaged(bp.id,0,0,entries,dropped,total)) continue;
        for (const auto& hit : entries) {
            if (hit.stack.empty()) continue;
            const uintptr_t caller=hit.stack.front(), callee=hit.instruction;
            if (!seenXrefs.insert({caller,callee}).second) continue;
            runtimeXrefs.push_back({{"from",Hex(caller)},{"from_name",process::DescribeAddress(caller)},
                                    {"to",Hex(callee)},{"to_name",process::DescribeAddress(callee)},
                                    {"kind","observed_callstack"}});
        }
    }
    json document{{"schema_version",2},{"exported_ms",GetTickCount64()},{"pointer_size",sizeof(uintptr_t)},
                  {"modules",modules},{"addresses",addresses},{"pointer_paths",projectData.value("pointer_paths",json::object())},
                  {"structs",definitions},{"traces",traces},{"re_facts",project::GetReFacts()},
                  {"tracked_objects",trackedObjects},{"runtime_xrefs",runtimeXrefs}};
    if (!Write(jsonPath, document.dump(2))) { error = "export_write_failed"; return false; }
    if (!Write(scriptPath, ImportScript)) { error = "script_write_failed"; return false; }
    return true;
}

bool ImportAnnotations(const json& document, size_t& imported, std::string& error) {
    if (!document.is_object()) { error = "document_object_required"; return false; }
    imported = 0;
    auto importAddressArray = [&](const json& rows, const std::string& defaultType) {
        if (!rows.is_array()) return;
        for (const auto& item : rows) {
            try {
                const std::string name=item.at("name").get<std::string>();
                const std::string notes=item.value("notes",item.value("comment",std::string()));
                const std::string type=item.value("type",defaultType);
                if (project::SetAddress(name,Address(item.at("address")),type,notes)) ++imported;
            } catch (...) {}
        }
    };
    if (document.contains("addresses")) importAddressArray(document["addresses"], "");
    if (document.contains("symbols")) importAddressArray(document["symbols"], "symbol");
    if (document.contains("vtables")) importAddressArray(document["vtables"], "vtable");

    if (document.contains("structs") && document["structs"].is_array()) {
        for (const auto& item : document["structs"]) {
            try {
                std::vector<structs::Field> fields;
                for (const auto& field : item.value("fields",json::array()))
                    fields.push_back({field.at("name").get<std::string>(),field.at("offset").get<int64_t>(),
                                      field.value("type",std::string("bytes")),field.value("count",0)});
                if (structs::Define(item.at("name").get<std::string>(),fields)) ++imported;
            } catch (...) {}
        }
    }
    if (document.contains("re_facts") && document["re_facts"].is_object()) {
        for (auto it=document["re_facts"].begin();it!=document["re_facts"].end();++it)
            if (project::SetReFact(it.key(),it.value())) ++imported;
    }
    if (document.contains("xrefs") && document["xrefs"].is_array()) {
        project::SetReFact("ghidra.xrefs",document["xrefs"]); ++imported;
    }
    if (imported==0 && !document.contains("addresses") && !document.contains("symbols") &&
        !document.contains("vtables") && !document.contains("structs") && !document.contains("re_facts")) {
        error="addresses_symbols_vtables_structs_or_re_facts_required"; return false;
    }
    return true;
}

} // namespace ghidra


