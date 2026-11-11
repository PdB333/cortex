#pragma once
#include "artifacts/artifact.h"
#include "artifacts/elf_provider.h"
#include <algorithm>
#include <iostream>
#include <random>

namespace elf_tests {
using namespace cortex::artifacts;
inline int Run(const std::string& fixtures) {
    int failed = 0;
    auto check = [&](bool ok, const char* label) { if (!ok) { ++failed; std::cerr << "ELF: " << label << '\n'; } };
    for (const char* filename : {"elf32-le.elf","elf32-be.elf","elf64-le.elf","elf64-be.elf","elf64-extended.elf"}) {
        try {
            const std::string name(filename);
            auto source = OpenFile(fixtures + "/elf-native/" + name);
            check(bool(source), "fixture exists"); if (!source) continue;
            const json report = DefaultRegistry().Inspect(*source);
            check(report.value("ok", false) && report.value("format", "") == "elf", "ELF detection and parsing");
            if (!report.value("ok", false)) { std::cerr << report.dump() << '\n'; continue; }
            const auto& d = report.at("data");
            check(d.at("class") == (name.find("64") != std::string::npos ? 64 : 32), "class decoded");
            check(d.at("byte_order") == (name.find("-be") != std::string::npos ? "big" : "little"), "byte order decoded");
            check(d.at("section_count") == 8 && d.at("segment_count") == 4, "normal and extended counts");
            check(d.at("symbols").size() == 3 && d.at("relocations").size() == 1, "symbols and relocations");
            check(d.at("symbols").at(1).at("name") == "target" && d.at("symbols").at(1).at("value") == "0x400500", "symbol name/value");
            check(d.at("relocations").at(0).at("symbol") == "target", "relocation symbol resolution");
            if (d.at("class") == 64) check(d.at("relocations").at(0).at("addend") == -4, "signed ELF64 addend");
            check(d.at("needed") == json::array({"libfixture.so"}), "dynamic string and DT_NEEDED");
            check(d.at("notes").at(0).at("build_id") == "0123456789abcdef", "GNU build ID");
            check(d.at("hardening").at("nx_stack") == true && d.at("hardening").at("relro") == "full_metadata", "NX/RELRO metadata");
            check(d.at("hardening").at("pie") == "indicated" && d.at("hardening").at("stack_canary_reference") == true, "PIE/canary indicators");
            check(!d.contains("warnings"), "valid fixture has no warning");
            InspectOptions small; small.maxEntries = 1;
            const auto limited = DefaultRegistry().Inspect(*source, small).at("data");
            check(limited.at("symbols").size() <= 1 && limited.at("sections").size() <= 1 && limited.at("truncated") == true, "bounded output");
            small.maxEntries = 0;
            const auto zero = DefaultRegistry().Inspect(*source, small).at("data");
            check(zero.at("symbols").empty() && zero.at("sections").empty(), "zero output limit");
            const auto bytes = source->ReadSome(0, static_cast<size_t>(source->Size()));
            auto bad = bytes; bad[5] = 0;
            check(DefaultRegistry().Inspect(*FromBytes(bad,"bad.elf")).value("error", "") == "elf_byte_order_unsupported", "bad byte order rejected");
            for (size_t n = 0; n < bytes.size(); n += 47)
                (void)DefaultRegistry().Inspect(*FromBytes(std::vector<uint8_t>(bytes.begin(), bytes.begin()+n),"cut.elf")).dump();
            std::mt19937 rng(0x454C46u);
            for (unsigned n = 0; n < 250; ++n) {
                auto mutated = bytes;
                for (unsigned j = 0; j < 4; ++j) mutated[rng() % mutated.size()] = static_cast<uint8_t>(rng());
                (void)DefaultRegistry().Inspect(*FromBytes(std::move(mutated), "mutation.elf")).dump();
            }
        } catch (const std::exception& ex) { ++failed; std::cerr << "ELF exception " << filename << ": " << ex.what() << '\n'; }
    }
    try {
        auto real = OpenFile(fixtures + "/elf/actor.elf");
        check(bool(real), "GCC PIE fixture exists");
        if (real) {
            const auto report = DefaultRegistry().Inspect(*real);
            check(report.value("ok", false) && report.value("format", "") == "elf", "native reader accepts existing GCC fixture");
            const auto& d = report.at("data");
            bool damage = false, add = false;
            for (const auto& symbol : d.at("symbols")) {
                damage |= symbol.value("name", "") == "CortexDemoApplyDamage";
                add |= symbol.value("name", "") == "CortexNativeAdd";
            }
            check(damage && add, "known real source functions found");
            check(d.at("hardening").at("pie") == "indicated", "GCC PIE detected");
            check(!d.at("needed").empty(), "real dynamic dependencies parsed");
        }
    } catch (const std::exception& ex) { ++failed; std::cerr << "ELF real fixture: " << ex.what() << '\n'; }
    return failed;
}
} // namespace elf_tests
