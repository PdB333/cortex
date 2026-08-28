#include "disassembly_service.h"

#include <Zydis/Zydis.h>

#include <algorithm>
#include <array>
#include <limits>

namespace cortex::services {
namespace {

void SetError(std::string* error, const char* value) {
    if (error) *error = value ? value : "";
}

std::vector<uint8_t> ReadWindow(const target::SessionPtr& session,
                                uint64_t address,
                                size_t requested,
                                std::string* error) {
    if (!session) {
        SetError(error, "no_active_session");
        return {};
    }

    const auto regions = session->MemoryRegions();
    const auto region = std::find_if(regions.begin(), regions.end(), [address](const auto& item) {
        if (!item.readable || item.size == 0 || address < item.base) return false;
        const uint64_t offset = address - item.base;
        return offset < item.size;
    });
    if (region == regions.end()) {
        SetError(error, "address_not_readable");
        return {};
    }

    const uint64_t offset = address - region->base;
    const uint64_t available64 = region->size - offset;
    const size_t available = available64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(available64);
    const size_t readSize = std::min(requested, available);
    if (readSize == 0) {
        SetError(error, "address_not_readable");
        return {};
    }

    std::vector<uint8_t> bytes(readSize);
    size_t bytesRead = 0;
    if (!session->ReadMemory(address, bytes.data(), bytes.size(), &bytesRead) || bytesRead == 0) {
        SetError(error, "memory_read_failed");
        return {};
    }
    bytes.resize(bytesRead);
    return bytes;
}

} // namespace

bool DisassemblyService::Decode(uint64_t address,
                                size_t count,
                                std::vector<DisassemblyInstruction>& instructions,
                                std::string* error) const {
    instructions.clear();
    if (error) error->clear();
    if (address == 0 || count == 0) {
        SetError(error, "invalid_address_or_count");
        return false;
    }

    const auto session = sessions_.Active();
    if (!session || !session->Alive()) {
        SetError(error, "no_active_session");
        return false;
    }
    if (!session->Capabilities().Has(target::Capability::MemoryRead)) {
        SetError(error, "memory_read_not_supported");
        return false;
    }

    ZydisMachineMode machineMode{};
    ZydisStackWidth stackWidth{};
    switch (session->Target().architecture) {
        case target::Architecture::X86:
            machineMode = ZYDIS_MACHINE_MODE_LEGACY_32;
            stackWidth = ZYDIS_STACK_WIDTH_32;
            break;
        case target::Architecture::X64:
            machineMode = ZYDIS_MACHINE_MODE_LONG_64;
            stackWidth = ZYDIS_STACK_WIDTH_64;
            break;
        default:
            SetError(error, "disassembly_architecture_not_supported");
            return false;
    }

    count = std::min<size_t>(count, 1000);
    const size_t requested = std::min<size_t>(count * 15 + 15, 64 * 1024);
    auto bytes = ReadWindow(session, address, requested, error);
    if (bytes.empty()) return false;

    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, machineMode, stackWidth))) {
        SetError(error, "zydis_decoder_init_failed");
        return false;
    }
    ZydisFormatter formatter;
    if (!ZYAN_SUCCESS(ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL))) {
        SetError(error, "zydis_formatter_init_failed");
        return false;
    }

    instructions.reserve(count);
    size_t offset = 0;
    while (offset < bytes.size() && instructions.size() < count) {
        const uint64_t instructionAddress = address + static_cast<uint64_t>(offset);
        ZydisDecodedInstruction decoded{};
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        const ZyanStatus status = ZydisDecoderDecodeFull(
            &decoder, bytes.data() + offset, bytes.size() - offset, &decoded, operands.data());

        DisassemblyInstruction item;
        item.address = instructionAddress;
        if (!ZYAN_SUCCESS(status) || decoded.length == 0) {
            item.bytes.assign(1, bytes[offset]);
            item.mnemonic = "?";
            item.text = "(invalid)";
            instructions.push_back(std::move(item));
            ++offset;
            continue;
        }

        item.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                          bytes.begin() + static_cast<std::ptrdiff_t>(offset + decoded.length));
        if (const char* mnemonic = ZydisMnemonicGetString(decoded.mnemonic)) item.mnemonic = mnemonic;

        char formatted[256] = {};
        if (ZYAN_SUCCESS(ZydisFormatterFormatInstruction(
                &formatter, &decoded, operands.data(), decoded.operand_count_visible,
                formatted, sizeof(formatted), instructionAddress, ZYAN_NULL))) {
            item.text = formatted;
        } else {
            item.text = item.mnemonic;
        }

        instructions.push_back(std::move(item));
        offset += decoded.length;
    }

    if (instructions.empty()) {
        SetError(error, "no_instructions_decoded");
        return false;
    }
    return true;
}

} // namespace cortex::services
