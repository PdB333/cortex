// Unit tests for the desktop Lua engine (Cheat Engine style API) and Cheat
// Engine address expressions, against an in-memory fake process.

#include "application/address_expression.h"
#include "application/lua_engine.h"
#include "fake_process.h"

#include <atomic>
#include <memory>
#include <iostream>
#include <string>
#include <vector>

namespace {

using cortex::application::AddressResolver;
using cortex::application::EvaluateAddress;
using cortex::application::LuaRunOptions;
using cortex::application::RunDesktopLua;
using cortex::target::MemoryRegionType;
using cortex::tests::FakeProcess;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

void TestExpressions() {
    AddressResolver resolver;
    resolver.symbol = [](const std::string& name, uint64_t& value) {
        if (name == "game.exe" || name == "Tutorial-i386.exe") {
            value = 0x400000;
            return true;
        }
        return false;
    };
    resolver.readPointer = [](uint64_t address, uint64_t& value) {
        if (address == 0x400010) value = 0x10000000;
        else if (address == 0x10000020) value = 0x20000000;
        else return false;
        return true;
    };
    uint64_t value = 0;
    std::string error;
    CHECK(EvaluateAddress("7FF6A1B20010", resolver, value) && value == 0x7FF6A1B20010ull);
    CHECK(EvaluateAddress("0x10", resolver, value) && value == 0x10);
    CHECK(EvaluateAddress("game.exe+1A", resolver, value) && value == 0x40001A);
    CHECK(EvaluateAddress("\"Tutorial-i386.exe\"+10", resolver, value) && value == 0x400010);
    CHECK(EvaluateAddress("[game.exe+10]", resolver, value) && value == 0x10000000);
    CHECK(EvaluateAddress("[[game.exe+10]+20]+8", resolver, value) && value == 0x20000008);
    CHECK(EvaluateAddress("game.exe + 4*2 - 1", resolver, value) && value == 0x400007);
    CHECK(EvaluateAddress("(10+6)*2", resolver, value) && value == 0x2C);
    CHECK(EvaluateAddress("[game.exe+10]-10", resolver, value) && value == 0x0FFFFFF0);
    CHECK(!EvaluateAddress("player+4", resolver, value, &error) && error.find("player") != std::string::npos);
    CHECK(!EvaluateAddress("[game.exe+14]", resolver, value, &error) && error.find("pointer") != std::string::npos);
    CHECK(!EvaluateAddress("[game.exe+10", resolver, value, &error));
    CHECK(!EvaluateAddress("game.exe+", resolver, value, &error));
    CHECK(!EvaluateAddress("", resolver, value, &error));
}

struct Harness {
    std::shared_ptr<FakeProcess> process = std::make_shared<FakeProcess>();
    std::vector<std::string> lines;
    LuaRunOptions options;

