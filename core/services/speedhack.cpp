#include "speedhack.h"

#include "assembler.h"

#include <cstdio>
#include <cstring>

namespace cortex::services {
namespace {

// The cave starts with three 8-byte slots the hook reads and writes.
constexpr uint64_t kMultiplierOffset = 0;
constexpr uint64_t kBaseOffset = 8;
constexpr uint64_t kFakeOffset = 16;
constexpr uint64_t kCodeOffset = 24;
constexpr size_t kCaveSize = 512;

enum class Shape {
    Tick32,   // returns a 32-bit tick count in eax
    Tick64,   // returns a 64-bit tick count in rax
    Counter,  // fills the LARGE_INTEGER the first argument points at
};

struct Candidate {
    const char* name;
    Shape shape;
    bool x64Only;
};

const Candidate kCandidates[] = {
    {"kernel32.GetTickCount", Shape::Tick32, false},
    {"kernel32.GetTickCount64", Shape::Tick64, true},
    {"kernel32.QueryPerformanceCounter", Shape::Counter, true},
    {"winmm.timeGetTime", Shape::Tick32, false},
};

std::string Hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string HexBytes(const std::vector<uint8_t>& bytes) {
    std::string text;
    char buffer[4] = {};
    for (const auto byte : bytes) {
        std::snprintf(buffer, sizeof(buffer), "%02X", byte);
        if (!text.empty()) text += ' ';
        text += buffer;
    }
    return text;
}

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

// The assembly of one cave: the data slots, a trampoline that runs the
// replaced instructions and jumps back, then the scaling hook.
std::string CaveSource(Shape shape, bool x64, double multiplier, uint64_t cave, uint64_t resume,
                       const std::vector<uint8_t>& relocated) {
    const std::string mult = "[" + Hex(cave + kMultiplierOffset) + "]";
    const std::string base = "[" + Hex(cave + kBaseOffset) + "]";
    const std::string fake = "[" + Hex(cave + kFakeOffset) + "]";
    char multiplierText[64] = {};
    std::snprintf(multiplierText, sizeof(multiplierText), "%.17g", multiplier);

    std::string text;
    text += "dq (double)" + std::string(multiplierText) + "\n";
    text += "dq 0\n";
    text += "dq 0\n";
    text += "trampoline:\n";
    text += "db " + HexBytes(relocated) + "\n";
    text += "jmp " + Hex(resume) + "\n";
    text += "hook:\n";

    if (shape == Shape::Counter) {
        // BOOL QueryPerformanceCounter(LARGE_INTEGER* counter) in rcx.
        text += "  sub rsp,38\n";
        text += "  mov [rsp+30],rcx\n";
        text += "  call trampoline\n";
        text += "  mov rcx,[rsp+30]\n";
        text += "  add rsp,38\n";
        text += "  mov rdx,[rcx]\n";
        text += "  mov r8,qword " + base + "\n";
        text += "  test r8,r8\n";
        text += "  jne scale\n";
        text += "  mov qword " + base + ",rdx\n";
        text += "  mov qword " + fake + ",rdx\n";
        text += "  mov r8,rdx\n";
        text += "scale:\n";
        text += "  sub rdx,r8\n";
        text += "  cvtsi2sd xmm0,rdx\n";
        text += "  mulsd xmm0,qword " + mult + "\n";
        text += "  cvttsd2si rdx,xmm0\n";
        text += "  add rdx,qword " + fake + "\n";
        text += "  mov [rcx],rdx\n";
        text += "  ret\n";
        return text;
    }

    const bool wide = shape == Shape::Tick64;
    const std::string a = wide ? "rax" : "eax";
    const std::string c = wide ? "rcx" : "ecx";
    const std::string word = wide ? "qword " : "dword ";
    if (x64) {
        text += "  sub rsp,28\n";
        text += "  call trampoline\n";
        text += "  add rsp,28\n";
    } else {
        text += "  call trampoline\n";
    }
    text += "  mov " + c + "," + word + base + "\n";
    text += "  test " + c + "," + c + "\n";
    text += "  jne scale\n";
    text += "  mov " + word + base + "," + a + "\n";
    text += "  mov " + word + fake + "," + a + "\n";
    text += "  mov " + c + "," + a + "\n";
    text += "scale:\n";
    text += "  sub " + a + "," + c + "\n";
    text += "  cvtsi2sd xmm0," + a + "\n";
    text += "  mulsd xmm0,qword " + mult + "\n";
    text += "  cvttsd2si " + a + ",xmm0\n";
    text += "  add " + a + "," + word + fake + "\n";
    text += "  ret\n";
    return text;
}

bool InstallOne(const SpeedhackHost& host, bool x64, double multiplier, const Candidate& candidate,
                SpeedhackHook& hook, std::string& error) {
    uint64_t address = 0;
    if (!host.symbol || !host.symbol(candidate.name, address) || !address)
        return Fail(&error, std::string(candidate.name) + " was not found");

    std::vector<uint8_t> head(32);
    if (!host.read || !host.read(address, head.data(), head.size()))
        return Fail(&error, std::string("cannot read ") + candidate.name);

    // The jump that replaces the prologue decides how much to relocate.
    size_t steal = 0;
    if (!CodeBoundary(head.data(), head.size(), 5, x64, steal, &error))
        return Fail(&error, std::string(candidate.name) + ": " + error);
    if (steal > head.size()) return Fail(&error, std::string(candidate.name) + ": prologue too long");

    uint64_t cave = 0;
    if (!host.allocate || !host.allocate(kCaveSize, address, cave, error))
        return Fail(&error, std::string(candidate.name) + ": " + error);

    auto abandon = [&](const std::string& message) {
        std::string ignored;
        if (host.release) host.release(cave, ignored);
        return Fail(&error, message);
    };

    std::vector<uint8_t> relocated;
    if (!RelocateCode(head.data(), steal, address, cave + kCodeOffset, x64, relocated, &error))
        return abandon(std::string(candidate.name) + ": " + error);

    AssembleRequest request;
    request.text = CaveSource(candidate.shape, x64, multiplier, cave, address + steal, relocated);
    request.address = cave;
    request.x64 = x64;
    AssembleBlockResult block;
    if (!AssembleBlock(request, block, &error))
        return abandon(std::string(candidate.name) + ": " + error);
    if (block.bytes.size() > kCaveSize) return abandon(std::string(candidate.name) + ": the hook is too large");

    uint64_t hookAddress = 0;
    for (const auto& label : block.labels)
        if (label.first == "hook") hookAddress = label.second;
    if (!hookAddress) return abandon(std::string(candidate.name) + ": the hook label is missing");

    // The patch at the function: a jump to the hook, padded with NOPs.
    AssembleRequest jump;
    jump.text = "jmp " + Hex(hookAddress);
    jump.address = address;
    jump.x64 = x64;
    std::vector<uint8_t> patch;
    if (!AssembleLine(jump, patch, &error)) return abandon(std::string(candidate.name) + ": " + error);
    if (patch.size() > steal) return abandon(std::string(candidate.name) + ": the jump does not fit");
    patch.resize(steal, 0x90);

    if (!host.write || !host.write(cave, block.bytes.data(), block.bytes.size()))
        return abandon(std::string(candidate.name) + ": cannot write the hook");
    if (!host.write(address, patch.data(), patch.size()))
        return abandon(std::string(candidate.name) + ": cannot patch the function");

    hook.function = candidate.name;
    hook.address = address;
    hook.cave = cave;
    hook.original.assign(head.begin(), head.begin() + static_cast<std::ptrdiff_t>(steal));
    return true;
}

} // namespace

bool InstallSpeedhack(const SpeedhackHost& host, bool x64, double multiplier, SpeedhackState& state,
                      std::string* error) {
    if (state.active) return Fail(error, "the speedhack is already installed");
    if (!(multiplier > 0.0)) return Fail(error, "the speed must be greater than zero");
    state.hooks.clear();
    state.skipped.clear();

    for (const auto& candidate : kCandidates) {
        if (candidate.x64Only && !x64) {
            state.skipped.push_back(std::string(candidate.name) + " (x64 only)");
            continue;
        }
        SpeedhackHook hook;
        std::string message;
        if (InstallOne(host, x64, multiplier, candidate, hook, message)) state.hooks.push_back(std::move(hook));
        else state.skipped.push_back(message);
    }

    if (state.hooks.empty()) return Fail(error, "no timing function could be hooked");
    state.multiplier = multiplier;
    state.active = true;
    if (error) error->clear();
    return true;
}

bool UpdateSpeedhack(const SpeedhackHost& host, SpeedhackState& state, double multiplier, std::string* error) {
    if (!state.active) return Fail(error, "the speedhack is not installed");
    if (!(multiplier > 0.0)) return Fail(error, "the speed must be greater than zero");
    for (const auto& hook : state.hooks) {
        // The scaling is applied around the value the hook first saw, so
        // only the multiplier has to change.
        if (!host.write || !host.write(hook.cave + kMultiplierOffset, &multiplier, sizeof(multiplier)))
            return Fail(error, "cannot update " + hook.function);
    }
    state.multiplier = multiplier;
    if (error) error->clear();
    return true;
}

bool RemoveSpeedhack(const SpeedhackHost& host, SpeedhackState& state, std::string* error) {
    bool ok = true;
    std::string message;
    for (const auto& hook : state.hooks) {
        if (!host.write || !host.write(hook.address, hook.original.data(), hook.original.size())) {
            ok = false;
            message = "cannot restore " + hook.function;
        }
    }
    // The caves are freed only once every function is back to normal: a
    // thread may still be inside one.
    if (ok) {
        std::string ignored;
        for (const auto& hook : state.hooks)
            if (host.release) host.release(hook.cave, ignored);
    }
    state.hooks.clear();
    state.skipped.clear();
    state.active = false;
    state.multiplier = 1.0;
    if (!ok) return Fail(error, message);
    if (error) error->clear();
    return true;
}

} // namespace cortex::services
