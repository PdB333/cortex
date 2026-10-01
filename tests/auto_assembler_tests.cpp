// Unit tests for the Auto Assembler script interpreter against a fake
// process: a module with known bytes, allocation and an AOB scanner.

#include "services/auto_assembler.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using cortex::services::AutoAssembleHost;
using cortex::services::AutoAssembleOptions;
using cortex::services::AutoAssembleResult;
using cortex::services::RunAutoAssembler;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

constexpr uint64_t kModuleBase = 0x140000000ull;
constexpr uint64_t kModuleSize = 0x10000ull;
// The instruction the script injects at: mov [rbx+00000100],eax
const uint8_t kTargetCode[] = {0x89, 0x83, 0x00, 0x01, 0x00, 0x00};
constexpr uint64_t kTargetOffset = 0x1000;

struct FakeTarget {
    std::map<uint64_t, std::vector<uint8_t>> blocks;  // base -> bytes
    uint64_t nextAllocation = 0x140200000ull;
    std::vector<uint64_t> freed;
    std::vector<uint64_t> threads;

    FakeTarget() {
        std::vector<uint8_t> image(kModuleSize, 0xCC);
        std::memcpy(image.data() + kTargetOffset, kTargetCode, sizeof(kTargetCode));
        blocks[kModuleBase] = std::move(image);
    }

    std::vector<uint8_t>* Find(uint64_t address, size_t size, size_t& offset) {
        for (auto& block : blocks) {
            if (address < block.first || address + size > block.first + block.second.size()) continue;
            offset = static_cast<size_t>(address - block.first);
            return &block.second;
        }
        return nullptr;
    }

    AutoAssembleHost Host() {
        AutoAssembleHost host;
        host.read = [this](uint64_t address, void* buffer, size_t size) {
            size_t offset = 0;
            auto* block = Find(address, size, offset);
            if (!block) return false;
            std::memcpy(buffer, block->data() + offset, size);
            return true;
        };
        host.write = [this](uint64_t address, const void* buffer, size_t size) {
            size_t offset = 0;
            auto* block = Find(address, size, offset);
            if (!block) return false;
            std::memcpy(block->data() + offset, buffer, size);
            return true;
        };
        host.allocate = [this](size_t size, uint64_t, uint64_t& address, std::string&) {
            address = nextAllocation;
            nextAllocation += 0x10000;
            blocks[address] = std::vector<uint8_t>(size < 0x1000 ? 0x1000 : size, 0);
            return true;
        };
        host.release = [this](uint64_t address, std::string&) {
            freed.push_back(address);
            blocks.erase(address);
            return true;
        };
        host.fullAccess = [](uint64_t, size_t, std::string&) { return true; };
        host.createThread = [this](uint64_t address, std::string&) {
            threads.push_back(address);
            return true;
        };
        host.symbol = [](const std::string& name, uint64_t& value) {
            if (name == "game.exe") {
                value = kModuleBase;
                return true;
            }
            return false;
        };
        host.moduleRange = [](const std::string& module, uint64_t& base, uint64_t& size) {
            if (module != "game.exe") return false;
            base = kModuleBase;
            size = kModuleSize;
            return true;
        };
        host.scan = [this](const std::string& pattern, uint64_t start, uint64_t stop,
                           std::vector<uint64_t>& matches, std::string&) {
            std::vector<uint8_t> bytes;
            std::vector<bool> mask;
            std::string token;
            for (size_t i = 0; i <= pattern.size(); ++i) {
                if (i < pattern.size() && pattern[i] != ' ') {
                    token += pattern[i];
                    continue;
                }
                if (token.empty()) continue;
                if (token == "??" || token == "?") {
                    bytes.push_back(0);
                    mask.push_back(false);
                } else {
                    bytes.push_back(static_cast<uint8_t>(std::strtoul(token.c_str(), nullptr, 16)));
                    mask.push_back(true);
                }
                token.clear();
            }
            if (bytes.empty()) return false;
            for (const auto& block : blocks) {
                for (size_t i = 0; i + bytes.size() <= block.second.size(); ++i) {
                    const uint64_t address = block.first + i;
                    if (address < start || address >= stop) continue;
                    bool hit = true;
                    for (size_t j = 0; j < bytes.size() && hit; ++j)
                        hit = !mask[j] || block.second[i + j] == bytes[j];
                    if (hit) matches.push_back(address);
                }
            }
            return true;
        };
        return host;
    }

