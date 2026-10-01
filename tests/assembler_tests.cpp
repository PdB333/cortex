// Unit tests for the text assembler (Zydis encoder, Cheat Engine syntax).

#include "services/assembler.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using cortex::services::AssembleLine;
using cortex::services::AssembleRequest;
using cortex::services::ParseAsmNumber;

int failures = 0;

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

bool Symbols(const std::string& name, uint64_t& value, std::string& error) {
    if (name == "returnhere") value = 0x140001234;
    else if (name == "health") value = 0x140001100;
    else if (name == "game.exe") value = 0x140000000;
    else {
        error = "Unknown symbol: " + name;
        return false;
    }
    return true;
}

void Expect(const char* text, uint64_t address, bool x64, const char* expected) {
    AssembleRequest request;
    request.text = text;
    request.address = address;
    request.x64 = x64;
    request.evaluate = Symbols;
    std::vector<uint8_t> bytes;
    std::string error;
    const bool ok = AssembleLine(request, bytes, &error);
    const std::string got = ok ? HexBytes(bytes) : "error: " + error;
    if (got != expected) {
        ++failures;
        std::cerr << "FAIL: " << text << " -> " << got << " (expected " << expected << ")" << std::endl;
    }
}

void ExpectError(const char* text, bool x64, const char* fragment) {
    AssembleRequest request;
    request.text = text;
    request.address = 0x140001000;
    request.x64 = x64;
    request.evaluate = Symbols;
    std::vector<uint8_t> bytes;
    std::string error;
    if (AssembleLine(request, bytes, &error) || error.find(fragment) == std::string::npos) {
        ++failures;
        std::cerr << "FAIL: " << text << " should fail with '" << fragment << "', got '" << error << "' "
                  << HexBytes(bytes) << std::endl;
    }
}

} // namespace

