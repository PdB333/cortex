#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace patch {

struct PatchInfo {
    int id;
    uintptr_t address;
    std::vector<uint8_t> originalBytes;
    std::vector<uint8_t> newBytes;
    std::string label;
    uintptr_t gateway = 0;
};

struct TrampolineInfo {
    int patchId = -1;
    uintptr_t gateway = 0;
    size_t overwrittenSize = 0;
    size_t gatewaySize = 0;
};

// Writes `newBytes` at `address`, remembering the original bytes so the
// patch can be reverted later. Returns the new patch id, or -1 if the
// original bytes can't be read or the write fails.
int Apply(uintptr_t address, const std::vector<uint8_t>& newBytes, const std::string& label);

// Convenience: same as Apply but fills `size` bytes with 0x90 (NOP).
int Nop(uintptr_t address, size_t size, const std::string& label);

// Writes a relative jmp (E9 rel32) from `address` to `target`, tracked as a
// normal revertable patch. `jmpSize` bytes at `address` are overwritten (5
// for the jmp itself, NOP-padded if larger -- callers overwriting a wider
// instruction should pass its full length so no half-instruction is left
// behind). Fails if `target` is out of rel32 range (+/-2GB) -- use
// AllocNearCave to get a target guaranteed to be in range on x64. Does NOT
// build a trampoline (i.e. does not preserve/relocate the overwritten
// instructions into the cave) -- callers wanting a classic detour-with-
// trampoline should read the original bytes (returned patch's original, or
// via /memory/read before patching) and write them into the cave themselves
// followed by their own jmp back, since that requires knowing instruction
// boundaries (disassemble first).
int Detour(uintptr_t address, uintptr_t target, int jmpSize, std::string& outError);

// Builds a classic detour gateway: decodes complete source instructions,
// relocates relative branches and RIP/EIP-relative memory operands into a
// nearby executable cave, appends a jump back, then patches the source.
bool CreateTrampoline(uintptr_t address, uintptr_t target, size_t minimumOverwrite,
                      TrampolineInfo& out, std::string& outError);

// Restores the original bytes at a patch's address and removes it from the
// registry. Returns false if `id` doesn't exist.
bool Revert(int id);

std::vector<PatchInfo> List();

// Allocates an RWX region of `size` bytes, on x64 within +/-2GB of
// `nearAddress` so a rel32 jmp/call can reach it (on x86 any address
// reaches, so this is just a plain RWX allocation). Returns 0 on failure.
// The region lives for the process's lifetime (never freed) -- appropriate
// for a code cave meant to back a detour trampoline.
uintptr_t AllocNearCave(uintptr_t nearAddress, size_t size);

// Hand-rolled mini x86 assembler covering the pragmatic subset of mnemonics
// needed to hand-write a Cheat-Engine-AA-style detour/trampoline without
// leaving the API: nop [count], int3, ret, push/pop reg, push imm32,
// mov/add/sub/xor/cmp reg,reg or reg,imm, inc/dec reg, and jmp/call/je/jne/
// jz/jnz/jg/jl/jge/jle to an absolute target address. One instruction per
// entry in `lines`; blank lines and lines starting with ";" are skipped.
// `address` is where the first assembled byte will end up once written --
// required because jmp/call/jcc operands are given as absolute addresses
// and each instruction's own address (needed to compute its relative
// displacement) depends on the cumulative length of the ones before it.
// Returns false and fills `outError` with the failing line's text plus a
// reason on the first unparseable/unencodable line; `outBytes` is only
// valid when this returns true.
bool Assemble(const std::vector<std::string>& lines, uintptr_t address, std::vector<uint8_t>& outBytes,
              std::string& outError);

} // namespace patch
