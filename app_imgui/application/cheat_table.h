#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

// Cheat Engine cheat tables (.CT): the XML file Cheat Engine saves its
// address list to. Cortex reads and writes the address entries (values,
// strings, byte arrays, pointers, groups); scripts are reported but not run.

struct CheatTableEntry {
    std::string description;
    std::string variableType = "4 Bytes";  // Cheat Engine's names
    std::string address;                   // as written: "game.exe"+1234, 0012ABCD...
    std::vector<uint32_t> offsets;         // in the order they are applied
    int length = 0;                        // String / Array of byte
    bool unicode = false;
    bool showAsHex = false;
    bool showAsSigned = true;
    bool groupHeader = false;
    bool script = false;
    int depth = 0;                         // nesting inside group headers
};

struct CheatTable {
    std::vector<CheatTableEntry> entries;
    size_t scripts = 0;                    // Auto Assembler scripts that were skipped
    size_t unsupported = 0;                // other entry types that were skipped
    std::string luaScript;
};

bool ParseCheatTable(const std::string& xml, CheatTable& table, std::string* error = nullptr);
std::string WriteCheatTable(const CheatTable& table);

bool LoadCheatTable(const std::string& path, CheatTable& table, std::string* error = nullptr);
bool SaveCheatTable(const std::string& path, const CheatTable& table, std::string* error = nullptr);

// "game.exe"+1A2B, game.exe+1A2B or 7FF6A1B20010 -> module name (empty for
// an absolute address) and offset.
bool ParseCheatAddress(const std::string& text, std::string& module, uint64_t& offset);
std::string FormatCheatAddress(const std::string& module, uint64_t offset);

} // namespace cortex::application