    uint32_t ReadU32(uint64_t address) {
        uint32_t value = 0;
        size_t offset = 0;
        auto* block = Find(address, 4, offset);
        if (block) std::memcpy(&value, block->data() + offset, 4);
        return value;
    }
    uint8_t ReadU8(uint64_t address) {
        size_t offset = 0;
        auto* block = Find(address, 1, offset);
        return block ? (*block)[offset] : 0;
    }
};

const char* const kScript = R"(
[ENABLE]
aobscanmodule(INJECT,game.exe,89 83 00 01 00 00)
alloc(newmem,$1000,INJECT)

label(code)
label(return)

newmem:
code:
  mov [rbx+00000100],eax
  mov [rbx+00000104],eax
  jmp return

INJECT:
  jmp newmem
  nop
return:

registersymbol(INJECT)

[DISABLE]
INJECT:
  db 89 83 00 01 00 00

unregistersymbol(INJECT)
dealloc(newmem)
)";

void TestEnableAndDisable() {
    FakeTarget target;
    auto host = target.Host();
    AutoAssembleOptions options;
    options.enable = true;
    options.x64 = true;

    AutoAssembleResult result;
    std::string error;
    const bool ok = RunAutoAssembler(kScript, host, options, result, &error);
    CHECK(ok);
    if (!ok) {
        std::cerr << "  enable error: " << error << std::endl;
        return;
    }

    const uint64_t inject = kModuleBase + kTargetOffset;
    CHECK(result.allocations.size() == 1 && result.allocations[0].first == "newmem");
    const uint64_t cave = result.allocations.empty() ? 0 : result.allocations[0].second;
    CHECK(result.registered.size() == 1 && result.registered[0].first == "INJECT" &&
          result.registered[0].second == inject);

    // The site now starts with a 5-byte jmp to the cave plus one nop.
    CHECK(target.ReadU8(inject) == 0xE9);
    const int32_t relative = static_cast<int32_t>(target.ReadU32(inject + 1));
    CHECK(static_cast<uint64_t>(static_cast<int64_t>(inject + 5) + relative) == cave);
    CHECK(target.ReadU8(inject + 5) == 0x90);

    // The cave holds the two movs and a jmp back to INJECT+6.
    CHECK(target.ReadU8(cave) == 0x89);
    const uint64_t jumpBack = cave + 12;  // two 6-byte movs
    CHECK(target.ReadU8(jumpBack) == 0xE9);
    const int32_t back = static_cast<int32_t>(target.ReadU32(jumpBack + 1));
    CHECK(static_cast<uint64_t>(static_cast<int64_t>(jumpBack + 5) + back) == inject + 6);

    // [DISABLE] restores the original bytes and frees the cave. The symbol
    // registered by [ENABLE] is supplied to the second run, as the caller
    // would keep it.
    AutoAssembleHost disableHost = target.Host();
    disableHost.symbol = [cave, inject](const std::string& name, uint64_t& value) {
        if (name == "game.exe") { value = kModuleBase; return true; }
        if (name == "newmem") { value = cave; return true; }
        if (name == "INJECT") { value = inject; return true; }
        return false;
    };
    options.enable = false;
    AutoAssembleResult undo;
    const bool undone = RunAutoAssembler(kScript, disableHost, options, undo, &error);
    CHECK(undone);
    if (!undone) std::cerr << "  disable error: " << error << std::endl;
    CHECK(undo.unregistered.size() == 1 && undo.unregistered[0] == "INJECT");
    CHECK(undo.freed.size() == 1 && undo.freed[0] == "newmem");
    CHECK(target.freed.size() == 1 && target.freed[0] == cave);
    for (size_t i = 0; i < sizeof(kTargetCode); ++i) CHECK(target.ReadU8(inject + i) == kTargetCode[i]);
}

void TestDefinesAndGlobalAlloc() {
    FakeTarget target;
    auto host = target.Host();
    AutoAssembleOptions options;
    AutoAssembleResult result;
    std::string error;
    const char* script = R"(
[ENABLE]
define(slot,rbx+100)
globalalloc(store,$100)
aobscanmodule(HOOK,game.exe,89 83 ?? 01 00 00)
HOOK:
  mov [slot],eax
)";
    const bool ok = RunAutoAssembler(script, host, options, result, &error);
    CHECK(ok);
    if (!ok) {
        std::cerr << "  defines error: " << error << std::endl;
        return;
    }
    // globalalloc allocates and registers in one go.
    CHECK(result.allocations.size() == 1 && result.allocations[0].first == "store");
    CHECK(result.registered.size() == 1 && result.registered[0].first == "store");
    // The define expanded back to the very same instruction bytes.
    for (size_t i = 0; i < sizeof(kTargetCode); ++i)
        CHECK(target.ReadU8(kModuleBase + kTargetOffset + i) == kTargetCode[i]);
}

