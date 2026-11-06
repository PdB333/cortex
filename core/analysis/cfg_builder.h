#pragma once

// The control-flow graph algorithm shared by the in-process runtime
// (analysis::BuildCFG, over the target's memory) and offline analysis of
// files (core/artifacts/native_code, over a PE's bytes). Only the byte
// source differs: `fetch(address, maxBytes)` returns the readable bytes at
// `address` (fewer near the end of a region, none if unreadable).
//
// Recursive descent: a block is decoded until a ret, a jump, a conditional
// branch or an undecodable byte. Calls do not end a block. Branch targets
// and fall-throughs after conditional branches become new leaders, explored
// with a visited set so loops terminate. An indirect jump yields an edge
// with `to = 0`, type "indirect".

#include "analysis.h"

#include <Zydis/Zydis.h>

#include <set>
#include <vector>

namespace analysis {

// The absolute target of a direct (immediate) call/jmp/jcc; false for
// indirect (register or memory) branches.
inline bool ResolveDirectBranch(const ZydisDecodedInstruction& instr, const ZydisDecodedOperand* operands,
                                uint64_t address, uint64_t& target) {
    if (instr.operand_count_visible < 1 || operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) return false;
    ZyanU64 absolute = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instr, &operands[0], address, &absolute))) return false;
    target = absolute;
    return true;
}

template <typename Fetch>
CfgResult BuildCfgFrom(uintptr_t address, const ZydisDecoder& decoder, Fetch&& fetch, size_t maxBlocks,
                       size_t maxInstructionsPerBlock) {
    CfgResult result;
    std::set<uintptr_t> leaders{address};
    std::set<uintptr_t> visited;
    std::vector<uintptr_t> worklist{address};

    while (!worklist.empty()) {
        if (result.blocks.size() >= maxBlocks) {
            result.truncated = true;
            break;
        }
        const uintptr_t blockStart = worklist.back();
        worklist.pop_back();
        if (visited.count(blockStart)) continue;
        visited.insert(blockStart);

        const std::vector<uint8_t> buf = fetch(blockStart, maxInstructionsPerBlock * 16);
        if (buf.empty()) continue;

        size_t offset = 0;
        uintptr_t curAddr = blockStart;
        uintptr_t blockEnd = blockStart;
        size_t instrCount = 0;

        while (offset < buf.size() && instrCount < maxInstructionsPerBlock) {
            if (curAddr != blockStart && leaders.count(curAddr)) {
                result.edges.push_back({blockStart, curAddr, "fallthrough"});
                if (!visited.count(curAddr)) worklist.push_back(curAddr);
                break;
            }

            ZydisDecodedInstruction instr;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            const ZyanStatus status =
                ZydisDecoderDecodeFull(&decoder, buf.data() + offset, buf.size() - offset, &instr, operands);
            if (!ZYAN_SUCCESS(status)) break;  // undecodable: end the block here, no successors

            blockEnd = curAddr + instr.length;
            instrCount++;
            const auto category = instr.meta.category;

            if (category == ZYDIS_CATEGORY_RET) break;
            if (category == ZYDIS_CATEGORY_COND_BR) {
                const uintptr_t fallAddr = curAddr + instr.length;
                leaders.insert(fallAddr);
                if (!visited.count(fallAddr)) worklist.push_back(fallAddr);
                result.edges.push_back({blockStart, fallAddr, "cond_false"});
                uint64_t target = 0;
                if (ResolveDirectBranch(instr, operands, curAddr, target)) {
                    leaders.insert(static_cast<uintptr_t>(target));
                    if (!visited.count(static_cast<uintptr_t>(target))) worklist.push_back(static_cast<uintptr_t>(target));
                    result.edges.push_back({blockStart, static_cast<uintptr_t>(target), "cond_true"});
                }
                break;
            }
            if (category == ZYDIS_CATEGORY_UNCOND_BR) {
                uint64_t target = 0;
                if (ResolveDirectBranch(instr, operands, curAddr, target)) {
                    leaders.insert(static_cast<uintptr_t>(target));
                    if (!visited.count(static_cast<uintptr_t>(target))) worklist.push_back(static_cast<uintptr_t>(target));
                    result.edges.push_back({blockStart, static_cast<uintptr_t>(target), "jump"});
                } else {
                    result.edges.push_back({blockStart, 0, "indirect"});
                }
                break;
            }

            offset += instr.length;
            curAddr += instr.length;
        }

        result.blocks.push_back({blockStart, blockEnd});
    }
    return result;
}

} // namespace analysis