int main() {
    const uint64_t at = 0x140001000;
    Expect("mov eax,5", at, true, "B8 05 00 00 00");
    Expect("mov [rax+10],ecx", at, true, "89 48 10");
    Expect("mov dword ptr [rbx+rcx*4+8],#100", at, true, "C7 44 8B 08 64 00 00 00");
    Expect("jmp 140002000", at, true, "E9 FB 0F 00 00");
    Expect("je short 140001010", at, true, "74 0E");
    Expect("je 140001010", at, true, "74 0E");  // auto-short
    Expect("jne 140001010", at, true, "75 0E");  // auto-short
    Expect("mov rax,[140001100]", at, true, "48 8B 05 F9 00 00 00");
    Expect("sub rsp,28", at, true, "48 83 EC 28");
    Expect("movss xmm0,[rax]", at, true, "F3 0F 10 00");
    Expect("movzx eax,byte ptr [rcx]", at, true, "0F B6 01");
    Expect("add dword ptr [rax],1", at, true, "83 00 01");
    Expect("lock inc dword ptr [rax]", at, true, "F0 FF 00");
    Expect("ret", at, true, "C3");
    Expect("push rbp", at, true, "55");
    Expect("nop", at, true, "90");
    Expect("lea rcx,[rax+rbx*2+10]", at, true, "48 8D 4C 58 10");
    // Zydis picks the short moffs form with a 0x67 prefix: same address.
    Expect("mov rax,gs:[60]", 0, true, "65 67 48 A1 60 00 00 00");
    Expect("cmove eax,ecx", at, true, "0F 44 C1");
    Expect("mov [rax-8],rdx", at, true, "48 89 50 F8");
    Expect("jmp 7FF000000000", at, true, "FF 25 00 00 00 00 00 00 00 00 F0 7F 00 00");
    Expect("call 7FF000000000", at, true, "FF 15 02 00 00 00 EB 08 00 00 00 00 F0 7F 00 00");
    Expect("jmp returnhere", at, true, "E9 2F 02 00 00");
    Expect("mov [health],eax", at, true, "89 05 FA 00 00 00");
    Expect("mov eax,[game.exe+1100]", at, true, "8B 05 FA 00 00 00");
    Expect("call qword ptr [rax]", at, true, "FF 10");
    Expect("jmp [rax]", at, true, "FF 20");
    Expect("db 90 90 'AB'", at, true, "90 90 41 42");
    Expect("dd (float)1.5", at, true, "00 00 C0 3F");
    Expect("dw 1234", at, true, "34 12");
    Expect("dq 1122334455667788", at, true, "88 77 66 55 44 33 22 11");
    Expect("dq returnhere", at, true, "34 12 00 40 01 00 00 00");
    Expect("dd #10, -1", at, true, "0A 00 00 00 FF FF FF FF");
    Expect("nop 3", at, true, "90 90 90");

    const uint64_t at32 = 0x00400000;
    Expect("mov eax,[00401000]", at32, false, "A1 00 10 40 00");
    Expect("jmp 00401000", at32, false, "E9 FB 0F 00 00");
    Expect("push 12345678", at32, false, "68 78 56 34 12");
    Expect("mov dword ptr [esi+4],(float)100", at32, false, "C7 46 04 00 00 C8 42");
    Expect("mov eax,fs:[30]", at32, false, "64 67 A1 30 00");
    Expect("mov ecx,fs:[30]", at32, false, "64 67 8B 0E 30 00");

    ExpectError("inc [rax]", true, "operand size");
    ExpectError("mov rax,[7FF000000000]", true, "out of reach");
    ExpectError("frobnicate eax", true, "Unknown instruction");
    ExpectError("mov eax,unknownsym", true, "unknownsym");
    ExpectError("mov eax,[rax*3]", true, "scale");
    ExpectError("je short 140009000", true, "short");
    ExpectError("mov eax,rbx", true, "No encoding");

    uint64_t value = 0;
    bool numbers = ParseAsmNumber("10", value) && value == 0x10;
    numbers = numbers && ParseAsmNumber("#10", value) && value == 10;
    numbers = numbers && ParseAsmNumber("-1", value) && value == ~0ull;
    numbers = numbers && ParseAsmNumber("0x7fff", value) && value == 0x7FFF;
    numbers = numbers && ParseAsmNumber("(float)2", value) && value == 0x40000000;
    numbers = numbers && !ParseAsmNumber("rax", value) && !ParseAsmNumber("#1a", value);
    if (!numbers) {
        ++failures;
        std::cerr << "FAIL: number parsing" << std::endl;
    }

    // Multi-line blocks with labels and forward references.
    {
        cortex::services::AssembleRequest request;
        request.address = 0x140001000;
        request.x64 = true;
        request.evaluate = Symbols;
        request.text =
            "start:\n"
            "  xor eax,eax\n"
            "loop:\n"
            "  inc eax\n"
            "  cmp eax,#10\n"
            "  jne loop\n"
            "  jmp done\n"
            "  db 00 00\n"
            "done:\n"
            "  ret";
        cortex::services::AssembleBlockResult result;
        std::string error;
        const bool ok = cortex::services::AssembleBlock(request, result, &error);
        // xor(2) inc(2) cmp(3) jne short(2) jmp short(2) db(2) ret(1) = 14
        if (!ok || result.bytes.size() != 14) {
            ++failures;
            std::cerr << "FAIL: block assembly: " << (ok ? HexBytes(result.bytes) : error) << std::endl;
        }
        uint64_t loopAddress = 0;
        uint64_t doneAddress = 0;
        for (const auto& label : result.labels) {
            if (label.first == "loop") loopAddress = label.second;
            if (label.first == "done") doneAddress = label.second;
        }
        // jne short loop at offset 7 targets loop at offset 2: rel8 -7 (0xF9)
        if (loopAddress != 0x140001002 || doneAddress != 0x14000100D || result.bytes[8] != 0xF9) {
            ++failures;
            std::cerr << "FAIL: block labels loop=" << std::hex << loopAddress << " done=" << doneAddress << std::endl;
        }
    }
    {
        cortex::services::AssembleRequest request;
        request.address = 0x140001000;
        request.x64 = true;
        request.text = "mov eax,1\njmp nowhere";
        cortex::services::AssembleBlockResult result;
        std::string error;
        if (cortex::services::AssembleBlock(request, result, &error) || error.find("line 2") == std::string::npos) {
            ++failures;
            std::cerr << "FAIL: block should report the failing line, got '" << error << "'" << std::endl;
        }
    }

    // Relocation fixes relative branches and RIP-relative operands.
    {
        // call 1400023d0 at 1400023e4 (e8 e7 ff ff ff), moved to 140150000.
        const uint8_t call[] = {0xE8, 0xE7, 0xFF, 0xFF, 0xFF};
        std::vector<uint8_t> out;
        std::string error;
        const bool ok = cortex::services::RelocateCode(call, sizeof(call), 0x1400023E4, 0x140150000, true, out, &error);
        // New call at 140150000 to 1400023d0: rel32 = 1400023d0 - 140150005 = FFEB23CB
        if (!ok || HexBytes(out) != "E8 CB 23 EB FF") {
            ++failures;
            std::cerr << "FAIL: relocate call -> " << (ok ? HexBytes(out) : error) << std::endl;
        }
    }
    {
        // mov rax,[rip+0xe5c90] (48 8b 05 ...) at 1400023e9 -> 140150000.
        const uint8_t rip[] = {0x48, 0x8B, 0x05, 0x90, 0x5C, 0x0E, 0x00};  // reads 1400e8080
        std::vector<uint8_t> out;
        std::string error;
        const bool ok = cortex::services::RelocateCode(rip, sizeof(rip), 0x1400023E9, 0x140150000, true, out, &error);
        // New disp = 1400e8080 - (140150000+7) = FFF98079
        if (!ok || HexBytes(out) != "48 8B 05 79 80 F9 FF") {
            ++failures;
            std::cerr << "FAIL: relocate rip -> " << (ok ? HexBytes(out) : error) << std::endl;
        }
    }
    {
        // sub rsp,28 + call: two instructions relocated together.
        const uint8_t block[] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0xE7, 0xFF, 0xFF, 0xFF};
        std::vector<uint8_t> out;
        std::string error;
        const bool ok = cortex::services::RelocateCode(block, sizeof(block), 0x1400023E0, 0x140150000, true, out, &error);
        // sub unchanged (4) + call re-encoded (5): call rel = 1400023d0-14015000d
        if (!ok || out.size() != 9 || HexBytes(out).substr(0, 11) != "48 83 EC 28") {
            ++failures;
            std::cerr << "FAIL: relocate block -> " << (ok ? HexBytes(out) : error) << std::endl;
        }
    }

    if (failures) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "assembler tests passed" << std::endl;
    return 0;
}
