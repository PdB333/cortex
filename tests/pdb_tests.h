#pragma once
#include "artifacts/artifact.h"
#include "artifacts/pdb_provider.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <random>

namespace pdb_tests {
using namespace cortex::artifacts;
inline int Run(const std::string& fixtures) {
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failed; std::cerr << "PDB: " << label << '\n'; }
    };
    for (const char* name : {"synthetic-512.pdb", "synthetic-4096.pdb"}) {
        try {
            auto source = OpenFile(fixtures + "/pdb/" + name);
            check(bool(source), "fixture exists"); if (!source) continue;
            const json report = DefaultRegistry().Inspect(*source);
            check(report.value("ok", false) && report.value("format", "") == "pdb", "registry selects PDB");
            if (!report.value("ok", false)) { std::cerr << report.dump(2) << '\n'; continue; }
            const json& d = report.at("data");
            check(d.at("info").at("guid") == "00112233-4455-6677-8899-aabbccddeeff", "GUID byte ordering");
            check(d.at("info").at("age") == 1, "PDB age");
            check(d.at("tpi").at("records").at(0).at("name") == "Point", "TPI struct name");
            check(d.at("tpi").at("records").at(0).at("size") == 8, "TPI struct size");
            check(!d.at("dbi").contains("error"), "DBI parses");
            check(d.at("dbi").at("modules").size() == 1, "module count");
            check(d.at("dbi").at("source_files").at(0) == "sample.cpp", "source file table");
            check(d.at("symbols").size() == 63, "public/global/private symbols");
            bool publicFound = false, privateFound = false;
            for (const auto& row : d.at("symbols")) {
                if (row.value("name", "") != "CortexPdbAdd") continue;
                check(row.value("rva", "") == "0x1010", "section offset to RVA");
                publicFound |= row.value("tag", "") == "public";
                privateFound |= row.value("tag", "") == "procedure";
            }
            check(publicFound && privateFound, "public and private records decoded");
            check(d.at("lines").size() == 2, "C13 line count");
            check(d.at("lines").at(0).at("file") == "sample.cpp", "C13 checksum/string link");
            check(d.at("lines").at(0).at("line") == 4 && d.at("lines").at(1).at("rva") == "0x1017", "C13 source mapping");
            (void)report.dump();

            InspectOptions limit; limit.maxEntries = 2;
            const json small = DefaultRegistry().Inspect(*source, limit).at("data");
            check(small.at("symbols").size() <= 2 && small.at("streams").size() <= 2 && small.at("truncated") == true,
                  "output bounds are explicit");
            limit.maxEntries = 0;
            const json zero = DefaultRegistry().Inspect(*source, limit).at("data");
            check(zero.at("symbols").empty() && zero.at("lines").empty(), "zero output budget");
            const auto original = source->ReadSome(0, static_cast<size_t>(source->Size()));
            auto invalid = original;
            std::fill(invalid.begin() + 32, invalid.begin() + 36, 0);
            const json badBlock = DefaultRegistry().Inspect(*FromBytes(invalid, "bad.pdb"));
            check(!badBlock.value("ok", true) && badBlock.value("error", "") == "msf_unsupported_block_size", "reject zero block size");
            invalid = original;
            std::fill(invalid.begin() + 52, invalid.begin() + 56, 0xFF);
            check(!DefaultRegistry().Inspect(*FromBytes(invalid, "bad.pdb")).value("ok", true), "reject directory outside file");
            for (size_t size = 0; size < std::min<size_t>(original.size(), 8192); size += 113) {
                const auto prefix = FromBytes(std::vector<uint8_t>(original.begin(), original.begin() + size), "cut.pdb");
                (void)DefaultRegistry().Inspect(*prefix).dump();
            }
            std::mt19937 rng(0x504442u);
            for (unsigned trial = 0; trial < 250; ++trial) {
                auto bytes = original;
                for (unsigned n = 0; n < 4; ++n) bytes[rng() % bytes.size()] = static_cast<uint8_t>(rng());
                (void)DefaultRegistry().Inspect(*FromBytes(std::move(bytes), "mutated.pdb")).dump();
            }
        } catch (const std::exception& ex) { ++failed; std::cerr << "PDB exception: " << name << ": " << ex.what() << '\n'; }
    }
    try {
        auto real = OpenFile(fixtures + "/pdb/sample-x64.pdb");
        check(bool(real), "clang/lld fixture exists");
        if (real) {
            const json result = DefaultRegistry().Inspect(*real);
            check(result.value("ok", false), "compiler-produced PDB parses");
            const auto& d = result.at("data");
            check(d.at("dbi").at("machine") == "0x8664", "real PDB machine");
            bool proc = false, point = false;
            for (const auto& s : d.at("symbols"))
                proc |= s.value("tag", "") == "procedure" && s.value("name", "") == "CortexPdbAdd";
            for (const auto& t : d.at("tpi").at("records"))
                point |= t.value("name", "") == "Point" && t.value("size", 0) == 8;
            check(proc && point, "real procedure and struct from owned source");
            check(d.at("lines").size() == 2 && d.at("lines").at(0).at("line") == 4 &&
                  d.at("lines").at(1).at("line") == 5, "real C13 lines match source");
            for (const auto& module : d.at("dbi").at("modules")) check(!module.contains("error"), "real module has no parse error");
        }
    } catch (const std::exception& ex) { ++failed; std::cerr << "real PDB: " << ex.what() << '\n'; }
    return failed;
}
} // namespace pdb_tests
