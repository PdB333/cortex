#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace cortex::application {

// Cheat Engine style address expressions:
//   7FF6A1B20010            hexadecimal (0x prefix optional)
//   game.exe+1A2B0          module or symbol plus hex offset
//   "Tutorial-i386.exe"+10  quoted names may contain any character
//   [[game.exe+10]+20]+8    [x] reads the pointer stored at x
//   base+10*4-8             + - * and parentheses
// A token made only of hex digits is a number; anything else is a name.
struct AddressResolver {
    // Module base or symbol address by name; false when unknown.
    std::function<bool(const std::string& name, uint64_t& value)> symbol;
    // Reads the pointer stored at an address; false when unreadable.
    std::function<bool(uint64_t address, uint64_t& value)> readPointer;
};

bool EvaluateAddress(const std::string& text, const AddressResolver& resolver, uint64_t& result,
                     std::string* error = nullptr);

} // namespace cortex::application