    Harness() {
        process->Add(0x400000, 0x1000, true, false, MemoryRegionType::Image);
        process->Add(0x10000000, 0x1000);
        process->Put<uint64_t>(0x400100, 0x10000000);
        process->Put<int32_t>(0x10000020, 100);
        process->Put<float>(0x10000024, 2.5f);
        const char name[] = "Hero";
        process->Write(0x10000030, name, sizeof(name));
        const uint8_t pattern[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
        process->Write(0x400200, pattern, sizeof(pattern));
        options.session = process;
        options.timeoutMs = 2000;
        options.output = [this](const std::string& line) { lines.push_back(line); };
        options.modules = [] {
            std::vector<cortex::target::ModuleInfo> modules(1);
            modules[0].name = "game.exe";
            modules[0].path = "C:\\\\game\\\\game.exe";
            modules[0].base = 0x400000;
            modules[0].size = 0x1000;
            return modules;
        };
        options.exports = [](const cortex::target::ModuleInfo& module) {
            std::vector<cortex::application::TargetSymbols::ExportEntry> exports;
            if (module.name == "game.exe") exports.push_back({"PlayerBase", module.base + 0x100});
            return exports;
        };
        options.userSymbols = std::make_shared<cortex::application::UserSymbols>();
    }

    cortex::application::LuaRunResult Run(const std::string& source) {
        lines.clear();
        auto result = RunDesktopLua(source, options);
        if (!result.ok && result.error.find("expected") != std::string::npos) std::cerr << result.error << std::endl;
        return result;
    }
};

void TestReads() {
    Harness h;
    auto result = h.Run(R"(
        local base = readPointer("game.exe+100")
        print(string.format("%X", base))
        print(readInteger(base + 0x20), readFloat("[game.exe+100]+24"), readString("[game.exe+100]+30"))
        return readInteger("[game.exe+100]+20") * 2, readBytes("game.exe+200", 2)
    )");
    CHECK(result.ok);
    CHECK(h.lines.size() == 2 && h.lines[0] == "10000000");
    CHECK(h.lines.size() == 2 && h.lines[1] == "100\t2.5\tHero");
    CHECK(result.returned == "200\t222\t173");

    result = h.Run("return readInteger(0x12345678)");
    CHECK(result.ok && result.returned == "nil");
    result = h.Run("return readBytes('game.exe+200', 4, true)[4]");
    CHECK(result.ok && result.returned == "239");
    result = h.Run("return readSmallInteger('game.exe+200', true), readByte('game.exe+200')");
    CHECK(result.ok && result.returned == "-21026\t222");
}

void TestWrites() {
    Harness h;
    auto result = h.Run("writeInteger('[game.exe+100]+20', 5)");
    CHECK(!result.ok && result.error.find("Writes allowed") != std::string::npos);
    int32_t value = 0;
    h.process->ReadMemory(0x10000020, &value, 4, nullptr);
    CHECK(value == 100);

    h.options.allowWrites = true;
    result = h.Run(R"(
        writeInteger("[game.exe+100]+20", 999)
        writeFloat(0x10000024, 7.25)
        writeString(0x10000030, "Zed")
        writeBytes("game.exe+200", 0x90, 0x90)
        writeBytes("game.exe+202", {1, 2})
        return readInteger(0x10000020), readFloat(0x10000024), readString(0x10000030), readBytes("game.exe+200", 4)
    )");
    CHECK(result.ok);
    CHECK(result.returned == "999\t7.25\tZedo\t144\t144\t1\t2");
}

void TestScansAndModules() {
    Harness h;
    auto result = h.Run(R"(
        local results = AOBScan("DE AD ?? EF")
        local text = results.Count .. " " .. results[0] .. " " .. results.getString(results, 0)
        results.destroy()
        return text, AOBScanUnique("BE EF 42"), AOBScanModuleUnique("game.exe", "AD BE"), AOBScan("11 22 33 44 55")
    )");
    CHECK(result.ok);
    CHECK(result.returned == "1 400200 400200\t4194818\t4194817\tnil");

    result = h.Run(R"(
        local modules = enumModules()
        return #modules, modules[1].Name, modules[1].Address, getModuleSize("game.exe"), targetIs64Bit()
    )");
    CHECK(result.ok && result.returned == "1\tgame.exe\t4194304\t4096\ttrue");

    result = h.Run("return getAddressSafe('nothing+1'), getAddress('game.exe+10')");
    CHECK(result.ok && result.returned == "nil\t4194320");
    result = h.Run("return getAddress('nothing+1')");
    CHECK(!result.ok && result.error.find("Unknown module or symbol: nothing") != std::string::npos);
}

void TestSymbols() {
    Harness h;
    auto result = h.Run(R"(
        return readInteger("[PlayerBase]+20"), readInteger("[game.exe.PlayerBase]+20"),
               readInteger("[game!playerbase]+20"), getAddress("game") == getAddress("game.exe")
    )");
    CHECK(result.ok && result.returned == "100\t100\t100\ttrue");

    result = h.Run(R"(
        registerSymbol("hero", "[PlayerBase]+20")
        local value = readInteger("hero")
        writeInteger("hero+4", 1)
    )");
    CHECK(!result.ok && result.error.find("Writes allowed") != std::string::npos);
    uint64_t hero = 0;
    CHECK(h.options.userSymbols->Find("HERO", hero) && hero == 0x10000020);
    result = h.Run("return readInteger('hero'), getNameFromAddress('PlayerBase'), getNameFromAddress(0x10000020)");
    CHECK(result.ok && result.returned == "100\tgame.exe+100\t10000020");
    result = h.Run("return inModule('game.exe+10'), inModule(0x10000000), inSystemModule('game.exe')");
    CHECK(result.ok && result.returned == "true\tfalse\tfalse");
    result = h.Run("unregisterSymbol('hero') return getAddressSafe('hero')");
    CHECK(result.ok && result.returned == "nil");
    result = h.Run("registerSymbol('bad name', 1)");
    CHECK(!result.ok && result.error.find("invalid symbol name") != std::string::npos);
    result = h.Run("return readShortInteger('[PlayerBase]+20')");
    CHECK(result.ok && result.returned == "100");

    // The same names work in address expressions outside Lua.
    cortex::application::TargetSymbols symbols;
    symbols.SetModules(h.options.modules());
    symbols.SetExportReader(h.options.exports);
    symbols.SetUserSymbols(h.options.userSymbols);
    h.options.userSymbols->Set("Player", 0x1234);
    uint64_t value = 0;
    CHECK(symbols.Resolve("player", value) && value == 0x1234);
    CHECK(symbols.Resolve("GAME.EXE", value) && value == 0x400000);
    CHECK(symbols.Resolve("game.exe.PlayerBase", value) && value == 0x400100);
    CHECK(!symbols.Resolve("game.exe.Missing", value));
    CHECK(!symbols.Resolve("other!PlayerBase", value));
    const auto found = symbols.Search("PLAYER", 10);
    CHECK(found.size() == 1 && found[0].first == "game.exe.PlayerBase" && found[0].second == 0x400100);
    CHECK(symbols.Search("nothing", 10).empty());

    // Forwarded exports lead to the module they name.
    cortex::application::TargetSymbols forwarded;
    std::vector<cortex::target::ModuleInfo> modules(2);
    modules[0].name = "KERNEL32.DLL";
    modules[0].base = 0x70000000;
    modules[1].name = "ntdll.dll";
    modules[1].base = 0x77000000;
    forwarded.SetModules(modules);
    forwarded.SetExportReader([](const cortex::target::ModuleInfo& module) {
        std::vector<cortex::application::TargetSymbols::ExportEntry> list;
        if (module.name == "ntdll.dll") list.push_back({"RtlAllocateHeap", 0x77001000, ""});
        else list.push_back({"HeapAlloc", 0, "NTDLL.RtlAllocateHeap"}), list.push_back({"Loop", 0, "KERNEL32.Loop"});
        return list;
    });
    CHECK(forwarded.Resolve("kernel32.HeapAlloc", value) && value == 0x77001000);
    CHECK(forwarded.Resolve("HeapAlloc", value) && value == 0x77001000);
    CHECK(!forwarded.Resolve("kernel32.Loop", value));
    const auto heap = forwarded.Search("heapalloc", 10);
    CHECK(heap.size() == 1 && heap[0].first == "KERNEL32.DLL.HeapAlloc" && heap[0].second == 0x77001000);
}

void TestConversionsAndLimits() {
    Harness h;
    auto result = h.Run(R"(
        local t = dwordToByteTable(0x11223344)
        return t[1], byteTableToDword(t), byteTableToString(stringToByteTable("ok")), byteTableToFloat(floatToByteTable(1.5))
    )");
    CHECK(result.ok && result.returned == "68\t287454020\tok\t1.5");

    h.options.timeoutMs = 150;
    result = h.Run("while true do end");
    CHECK(!result.ok && result.error.find("time limit") != std::string::npos);

    std::atomic_bool cancelled{true};
    h.options.cancelled = &cancelled;
    h.options.timeoutMs = 0;
    result = h.Run("sleep(5000)");
    CHECK(!result.ok && result.error.find("stopped") != std::string::npos);
    h.options.cancelled = nullptr;

    result = h.Run("return io == nil, os == nil, dofile == nil, load ~= nil");
    CHECK(result.ok && result.returned == "true\ttrue\ttrue\ttrue");
    result = h.Run("syntax error here");
    CHECK(!result.ok && !result.error.empty());
    result = h.Run("error('custom failure')");
    CHECK(!result.ok && result.error.find("custom failure") != std::string::npos);
}

} // namespace

int main() {
    TestExpressions();
    TestReads();
    TestWrites();
    TestScansAndModules();
    TestSymbols();
    TestConversionsAndLimits();
    if (failures) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "lua engine tests passed" << std::endl;
    return 0;
}