void TestErrors() {
    FakeTarget target;
    auto host = target.Host();
    AutoAssembleOptions options;
    AutoAssembleResult result;
    std::string error;

    CHECK(!RunAutoAssembler("[ENABLE]\naobscanmodule(X,game.exe,11 22 33 44 55 66 77)\nX:\n nop\n", host,
                            options, result, &error));
    CHECK(error.find("not found") != std::string::npos);

    error.clear();
    CHECK(!RunAutoAssembler("[ENABLE]\nnowhere:\n nop\n", host, options, result, &error));
    CHECK(error.find("unknown symbol") != std::string::npos);

    error.clear();
    CHECK(!RunAutoAssembler("[ENABLE]\nassert(game.exe+1000,90 90 90)\n", host, options, result, &error));
    CHECK(error.find("assert failed") != std::string::npos);

    error.clear();
    CHECK(!RunAutoAssembler("[ENABLE]\ngame.exe+1000:\n frobnicate eax\n", host, options, result, &error));
    CHECK(error.find("Unknown instruction") != std::string::npos);

    // A script without [ENABLE] is treated as one enable section.
    error.clear();
    AutoAssembleResult plain;
    CHECK(RunAutoAssembler("game.exe+1000:\n nop\n nop\n", host, options, plain, &error));
    CHECK(target.ReadU8(kModuleBase + kTargetOffset) == 0x90);
}

void TestSectionsAndDetection() {
    std::string enable;
    std::string disable;
    const bool split = cortex::services::SplitAutoAssemblerSections(
        "define(a,1)\n[ENABLE]\nnop\n[DISABLE]\nret\n", enable, disable);
    CHECK(split);
    // Text before the first header belongs to both sections.
    CHECK(enable.find("define(a,1)") != std::string::npos && enable.find("nop") != std::string::npos);
    CHECK(disable.find("define(a,1)") != std::string::npos && disable.find("ret") != std::string::npos);
    CHECK(enable.find("ret") == std::string::npos);

    CHECK(cortex::services::LooksLikeAutoAssembler("[ENABLE]\nnop"));
    CHECK(cortex::services::LooksLikeAutoAssembler("alloc(x,$100)"));
    CHECK(!cortex::services::LooksLikeAutoAssembler("mov eax,1\nnop"));

    // Comments are stripped, including block comments.
    std::string commented;
    std::string ignored;
    cortex::services::SplitAutoAssemblerSections("[ENABLE]\nnop // trailing\n{a\nblock}\nret\n", commented, ignored);
    CHECK(commented.find("trailing") == std::string::npos && commented.find("block") == std::string::npos);
    CHECK(commented.find("nop") != std::string::npos && commented.find("ret") != std::string::npos);
}

void TestDryRunAndThread() {
    FakeTarget target;
    auto host = target.Host();
    AutoAssembleOptions options;
    options.dryRun = true;
    AutoAssembleResult result;
    std::string error;
    const bool ok = RunAutoAssembler(kScript, host, options, result, &error);
    CHECK(ok);
    if (!ok) std::cerr << "  dry run error: " << error << std::endl;
    // A dry run checks the script without touching the target.
    CHECK(target.ReadU8(kModuleBase + kTargetOffset) == kTargetCode[0]);
    CHECK(target.blocks.size() == 1);

    options.dryRun = false;
    AutoAssembleResult threaded;
    error.clear();
    const bool started = RunAutoAssembler(
        "[ENABLE]\nalloc(entry,$100)\nentry:\n ret\ncreatethread(entry)\n", host, options, threaded, &error);
    CHECK(started);
    CHECK(target.threads.size() == 1 && !threaded.allocations.empty() &&
          target.threads[0] == threaded.allocations[0].second);
}

} // namespace

int main() {
    TestEnableAndDisable();
    TestDefinesAndGlobalAlloc();
    TestErrors();
    TestSectionsAndDetection();
    TestDryRunAndThread();
    if (failures) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "auto assembler tests passed" << std::endl;
    return 0;
}
