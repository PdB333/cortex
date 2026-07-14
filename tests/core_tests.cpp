#include "memory/scan.h"
#include "action/action.h"
#include "memory/provenance.h"
#include "patch/patch.h"
#include "struct/infer.h"
#include "timeline/timeline.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <cstring>

namespace {
int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

void TestExact64BitScan() {
    auto* page = static_cast<uint64_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Expect(page != nullptr, "VirtualAlloc test page");
    if (!page) return;

    constexpr uint64_t precise = 9007199254740993ULL; // first odd integer above JS's exact range
    page[0] = precise;
    size_t count = 0;
    bool truncated = false;
    int id = memscan::ScanNew("u64", std::string("9007199254740993"),
                              reinterpret_cast<uintptr_t>(page), reinterpret_cast<uintptr_t>(page) + 8,
                              count, truncated);
    Expect(id > 0, "u64 scan session created");
    Expect(count == 1 && !truncated, "u64 exact scan keeps full precision");

    std::vector<memscan::ScanResult> results;
    size_t total = 0;
    Expect(memscan::ScanResults(id, 0, 10, results, total), "u64 scan results available");
    Expect(results.size() == 1 && std::holds_alternative<uint64_t>(results[0].value) &&
           std::get<uint64_t>(results[0].value) == precise, "u64 result is not widened to double");

    page[0] = precise + 1;
    Expect(memscan::ScanNext(id, memscan::Filter::IncreasedBy, std::string("1"), std::nullopt, count),
           "u64 next scan succeeds");
    Expect(count == 1, "u64 increased_by comparison stays exact");
    memscan::ScanReset(id);

    const int64_t minimum = (std::numeric_limits<int64_t>::min)();
    *reinterpret_cast<int64_t*>(page) = minimum;
    id = memscan::ScanNew("i64", std::to_string(minimum), reinterpret_cast<uintptr_t>(page),
                          reinterpret_cast<uintptr_t>(page) + 8, count, truncated);
    Expect(id > 0 && count == 1, "i64 minimum scans exactly");
    memscan::ScanReset(id);
    VirtualFree(page, 0, MEM_RELEASE);
}

void TestLargeRegionIsNotSkipped() {
    constexpr size_t regionSize = 80u * 1024 * 1024;
    auto* region = static_cast<uint8_t*>(VirtualAlloc(nullptr, regionSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Expect(region != nullptr, "VirtualAlloc large scan region");
    if (!region) return;

    constexpr size_t targetOffset = 70u * 1024 * 1024;
    constexpr int32_t targetValue = 0x12345678;
    memcpy(region + targetOffset, &targetValue, sizeof(targetValue));
    constexpr size_t blockBoundaryOffset = 8u * 1024 * 1024 - 2;
    constexpr int32_t boundaryValue = 0x23456701;
    memcpy(region + blockBoundaryOffset, &boundaryValue, sizeof(boundaryValue));

    size_t count = 0;
    bool truncated = false;
    int id = memscan::ScanNew("i32", std::to_string(targetValue),
                              reinterpret_cast<uintptr_t>(region),
                              reinterpret_cast<uintptr_t>(region) + regionSize,
                              count, truncated);
    Expect(id > 0, "large-region scan session created");
    Expect(count == 1 && !truncated, "value beyond 64 MiB region boundary is scanned");

    std::vector<memscan::ScanResult> results;
    size_t total = 0;
    memscan::ScanResults(id, 0, 10, results, total);
    Expect(results.size() == 1 && results[0].address == reinterpret_cast<uintptr_t>(region) + targetOffset,
           "large-region scan returns exact target address");
    memscan::ScanReset(id);

    memscan::ScanOptions byteAligned;
    byteAligned.alignment = 1;
    id = memscan::ScanNew("i32", std::to_string(boundaryValue),
                          reinterpret_cast<uintptr_t>(region),
                          reinterpret_cast<uintptr_t>(region) + 16u * 1024 * 1024,
                          count, truncated, byteAligned);
    results.clear();
    memscan::ScanResults(id, 0, 10, results, total);
    Expect(count == 1 && results.size() == 1 &&
           results[0].address == reinterpret_cast<uintptr_t>(region) + blockBoundaryOffset,
           "unaligned value crossing scan-block boundary is found exactly once");
    memscan::ScanReset(id);
    VirtualFree(region, 0, MEM_RELEASE);
}

void TestGlobalAobScanIncludesPrivateMemory() {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Expect(page != nullptr, "VirtualAlloc private AOB page");
    if (!page) return;

    constexpr uint8_t signature[] = {0xD3, 0xAD, 0x7B, 0x41, 0xC9, 0x5E, 0x26, 0xF0};
    constexpr size_t signatureOffset = 733;
    memcpy(page + signatureOffset, signature, sizeof(signature));

    bool truncated = false;
    const auto matches = memscan::AobScan("D3 AD 7B 41 C9 5E 26 F0", "", truncated);
    const auto expected = reinterpret_cast<uintptr_t>(page) + signatureOffset;
    Expect(std::find(matches.begin(), matches.end(), expected) != matches.end(),
           "global AOB scan includes committed private memory");
    Expect(!truncated, "private-memory AOB scan is not truncated");

    VirtualFree(page, 0, MEM_RELEASE);
}

void TestActionRollbackOrder() {
    action::Clear();
    int value = 0;
    const uint64_t checkpoint = action::Checkpoint();
    value = 1;
    action::Record("first", [&value] { value = 0; return true; });
    value = 2;
    action::Record("second", [&value] { value = 1; return true; });
    const auto result = action::RollbackTo(checkpoint);
    Expect(result.size() == 2, "journal rolls back every action after checkpoint");
    Expect(value == 0, "journal rolls back in reverse order");
    Expect(action::List().empty(), "successful rollback removes journal entries");
}

void TestCortexOwnedMemoryIsExcluded() {
    auto* page = static_cast<uint32_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Expect(page != nullptr, "VirtualAlloc provenance page");
    if (!page) return;
    page[0] = 0x6A17B35Du;
    const uint64_t rangeId = provenance::Register(reinterpret_cast<uintptr_t>(page), 4096, "test", "owned");
    size_t count = 0; bool truncated = false;
    int id = memscan::ScanNew("u32", std::to_string(page[0]), reinterpret_cast<uintptr_t>(page),
                              reinterpret_cast<uintptr_t>(page) + 4, count, truncated);
    Expect(id > 0 && count == 0, "default scan excludes Cortex-owned ranges");
    memscan::ScanReset(id);
    memscan::ScanOptions includeOwned; includeOwned.excludeCortex = false;
    id = memscan::ScanNew("u32", std::to_string(page[0]), reinterpret_cast<uintptr_t>(page),
                          reinterpret_cast<uintptr_t>(page) + 4, count, truncated, includeOwned);
    Expect(id > 0 && count == 1, "scan can explicitly include Cortex-owned ranges");
    memscan::ScanReset(id);
    provenance::Unregister(rangeId);
    VirtualFree(page, 0, MEM_RELEASE);
}

void TestRelocatingTrampoline() {
    auto* source = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    auto* target = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    Expect(source && target, "VirtualAlloc trampoline pages");
    if (!source || !target) return;
    const uint8_t prologue[] = {0x55,0x8B,0xEC,0x83,0xEC,0x10,0x90,0xC3};
    memcpy(source, prologue, sizeof(prologue)); target[0] = 0xC3;
    patch::TrampolineInfo info; std::string error;
    const bool ok = patch::CreateTrampoline(reinterpret_cast<uintptr_t>(source),
                                             reinterpret_cast<uintptr_t>(target), 5, info, error);
    Expect(ok, "relocating trampoline is created");
    if (ok) {
        Expect(source[0] == 0xE9 && info.gateway != 0 && info.overwrittenSize >= 5,
               "trampoline patches source and returns gateway metadata");
        Expect(patch::Revert(info.patchId), "trampoline patch reverts");
        Expect(memcmp(source, prologue, info.overwrittenSize) == 0, "trampoline revert restores complete instructions");
    } else std::cerr << "trampoline error: " << error << '\n';
    VirtualFree(source, 0, MEM_RELEASE); VirtualFree(target, 0, MEM_RELEASE);
}

void TestStructureInference() {
    constexpr size_t size = 64;
    auto* a = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto* b = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Expect(a && b, "VirtualAlloc inference objects");
    if (!a || !b) return;
    const size_t p = sizeof(uintptr_t);
    int32_t ammoA=38,ammoB=50; float speedA=1.5f,speedB=2.5f;
    memcpy(a+p,&ammoA,4);memcpy(b+p,&ammoB,4);memcpy(a+p+4,&speedA,4);memcpy(b+p+4,&speedB,4);
    std::vector<structinfer::FieldGuess> fields; std::string error;
    Expect(structinfer::Infer({reinterpret_cast<uintptr_t>(a),reinterpret_cast<uintptr_t>(b)},size,fields,error),
           "multi-instance structure inference succeeds");
    auto ammo=std::find_if(fields.begin(),fields.end(),[&](const auto& f){return f.offset==p;});
    auto speed=std::find_if(fields.begin(),fields.end(),[&](const auto& f){return f.offset==p+4;});
    Expect(ammo!=fields.end() && ammo->type=="i32" && !ammo->constant,"varying integer field is inferred");
    Expect(speed!=fields.end() && speed->type=="float","floating-point field is inferred");
    VirtualFree(a,0,MEM_RELEASE);VirtualFree(b,0,MEM_RELEASE);
}

void TestTargetedTimeline() {
    auto* value=static_cast<uint32_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    Expect(value!=nullptr,"VirtualAlloc timeline page");if(!value)return;
    std::string error;*value=8;
    int a=timeline::Capture({{reinterpret_cast<uintptr_t>(value),4}},"before",error);
    *value=6;int b=timeline::Capture({{reinterpret_cast<uintptr_t>(value),4}},"after",error);
    std::vector<timeline::Change> changes;
    Expect(timeline::Diff(a,b,changes,error)&&changes.size()==1,"timeline reports targeted memory change");
    timeline::Transition transition{};
    Expect(timeline::LastChange(reinterpret_cast<uintptr_t>(value),4,transition,error)&&transition.fromId==a&&transition.toId==b,
           "timeline finds latest transition");
    std::vector<timeline::Range> previous;
    Expect(timeline::Restore(a,previous,error)&&*value==8,"timeline rewinds selected memory");
    Expect(timeline::RestoreRanges(previous)&&*value==6,"timeline rewind can be undone");
    timeline::Clear();VirtualFree(value,0,MEM_RELEASE);
}
}

int main() {
    TestExact64BitScan();
    TestLargeRegionIsNotSkipped();
    TestGlobalAobScanIncludesPrivateMemory();
    TestActionRollbackOrder();
    TestCortexOwnedMemoryIsExcluded();
    TestRelocatingTrampoline();
    TestStructureInference();
    TestTargetedTimeline();
    if (g_failures == 0) std::cout << "All Cortex core tests passed\n";
    return g_failures == 0 ? 0 : 1;
}
