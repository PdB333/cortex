// The JSON records Cortex keeps on disk (evidence, investigations) and the
// knowledge records clients send: a file can be edited or replaced, and a
// client sends whatever it wants. The fuzzed bytes become a record file in a
// store, then the store's read paths run on it (list, get, export, start,
// approve); for knowledge, Validate and Prepare (with a fuzzed previous
// revision too). Nothing may crash or throw out of the store.
#include "evidence/evidence.h"
#include "investigation/investigation.h"
#include "project/knowledge.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const fs::path& Directory() {
    static const fs::path directory = [] {
        fs::path base = fs::temp_directory_path() / ("cortex-fuzz-stores-" + std::to_string(::getpid()));
        fs::create_directories(base / "evidence");
        fs::create_directories(base / "investigations");
        return base;
    }();
    return directory;
}

void Write(const fs::path& path, const uint8_t* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const uint8_t mode = data[0] % 3;
    const uint8_t* body = data + 1;
    const size_t length = size - 1;
    try {
        if (mode == 0) {
            const std::string id = "ev_00000000000000000000";
            const fs::path file = Directory() / "evidence" / (id + ".json");
            Write(file, body, length);
            cortex::evidence::Store store(Directory() / "evidence");
            (void)store.Get(id).dump();
            (void)store.List(json::object(), 20).dump();
            (void)store.Export(json::object(), "json").dump();
            (void)store.Export(json::object(), "markdown").dump();
            fs::remove(file);
        } else if (mode == 1) {
            const std::string id = "inv_00000000000000000000";
            const fs::path file = Directory() / "investigations" / (id + ".json");
            Write(file, body, length);
            cortex::investigation::Store store(Directory() / "investigations");
            (void)store.Get(id).dump();
            (void)store.List("", 20).dump();
            (void)store.List("running", 20).dump();
            (void)store.Start(id, 1, "desktop").dump();
            (void)store.Approve(id, 0, true, "", "desktop").dump();
            fs::remove(file);
        } else {
            // "<previous>\n<input>": both are untrusted.
            const std::string text(reinterpret_cast<const char*>(body), length);
            const size_t split = text.find('\n');
            const json previous = json::parse(text.substr(0, split), nullptr, false);
            const json input = json::parse(split == std::string::npos ? std::string() : text.substr(split + 1),
                                           nullptr, false);
            std::string error;
            json out;
            (void)project::knowledge::Validate(input, error);
            (void)project::knowledge::Prepare(input, nullptr, out, error);
            if (previous.is_object()) (void)project::knowledge::Prepare(input, &previous, out, error);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "exception escaped a store (mode %d): %s\n", mode, e.what());
        std::abort();
    }
    return 0;
}
