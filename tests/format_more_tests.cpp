// Formats beyond wave 1: each fixture comes from the real toolchain and each
// expected value from an independent reader (see the make_*.py generators in
// tests/fixtures/format-more). Usage: cortex_format_more_tests <fixtures dir>
#include "artifacts/artifact.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace cortex::artifacts;

namespace {

struct Tests {
    std::string root;
    int checks = 0, failures = 0;

    void Check(bool ok, const std::string& label) {
        ++checks;
        if (!ok) { ++failures; std::cerr << "FAIL " << label << '\n'; }
    }
    ByteSourcePtr File(const std::string& path) {
        auto source = OpenFile(root + "/format-more/" + path);
        if (!source) throw std::runtime_error("missing fixture: " + path);
        return source;
    }
    json Inspect(const std::string& path) { return DefaultRegistry().Inspect(*File(path)); }
    json Expected(const std::string& path) {
        std::ifstream in(root + "/format-more/" + path);
        if (!in) throw std::runtime_error("missing expected file: " + path);
        return json::parse(in);
    }
    // A fixture cut short must be refused, never crash or claim success.
    void Truncated(const std::string& path, size_t keep) {
        auto bytes = File(path)->ReadSome(0, keep);
        const auto report = DefaultRegistry().Inspect(*FromBytes(std::move(bytes), path + "#truncated"));
        Check(!report.value("ok", false), "truncated " + path + " refused");
    }
    void Run(const char* name, const std::function<void()>& action) {
        try { action(); }
        catch (const std::exception& e) { ++failures; std::cerr << name << ": " << e.what() << '\n'; }
    }
};

const json* FindByName(const json& list, const std::string& name) {
    for (const auto& item : list) if (item.value("name", "") == name) return &item;
    return nullptr;
}

// ------------------------------------------------------------- bytecode ---

void ComparePyCode(Tests& t, const json& got, const json& want, const std::string& path) {
    t.Check(got.value("name", "") == want.at("name"), path + " name");
    t.Check(got.value("qualname", "") == want.at("qualname"), path + " qualname");
    t.Check(got.value("first_line", -1) == want.at("first_line"), path + " first line");
    t.Check(got.value("argcount", -1) == want.at("argcount"), path + " argcount");
    t.Check(got.value("kwonly_argcount", -1) == want.at("kwonly_argcount"), path + " kwonly");
    t.Check(got.at("names") == want.at("names"), path + " names");
    const auto& ins = got.at("instructions");
    const auto& ref = want.at("instructions");
    bool same = ins.size() == ref.size();
    for (size_t i = 0; same && i < ins.size(); ++i)
        same = ins[i].at("offset") == ref[i][0] && ins[i].at("opname") == ref[i][1] && ins[i].at("arg") == ref[i][2];
    t.Check(same, path + " instructions match dis (" + std::to_string(ins.size()) + " vs " + std::to_string(ref.size()) + ")");
    const auto& fns = got.at("functions");
    const auto& refFns = want.at("functions");
    t.Check(fns.size() == refFns.size(), path + " nested code count");
    for (size_t i = 0; i < fns.size() && i < refFns.size(); ++i)
        ComparePyCode(t, fns[i], refFns[i], path + "/" + refFns[i].at("name").get<std::string>());
}

void Python(Tests& t) {
    const auto expected = t.Expected("bytecode/expected.json").at("pyc");
    const auto report = t.Inspect("bytecode/sample313.pyc");
    t.Check(report.value("ok", false) && report.value("format", "") == "pyc", "pyc detected");
    const auto& data = report.at("data");
    t.Check(data.at("python_version") == "3.13", "pyc version 3.13");
    t.Check(data.at("magic") == expected.at("magic"), "pyc magic");
    t.Check(data.at("invalidation") == "timestamp", "pyc timestamp invalidation");
    t.Check(data.at("disassembly") == "cpython-3.13", "pyc 3.13 disassembly available");
    ComparePyCode(t, data.at("module"), expected.at("module"), "pyc module");
    const auto hashed = t.Inspect("bytecode/sample313-hash.pyc");
    t.Check(hashed.at("data").at("invalidation") == "unchecked-hash", "pyc unchecked hash");
    t.Check(hashed.at("data").at("source_hash").get<std::string>().size() == 16, "pyc source hash");
    t.Truncated("bytecode/sample313.pyc", 200);
}

void Java(Tests& t) {
    const auto expected = t.Expected("bytecode/expected.json").at("java");
    const auto report = t.Inspect("bytecode/Sample.class");
    t.Check(report.value("ok", false) && report.value("format", "") == "java-class", "class detected");
    const auto& data = report.at("data");
    t.Check(data.at("class") == expected.at("class"), "class name");
    t.Check(data.at("super") == expected.at("super"), "class super");
    t.Check(data.at("interfaces") == expected.at("interfaces"), "class interfaces");
    t.Check(data.at("major_version") == expected.at("major"), "class major version");
    t.Check(data.at("java_version") == "17", "class Java 17");
    t.Check(data.value("source_file", "") == "Sample.java", "class source file");
    const auto* limit = FindByName(data.at("fields"), "LIMIT");
    t.Check(limit && limit->value("constant_value", "") == "42", "class constant field");
    for (const auto& [name, ops] : expected.at("methods").items()) {
        const auto* method = FindByName(data.at("methods"), name);
        t.Check(method != nullptr, "method " + name + " present");
        if (!method) continue;
        const auto& ins = method->at("instructions");
        bool same = ins.size() == ops.size();
        for (size_t i = 0; same && i < ins.size(); ++i)
            same = ins[i].at("offset") == ops[i][0] && ins[i].at("op") == ops[i][1];
        t.Check(same, "method " + name + " instructions match javap -c");
    }
    t.Truncated("bytecode/Sample.class", 300);
}

void Lua(Tests& t) {
    for (const char* name : {"lua51", "lua52", "lua53", "lua54"}) {
        const std::string version = std::string(name).substr(3, 1) + "." + std::string(name).substr(4, 1);
        // string.dump can strip debug information from Lua 5.3 on.
        const bool canStrip = std::string(name) >= "lua53";
        for (bool stripped : {false, true}) {
            if (stripped && !canStrip) continue;
            const std::string file = std::string("bytecode/") + name + (stripped ? "-stripped" : "") + ".luac";
            const auto report = t.Inspect(file);
            t.Check(report.value("ok", false) && report.value("format", "") == "lua-bytecode", file + " detected");
            if (!report.value("ok", false)) { std::cerr << "  " << report.dump() << '\n'; continue; }
            const auto& data = report.at("data");
            t.Check(data.at("lua_version") == version, file + " version");
            t.Check(data.at("function_count") == 3, file + " three prototypes");
            const auto& main = data.at("main");
            bool greeting = false;
            for (const auto& k : main.at("constants")) greeting = greeting || k == "cortex lua fixture";
            t.Check(greeting, file + " main string constant");
            const auto& scale = main.at("functions").at(0);
            t.Check(scale.at("parameters") == 2, file + " scale parameters");
            bool half = false;
            for (const auto& k : scale.at("constants")) half = half || (k.is_number() && k.get<double>() == 0.5);
            t.Check(half, file + " scale float constant");
            if (stripped) {
                t.Check(scale.at("locals").empty() && scale.at("stripped") == true, file + " debug names stripped");
            } else {
                t.Check(scale.at("locals") == json::array({"value", "factor", "result"}), file + " local names");
            }
        }
        t.Truncated(std::string("bytecode/") + name + ".luac", 40);
    }
    for (const char* name : {"luajit20", "luajit21"}) {
        for (bool stripped : {false, true}) {
            const std::string file = std::string("bytecode/") + name + (stripped ? "-stripped" : "") + ".luac";
            const auto report = t.Inspect(file);
            t.Check(report.value("ok", false) && report.value("format", "") == "luajit-bytecode", file + " detected");
            const auto& data = report.at("data");
            t.Check(data.at("stripped") == stripped, file + " strip flag");
            t.Check(data.at("prototypes").size() == 3, file + " three prototypes");
            const auto& main = data.at("prototypes").back();
            t.Check(main.at("has_child") == true, file + " main prototype has children");
            if (!stripped) t.Check(data.value("chunk_name", "").find("local greeting") != std::string::npos, file + " chunk name is the source");
        }
    }
}

void Wasm(Tests& t) {
    const auto expected = t.Expected("bytecode/expected.json").at("wasm");
    const auto report = t.Inspect("bytecode/module.wasm");
    t.Check(report.value("ok", false) && report.value("format", "") == "wasm", "wasm detected");
    const auto& data = report.at("data");
    std::vector<int> ids;
    for (const auto& s : data.at("sections")) ids.push_back(s.at("id"));
    t.Check(json(ids) == expected.at("section_ids"), "wasm sections match llvm-readobj");
    std::vector<std::string> exports;
    for (const auto& e : data.at("exports")) exports.push_back(e.at("name"));
    t.Check(json(exports) == expected.at("exports"), "wasm exports");
    t.Check(data.at("imports").size() == 1 && data.at("imports")[0].at("module") == "env" &&
            data.at("imports")[0].at("name") == "host_log", "wasm import");
    t.Check(data.at("imported_function_count") == 1 && data.at("defined_function_count") == 2, "wasm function counts");
    t.Truncated("bytecode/module.wasm", 60);
}


// --------------------------------------------------------------- native ---

std::vector<std::string> Names(const json& list, const char* key = "name") {
    std::vector<std::string> out;
    for (const auto& item : list) out.push_back(item.at(key).get<std::string>());
    return out;
}

void MachO(Tests& t) {
    const auto expected = t.Expected("native/expected.json").at("macho");
    for (const char* arch : {"x86_64", "arm64"}) {
        const std::string file = std::string("native/macho-") + arch + ".o";
        const auto report = t.Inspect(file);
        t.Check(report.value("ok", false) && report.value("format", "") == "macho", file + " detected");
        const auto& data = report.at("data");
        t.Check(data.at("cpu") == arch && data.at("file_type") == "object", file + " cpu and type");
        std::vector<std::string> sections;
        for (const auto& seg : data.at("segments")) for (const auto& s : seg.at("sections")) sections.push_back(s.at("name"));
        t.Check(json(sections) == expected.at(arch).at("sections"), file + " sections match llvm-readobj");
        std::vector<std::string> symbols;
        for (const auto& s : data.at("symbols")) if (!s.at("debug").get<bool>()) symbols.push_back(s.at("name"));
        t.Check(json(symbols) == expected.at(arch).at("symbols"), file + " symbols match llvm-readobj");
        bool undefinedHelper = false;
        for (const auto& s : data.at("symbols")) undefinedHelper = undefinedHelper || (s.at("name") == "_cortex_external_helper" && s.at("undefined") == true);
        t.Check(undefinedHelper, file + " undefined external");
        t.Check(data.at("build_version").at("platform") == "macos" && data.at("build_version").at("minimum_os") == "11.0.0", file + " build version");
        t.Truncated(file, 100);
    }
    auto fat = t.File("native/macho-fat.o");
    const auto report = DefaultRegistry().Inspect(*fat);
    t.Check(report.value("ok", false) && report.value("format", "") == "macho-fat", "fat detected (not a Java class)");
    std::vector<uint64_t> offsets;
    for (const auto& a : report.at("data").at("architectures")) offsets.push_back(a.at("offset"));
    t.Check(json(offsets) == expected.at("fat_offsets"), "fat slices match llvm-objdump");
    const auto list = DefaultRegistry().ListChildren(*fat);
    t.Check(list.at("children").size() == 2, "fat lists two slices");
    std::string error;
    auto arm = DefaultRegistry().OpenChild(fat, "arm64", error);
    t.Check(arm && DefaultRegistry().Inspect(*arm).at("data").at("cpu") == "arm64", "fat slice opens as arm64 Mach-O");
}

void Coff(Tests& t) {
    const auto expected = t.Expected("native/expected.json").at("coff");
    for (const char* arch : {"amd64", "i386"}) {
        const std::string file = std::string("native/coff-") + arch + ".obj";
        const auto report = t.Inspect(file);
        t.Check(report.value("ok", false) && report.value("format", "") == "coff-object", file + " detected");
        const auto& data = report.at("data");
        t.Check(data.at("machine") == arch, file + " machine");
        t.Check(json(Names(data.at("sections"))) == expected.at(arch).at("sections"), file + " sections (long name) match llvm-readobj");
        t.Check(json(Names(data.at("symbols"))) == expected.at(arch).at("symbols"), file + " symbols match llvm-readobj");
        const std::string prefix = std::string(arch) == "i386" ? "_" : "";
        t.Check(data.at("undefined_externals") == json::array({prefix + "cortex_external_helper"}), file + " undefined external");
    }
    auto lib = t.File("native/coff.lib");
    const auto list = DefaultRegistry().ListChildren(*lib);
    bool found = false;
    for (const auto& child : list.at("children")) {
        const std::string name = child.at("name");
        if (name.find("coff-amd64.obj") == std::string::npos || !child.value("readable", false)) continue;
        std::string error;
        auto member = DefaultRegistry().OpenChild(lib, name, error);
        found = member && DefaultRegistry().Inspect(*member).value("format", "") == "coff-object";
    }
    t.Check(found, ".lib member opens as a COFF object");
}

void Go(Tests& t) {
    const auto expected = t.Expected("native/expected.json").at("go");
    for (const char* name : {"go-linux", "go-windows.exe"}) {
        const std::string key = std::string(name).substr(0, std::string(name).find('.'));
        const auto& want = expected.at(key);
        const auto report = t.Inspect(std::string("native/") + name);
        t.Check(report.value("ok", false), std::string(name) + " inspected");
        const auto& go = report.at("data").at("go");
        t.Check(go.at("version") == want.at("version"), std::string(name) + " Go version matches go version -m");
        t.Check(go.at("path") == want.at("path"), std::string(name) + " main path");
        t.Check(go.at("main_module").at("path") == want.at("main_module"), std::string(name) + " main module");
        bool settings = true;
        for (const auto& [k, v] : want.at("build").items()) settings = settings && go.at("build_settings").value(k, "") == v;
        t.Check(settings, std::string(name) + " build settings");
        const auto& functions = go.at("pclntab").at("functions");
        t.Check(go.at("pclntab").at("layout") == "1.20+", std::string(name) + " pclntab layout");
        for (const auto& fn : want.at("main_functions")) {
            bool present = false;
            for (const auto& f : functions) present = present || f == fn;
            t.Check(present, std::string(name) + " stripped binary still names " + fn.get<std::string>());
        }
    }
}


// ----------------------------------------------------------------- data ---

void Sqlite(Tests& t) {
    const auto expected = t.Expected("data/expected.json").at("sqlite");
    const auto report = t.Inspect("data/sample.db");
    t.Check(report.value("ok", false) && report.value("format", "") == "sqlite", "sqlite detected");
    if (!report.value("ok", false)) { std::cerr << report.dump() << '\n'; return; }
    const auto& data = report.at("data");
    t.Check(data.at("page_size") == expected.at("page_size"), "sqlite page size");
    t.Check(data.at("user_version") == expected.at("user_version"), "sqlite user_version");
    t.Check(data.at("application_id") == expected.at("application_id"), "sqlite application_id");
    const auto& schema = data.at("schema");
    t.Check(schema.size() == expected.at("schema").size(), "sqlite schema row count");
    for (size_t i = 0; i < schema.size() && i < expected.at("schema").size(); ++i) {
        const auto& want = expected.at("schema")[i];
        t.Check(schema[i].at("type") == want[0] && schema[i].at("name") == want[1] && schema[i].at("table") == want[2] &&
                schema[i].at("root_page") == want[3] && schema[i].at("sql") == want[4], "sqlite schema row " + want[1].get<std::string>());
    }
    for (const auto& table : data.at("tables")) {
        const std::string name = table.at("name");
        t.Check(table.value("rows", -1) == expected.at("counts").at(name), "sqlite rows of " + name + " match count(*)");
    }
    const auto walReport = t.Inspect("data/wal.db-wal");
    t.Check(walReport.value("format", "") == "sqlite-wal", "sqlite WAL detected");
    t.Check(walReport.at("data").at("frames") == expected.at("wal_frames"), "WAL frames match wal_checkpoint");
    t.Check(walReport.at("data").at("page_size") == expected.at("wal_page_size"), "WAL page size");
    // A cut database still shows what its first pages hold, and says it is cut.
    const auto cut = DefaultRegistry().Inspect(*FromBytes(t.File("data/sample.db")->ReadSome(0, 3072), "cut.db"));
    bool flagged = false;
    for (const auto& w : cut.at("data").at("warnings")) flagged = flagged || w.get<std::string>().rfind("file_shorter_than_header", 0) == 0;
    t.Check(flagged, "cut sqlite database flagged");
}

void Plist(Tests& t) {
    const auto expected = t.Expected("data/expected.json").at("bplist");
    const auto report = t.Inspect("data/sample.bplist");
    t.Check(report.value("ok", false) && report.value("format", "") == "bplist", "bplist detected");
    const auto& root = report.at("data").at("root");
    t.Check(root.at("ascii") == "hello" && root.at("unicode") == expected.at("unicode"), "bplist strings (ASCII and UTF-16)");
    t.Check(root.at("int") == 42 && root.at("negative") == -7 && root.at("big") == expected.at("big"), "bplist integers");
    t.Check(root.at("real") == 1.5 && root.at("true") == true && root.at("false") == false, "bplist real and booleans");
    t.Check(root.at("data").at("$data") == expected.at("data_hex"), "bplist data");
    t.Check(root.at("date").at("$date") == expected.at("date_seconds"), "bplist date");
    t.Check(root.at("array") == json::array({1, "two", json::array({3.0})}) && root.at("nested").at("k") == "v", "bplist containers");
    const auto keyed = t.Inspect("data/keyed.bplist");
    t.Check(keyed.at("data").at("keyed_archive").at("classes") == expected.at("classes"), "NSKeyedArchiver classes");
    t.Truncated("data/sample.bplist", 100);
}

void Pickle(Tests& t) {
    const auto expected = t.Expected("data/expected.json").at("pickle");
    for (const auto& [name, want] : expected.items()) {
        const auto report = t.Inspect("data/" + name);
        t.Check(report.value("ok", false) && report.value("format", "") == "pickle", name + " detected");
        if (!report.value("ok", false)) { std::cerr << report.dump() << '\n'; continue; }
        const auto& data = report.at("data");
        const auto& ins = data.at("instructions");
        const auto& ops = want.at("ops");
        bool same = ins.size() == ops.size();
        for (size_t i = 0; same && i < ins.size(); ++i) same = ins[i].at("offset") == ops[i][0] && ins[i].at("op") == ops[i][1];
        t.Check(same, name + " opcodes match pickletools");
        json imports = json::array();
        for (const auto& imp : data.at("imports")) imports.push_back({imp.at("module"), imp.at("name")});
        t.Check(imports == want.at("imports"), name + " imports match the recording Unpickler");
        t.Check(data.at("executed") == false, name + " never executed");
    }
    t.Check(t.Inspect("data/evil.pkl").at("data").at("dangerous_imports") == true, "os.system pickle flagged dangerous");
    t.Check(t.Inspect("data/benign.pkl").at("data").at("dangerous_imports") == false, "plain data pickle not flagged");
    auto pt = t.File("data/model.pt");
    std::string error;
    auto inner = DefaultRegistry().OpenChild(pt, "model/data.pkl", error);
    t.Check(inner != nullptr, "PyTorch checkpoint lists its pickle");
    if (inner) {
        const auto report = DefaultRegistry().Inspect(*inner);
        bool rebuild = false;
        for (const auto& imp : report.at("data").at("imports")) rebuild = rebuild || imp.at("assessment") == "tensor_rebuild";
        t.Check(rebuild && report.at("data").at("persistent_ids") == 1, "PyTorch pickle: tensor rebuild and one storage id");
    }
}

void Npy(Tests& t) {
    const auto expected = t.Expected("data/expected.json").at("npy");
    for (const auto& [name, want] : expected.items()) {
        const auto report = t.Inspect("data/" + name);
        t.Check(report.value("ok", false) && report.value("format", "") == "npy", name + " detected");
        const auto& data = report.at("data");
        t.Check(data.at("shape") == want.at("shape"), name + " shape");
        t.Check(data.at("dtype") == want.at("descr"), name + " dtype");
        t.Check(data.at("fortran_order") == want.at("fortran"), name + " order");
        if (data.contains("size_matches")) t.Check(data.at("size_matches") == true, name + " data size");
    }
    const auto object = t.Inspect("data/object.npy").at("data");
    t.Check(object.at("object_array") == true && object.at("pickle").at("executed") == false, "object array pickle disassembled, not loaded");
    auto npz = t.File("data/arrays.npz");
    std::string error;
    auto first = DefaultRegistry().OpenChild(npz, "first.npy", error);
    t.Check(first && DefaultRegistry().Inspect(*first).at("data").at("shape") == json::array({3, 4}), "npz member opens as npy");
}

void Safetensors(Tests& t) {
    const auto expected = t.Expected("data/expected.json").at("safetensors");
    const auto report = t.Inspect("data/weights.safetensors");
    t.Check(report.value("ok", false) && report.value("format", "") == "safetensors", "safetensors detected");
    const auto& data = report.at("data");
    t.Check(data.at("metadata") == expected.at("metadata"), "safetensors metadata");
    t.Check(data.at("tensor_count") == expected.at("tensors").size(), "safetensors tensor count");
    for (const auto& tensor : data.at("tensors")) {
        const auto& want = expected.at("tensors").at(tensor.at("name").get<std::string>());
        t.Check(tensor.at("dtype") == want.at("dtype") && tensor.at("shape") == want.at("shape"), "safetensors " + tensor.at("name").get<std::string>());
    }
    t.Truncated("data/weights.safetensors", 120);
}

void Gguf(Tests& t) {
    const auto expected = t.Expected("data/expected.json").at("gguf");
    const auto report = t.Inspect("data/model.gguf");
    t.Check(report.value("ok", false) && report.value("format", "") == "gguf", "gguf detected");
    const auto& data = report.at("data");
    t.Check(data.at("metadata").at("cortex.answer") == expected.at("answer"), "gguf uint32 value");
    t.Check(data.at("metadata").at("general.name") == expected.at("name"), "gguf string value");
    t.Check(data.at("metadata").at("cortex.owned") == true, "gguf bool value");
    t.Check(data.at("metadata").at("tokenizer.ggml.tokens").at("count") == 3, "gguf array summary");
    for (const auto& tensor : data.at("tensors")) {
        const auto& want = expected.at("tensors").at(tensor.at("name").get<std::string>());
        t.Check(tensor.at("shape") == want.at("shape") && tensor.at("type") == want.at("type"), "gguf tensor " + tensor.at("name").get<std::string>() + " matches GGUFReader");
    }
    t.Truncated("data/model.gguf", 200);
}

void Onnx(Tests& t) {
    const auto expected = t.Expected("data/expected.json").at("onnx");
    const auto report = t.Inspect("data/model.onnx");
    t.Check(report.value("ok", false) && report.value("format", "") == "onnx", "onnx detected");
    const auto& data = report.at("data");
    t.Check(data.at("ir_version") == expected.at("ir_version") && data.at("producer_name") == expected.at("producer"), "onnx model fields");
    t.Check(data.at("opset_import").at(0).at("version") == expected.at("opset").at(0).at(1), "onnx opset");
    t.Check(data.at("metadata") == expected.at("metadata"), "onnx metadata");
    const auto& graph = data.at("graph");
    t.Check(graph.at("name") == expected.at("graph"), "onnx graph name");
    std::vector<std::string> ops;
    for (const auto& n : graph.at("nodes")) ops.push_back(n.at("op_type"));
    t.Check(json(ops) == expected.at("ops"), "onnx node op types match onnx.load");
    t.Check(graph.at("inputs") == expected.at("inputs") && graph.at("outputs") == expected.at("outputs"), "onnx inputs and outputs");
    json inits = json::array();
    for (const auto& i : graph.at("initializers")) inits.push_back({i.at("name"), i.at("dims")});
    t.Check(inits == expected.at("initializers"), "onnx initializers");
}


// ------------------------------------------------- captures and firmware ---

void Captures(Tests& t) {
    const auto all = t.Expected("media/expected.json");
    for (const char* name : {"pcap", "pcapng"}) {
        const auto& want = all.at(name);
        const auto report = t.Inspect(std::string("media/sample.") + name);
        t.Check(report.value("ok", false) && report.value("format", "") == name, std::string(name) + " detected");
        if (!report.value("ok", false)) { std::cerr << report.dump() << '\n'; continue; }
        const auto& data = report.at("data");
        t.Check(data.at("packets") == want.at("packets"), std::string(name) + " packet count matches dpkt");
        for (const auto& [proto, count] : want.at("protocols").items())
            t.Check(data.at("protocols").value(proto, 0) == count, std::string(name) + " " + proto + " count");
        json flows = json::array();
        for (const auto& f : data.at("flows")) flows.push_back({f.at("protocol"), f.at("source_port"), f.at("destination_port")});
        std::sort(flows.begin(), flows.end());
        t.Check(flows == want.at("flows"), std::string(name) + " flows match dpkt");
        t.Check(data.at("tls_server_names") == want.at("sni"), std::string(name) + " TLS SNI from a real ClientHello");
        t.Check(data.at("dns_queries") == want.at("dns"), std::string(name) + " DNS question");
        t.Check(data.at("http_requests") == want.at("http"), std::string(name) + " HTTP request line");
        t.Check(std::abs(data.at("first_timestamp").get<double>() - want.at("first").get<double>()) < 1e-6 &&
                std::abs(data.at("last_timestamp").get<double>() - want.at("last").get<double>()) < 1e-6, std::string(name) + " timestamps");
    }
    t.Truncated("media/sample.pcapng", 20);
}

void Firmware(Tests& t) {
    const auto all = t.Expected("media/expected.json");
    auto rangesOf = [](const json& data) {
        json out = json::array();
        for (const auto& r : data.at("ranges")) out.push_back({std::stoull(r.at("start").get<std::string>(), nullptr, 16), std::stoull(r.at("end").get<std::string>(), nullptr, 16)});
        return out;
    };
    const auto hex = t.Inspect("media/image.hex");
    t.Check(hex.value("format", "") == "ihex", "Intel HEX detected");
    t.Check(rangesOf(hex.at("data")) == all.at("ihex").at("segments"), "Intel HEX segments match intelhex");
    t.Check(hex.at("data").at("start_address") == HexU64(all.at("ihex").at("start")), "Intel HEX start address");
    t.Check(hex.at("data").at("checksum_errors") == 0 && hex.at("data").at("data_bytes") == all.at("ihex").at("bytes"), "Intel HEX checksums and bytes");
    const auto srec = t.Inspect("media/image.srec");
    t.Check(srec.value("format", "") == "srec", "S-record detected");
    t.Check(rangesOf(srec.at("data")) == all.at("srec").at("segments"), "S-record segments match bincopy");
    t.Check(srec.at("data").at("header") == all.at("srec").at("header") && srec.at("data").at("start_address") == HexU64(all.at("srec").at("start")), "S-record header and entry");
    t.Check(srec.at("data").at("checksum_errors") == 0, "S-record checksums");
    const auto uf2 = t.Inspect("media/image.uf2");
    t.Check(uf2.value("format", "") == "uf2", "UF2 detected");
    t.Check(uf2.at("data").at("families").at(0).at("name") == "RP2040" && uf2.at("data").at("data_bytes") == all.at("uf2").at("bytes"), "UF2 family and bytes");
    const auto dtb = t.Inspect("media/board.dtb");
    t.Check(dtb.value("format", "") == "dtb", "DTB detected");
    const auto& d = dtb.at("data");
    std::vector<std::string> paths;
    for (const auto& n : d.at("nodes")) paths.push_back(n.at("path"));
    std::sort(paths.begin(), paths.end());
    t.Check(json(paths) == all.at("dtb").at("paths"), "DTB node paths match fdt");
    t.Check(d.at("model") == all.at("dtb").at("model") && d.at("compatible") == all.at("dtb").at("compatible"), "DTB model and compatible");
    for (const auto& n : d.at("nodes")) {
        if (n.at("path") == "/memory@80000000") t.Check(n.at("properties").at("reg").at("cells") == all.at("dtb").at("memory_reg"), "DTB memory reg cells");
        if (n.at("path") == "/chosen") t.Check(n.at("properties").at("bootargs") == all.at("dtb").at("bootargs"), "DTB bootargs");
    }
    t.Truncated("media/board.dtb", 60);
}

void Documents(Tests& t) {
    const auto all = t.Expected("media/expected.json");
    const auto& want = all.at("pdf");
    const auto pdf = t.Inspect("media/sample.pdf");
    t.Check(pdf.value("format", "") == "pdf", "PDF detected");
    const auto& p = pdf.at("data");
    t.Check(p.at("pages") == want.at("pages"), "PDF pages match pypdf");
    t.Check(p.at("info").value("Title", "") == want.at("title") && p.at("info").value("Author", "") == want.at("author"), "PDF info");
    t.Check(p.at("embedded_files") == want.at("attachments"), "PDF attachments match pypdf");
    t.Check(p.at("uris") == want.at("uris"), "PDF URI actions match pypdf");
    bool script = false;
    for (const auto& j : p.at("javascript")) script = script || j.value("script", "").find("cortex fixture") != std::string::npos;
    t.Check(script && p.at("open_action") == true && p.at("executed") == false, "PDF JavaScript listed, open action flagged, nothing run");
    const auto enc = t.Inspect("media/encrypted.pdf");
    t.Check(enc.at("data").at("encrypted") == want.at("encrypted") && enc.at("data").at("info").value("Title", "") == "(encrypted)", "encrypted PDF flagged, strings left encrypted");
    const auto& lw = all.at("lnk");
    const auto lnk = t.Inspect("media/target.lnk");
    t.Check(lnk.value("format", "") == "lnk", "LNK detected");
    const auto& l = lnk.at("data");
    t.Check(l.at("flags") == lw.at("flags"), "LNK flags match LnkParse3");
    t.Check(l.at("strings").value("name", "") == lw.at("data").at("description") &&
            l.at("strings").value("arguments", "") == lw.at("data").at("command_line_arguments") &&
            l.at("strings").value("working_dir", "") == lw.at("data").at("working_directory") &&
            l.at("strings").value("icon_location", "") == lw.at("data").at("icon_location"), "LNK strings match LnkParse3");
    t.Check(l.at("target_path") == lw.at("target_path"), "LNK target path from shell items");
    t.Check(l.at("icon_index") == lw.at("icon_index") && l.at("show_command") == "maximized", "LNK icon and window");
    t.Truncated("media/target.lnk", 90);
}

void Spirv(Tests& t) {
    const auto want = t.Expected("media/expected.json").at("spirv");
    for (const char* stage : {"compute", "fragment"}) {
        const auto report = t.Inspect(std::string("media/") + stage + ".spv");
        t.Check(report.value("ok", false) && report.value("format", "") == "spirv", std::string(stage) + " SPIR-V detected");
        if (!report.value("ok", false)) continue;
        const auto& data = report.at("data");
        const auto& entry = data.at("entry_points").at(0);
        t.Check(entry.at("name") == "main" && entry.at("execution_model") == want.at(stage).at("model"), std::string(stage) + " entry point");
        bool shader = false;
        for (const auto& c : data.at("capabilities")) shader = shader || c == "Shader";
        t.Check(shader, std::string(stage) + " Shader capability");
        for (const auto& n : want.at(stage).at("names")) {
            bool found = false;
            for (const auto& d : data.at("debug_names")) found = found || d.at("name") == n;
            for (const auto& d : data.at("member_names")) found = found || d.at("name") == n;
            t.Check(found, std::string(stage) + " debug name " + n.get<std::string>());
        }
        if (std::string(stage) == "compute") {
            const auto& mode = entry.at("execution_modes").at(0);
            t.Check(mode.at("mode") == "LocalSize" && json({mode.at("x"), mode.at("y"), mode.at("z")}) == want.at(stage).at("local_size"), "compute local size");
        }
    }
    t.Truncated("media/compute.spv", 64);
}


// ---------------------------------------------------------------- games ---

void Games(Tests& t) {
    const auto all = t.Expected("games/expected.json");
    const std::map<std::string, std::string> content = {{"scripts/config.txt", "owned = true\n"}, {"readme.md", "# cortex fixture\n"}};
    struct Case { const char* file; const char* format; const char* key; };
    for (const Case c : {Case{"games/pak01_dir.vpk", "vpk", "vpk"}, Case{"games/quake.pak", "id-pak", "pak"}, Case{"games/game.rpa", "rpa", "rpa"}}) {
        auto file = t.File(c.file);
        const auto report = DefaultRegistry().Inspect(*file);
        t.Check(report.value("ok", false) && report.value("format", "") == c.format, std::string(c.file) + " detected");
        if (!report.value("ok", false)) { std::cerr << report.dump() << '\n'; continue; }
        const auto& want = all.at(c.key);
        const json& wantFiles = c.key == std::string("rpa") ? want.at("files") : want;
        t.Check(report.at("data").at("files").size() == wantFiles.size(), std::string(c.file) + " entry count matches the reference");
        for (const auto& f : report.at("data").at("files")) {
            const std::string name = f.at("name");
            const auto& w = wantFiles.at(name);
            const uint64_t size = w.is_object() ? (w.contains("size") ? w.at("size").get<uint64_t>() : w.at("length").get<uint64_t>()) : w.get<uint64_t>();
            t.Check(f.at("size") == size, std::string(c.file) + " size of " + name);
            if (c.key == std::string("vpk")) t.Check(f.at("crc32") == w.at("crc"), std::string(c.file) + " CRC of " + name);
        }
        for (const auto& [name, text] : content) {
            std::string error;
            auto child = DefaultRegistry().OpenChild(file, name, error);
            const auto bytes = child ? child->ReadSome(0, 4096) : std::vector<uint8_t>();
            t.Check(child && std::string(bytes.begin(), bytes.end()) == text, std::string(c.file) + " opens " + name + " " + error);
        }
    }
    const auto wad = t.Inspect("games/doom.wad");
    t.Check(wad.value("format", "") == "wad" && wad.at("data").at("type") == "PWAD", "WAD detected");
    json lumps = json::array();
    for (const auto& f : wad.at("data").at("files")) lumps.push_back({f.at("name"), f.at("size")});
    t.Check(lumps == all.at("wad").at("lumps"), "WAD lumps match omgifol");
    t.Truncated("games/game.rpa", 40);
}

} // namespace

int main(int argc, char** argv) {
    Tests t;
    t.root = argc > 1 ? argv[1] : "tests/fixtures";
    t.Run("python", [&] { Python(t); });
    t.Run("java", [&] { Java(t); });
    t.Run("lua", [&] { Lua(t); });
    t.Run("wasm", [&] { Wasm(t); });
    t.Run("macho", [&] { MachO(t); });
    t.Run("coff", [&] { Coff(t); });
    t.Run("go", [&] { Go(t); });
    t.Run("sqlite", [&] { Sqlite(t); });
    t.Run("bplist", [&] { Plist(t); });
    t.Run("pickle", [&] { Pickle(t); });
    t.Run("npy", [&] { Npy(t); });
    t.Run("safetensors", [&] { Safetensors(t); });
    t.Run("gguf", [&] { Gguf(t); });
    t.Run("onnx", [&] { Onnx(t); });
    t.Run("captures", [&] { Captures(t); });
    t.Run("firmware", [&] { Firmware(t); });
    t.Run("documents", [&] { Documents(t); });
    t.Run("spirv", [&] { Spirv(t); });
    t.Run("games", [&] { Games(t); });
    std::cout << t.checks << " checks, " << t.failures << " failures\n";
    return t.failures ? 1 : 0;
}
