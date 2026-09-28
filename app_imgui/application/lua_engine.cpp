#include "lua_engine.h"
#include "address_expression.h"

#include "process/process_control.h"
#include "services/value_scanner.h"

// Lua is compiled as C++ for the desktop, so an error raised inside these
// functions unwinds the C++ objects on the way instead of longjmp-ing past
// their destructors.
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>

namespace cortex::application {
namespace {

using clock_type = std::chrono::steady_clock;

struct Context {
    const LuaRunOptions* options = nullptr;
    clock_type::time_point deadline{};
    bool hasDeadline = false;
    bool paused = false;
    std::vector<target::ModuleInfo> modules;
    bool modulesLoaded = false;
    TargetSymbols symbols;
};

const char kContextKey = 0;

Context& Ctx(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kContextKey);
    auto* context = static_cast<Context*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *context;
}

bool Stopped(Context& context) {
    if (context.options->cancelled && context.options->cancelled->load()) return true;
    return context.hasDeadline && clock_type::now() >= context.deadline;
}

void Hook(lua_State* L, lua_Debug*) {
    Context& context = Ctx(L);
    if (context.options->cancelled && context.options->cancelled->load()) luaL_error(L, "script stopped");
    if (context.hasDeadline && clock_type::now() >= context.deadline) luaL_error(L, "script time limit reached");
}

const std::vector<target::ModuleInfo>& Modules(Context& context) {
    if (!context.modulesLoaded) {
        if (context.options->modules) context.modules = context.options->modules();
        context.modulesLoaded = true;
        context.symbols.SetModules(context.modules);
        context.symbols.SetExportReader(context.options->exports);
        context.symbols.SetUserSymbols(context.options->userSymbols);
    }
    return context.modules;
}

bool Is64(Context& context) {
    return context.options->session->Target().architecture != target::Architecture::X86;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

// An address argument: a number, or a Cheat Engine address expression.
uint64_t ToAddress(lua_State* L, int index) {
    if (lua_isinteger(L, index)) return static_cast<uint64_t>(lua_tointeger(L, index));
    if (lua_type(L, index) == LUA_TNUMBER) return static_cast<uint64_t>(lua_tonumber(L, index));
    const char* text = luaL_checkstring(L, index);
    Context& context = Ctx(L);
    AddressResolver resolver;
    resolver.symbol = [&context](const std::string& name, uint64_t& value) {
        Modules(context);
        return context.symbols.Resolve(name, value);
    };
    resolver.readPointer = [&context](uint64_t address, uint64_t& value) {
        value = 0;
        return context.options->session->ReadMemory(address, &value, Is64(context) ? 8 : 4, nullptr);
    };
    uint64_t address = 0;
    std::string error;
    if (!EvaluateAddress(text, resolver, address, &error)) luaL_error(L, "%s", error.c_str());
    return address;
}

void RequireWrites(lua_State* L) {
    if (!Ctx(L).options->allowWrites) luaL_error(L, "writes are not allowed: switch Cortex to Writes allowed");
}

bool ReadRaw(lua_State* L, uint64_t address, void* buffer, size_t size) {
    return Ctx(L).options->session->ReadMemory(address, buffer, size, nullptr);
}

void WriteRaw(lua_State* L, uint64_t address, const void* buffer, size_t size) {
    RequireWrites(L);
    if (!Ctx(L).options->session->WriteMemory(address, buffer, size, nullptr))
        luaL_error(L, "cannot write %d byte(s) at %p", static_cast<int>(size), reinterpret_cast<void*>(address));
}

// ---------------------------------------------------------------- reads

template <typename T>
int ReadNumber(lua_State* L) {
    const uint64_t address = ToAddress(L, 1);
    T value{};
    if (!ReadRaw(L, address, &value, sizeof(T))) {
        lua_pushnil(L);
        return 1;
    }
    if constexpr (std::is_floating_point_v<T>) {
        lua_pushnumber(L, static_cast<lua_Number>(value));
    } else {
        const bool isSigned = lua_toboolean(L, 2) != 0;
        using S = std::make_signed_t<T>;
        lua_pushinteger(L, isSigned ? static_cast<lua_Integer>(static_cast<S>(value))
                                    : static_cast<lua_Integer>(value));
    }
    return 1;
}

int ReadPointer(lua_State* L) {
    const uint64_t address = ToAddress(L, 1);
    uint64_t value = 0;
    if (!ReadRaw(L, address, &value, Is64(Ctx(L)) ? 8 : 4)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    return 1;
}

int ReadBytes(lua_State* L) {
    const uint64_t address = ToAddress(L, 1);
    const lua_Integer count = luaL_optinteger(L, 2, 1);
    const bool asTable = lua_toboolean(L, 3) != 0;
    if (count < 1 || count > 16 * 1024 * 1024) return luaL_error(L, "readBytes: count must be 1..16777216");
    std::vector<uint8_t> bytes(static_cast<size_t>(count));
    if (!ReadRaw(L, address, bytes.data(), bytes.size())) {
        lua_pushnil(L);
        return 1;
    }
    if (asTable) {
        lua_createtable(L, static_cast<int>(count), 0);
        for (size_t i = 0; i < bytes.size(); ++i) {
            lua_pushinteger(L, bytes[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }
    if (count > 200) return luaL_error(L, "readBytes: use returnAsTable for more than 200 bytes");
    luaL_checkstack(L, static_cast<int>(count), "readBytes");
    for (const auto byte : bytes) lua_pushinteger(L, byte);
    return static_cast<int>(count);
}

int ReadString(lua_State* L) {
    const uint64_t address = ToAddress(L, 1);
    const lua_Integer maxLength = luaL_optinteger(L, 2, 1000);
    const bool wide = lua_toboolean(L, 3) != 0;
    if (maxLength < 1 || maxLength > 1024 * 1024) return luaL_error(L, "readString: bad length");
    const size_t unit = wide ? 2 : 1;
    std::vector<uint8_t> bytes(static_cast<size_t>(maxLength) * unit);
    size_t size = bytes.size();
    while (size >= unit && !ReadRaw(L, address, bytes.data(), size)) size /= 2;
    if (size < unit) {
        lua_pushnil(L);
        return 1;
    }
    std::string text;
    for (size_t i = 0; i + unit <= size; i += unit) {
        const uint32_t code = wide ? (bytes[i] | (static_cast<uint32_t>(bytes[i + 1]) << 8)) : bytes[i];
        if (code == 0) break;
        if (code < 0x80) {
            text += static_cast<char>(code);
        } else if (code < 0x800) {
            text += static_cast<char>(0xC0 | (code >> 6));
            text += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            text += static_cast<char>(0xE0 | (code >> 12));
            text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    lua_pushstring(L, text.c_str());
    return 1;
}

// ---------------------------------------------------------------- writes

template <typename T>
int WriteNumber(lua_State* L) {
    const uint64_t address = ToAddress(L, 1);
    T value{};
    if constexpr (std::is_floating_point_v<T>) {
        value = static_cast<T>(luaL_checknumber(L, 2));
    } else {
        value = static_cast<T>(lua_isinteger(L, 2) ? lua_tointeger(L, 2)
                                                   : static_cast<lua_Integer>(luaL_checknumber(L, 2)));
    }
    WriteRaw(L, address, &value, sizeof(T));
    lua_pushboolean(L, 1);
    return 1;
}

int WritePointer(lua_State* L) {
    const uint64_t address = ToAddress(L, 1);
    const uint64_t value = static_cast<uint64_t>(luaL_checkinteger(L, 2));
    WriteRaw(L, address, &value, Is64(Ctx(L)) ? 8 : 4);
    lua_pushboolean(L, 1);
    return 1;
}

int WriteBytes(lua_State* L) {
    const uint64_t address = ToAddress(L, 1);
    std::vector<uint8_t> bytes;
    if (lua_istable(L, 2)) {
        const lua_Integer length = static_cast<lua_Integer>(lua_rawlen(L, 2));
        for (lua_Integer i = 1; i <= length; ++i) {
            lua_rawgeti(L, 2, i);
            bytes.push_back(static_cast<uint8_t>(luaL_checkinteger(L, -1)));
            lua_pop(L, 1);
        }
    } else {
        for (int i = 2; i <= lua_gettop(L); ++i) bytes.push_back(static_cast<uint8_t>(luaL_checkinteger(L, i)));
    }
    if (bytes.empty()) return luaL_error(L, "writeBytes: no bytes");
    WriteRaw(L, address, bytes.data(), bytes.size());
    lua_pushboolean(L, 1);
    return 1;
}

int WriteString(lua_State* L) {
    const uint64_t address = ToAddress(L, 1);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    const bool wide = lua_toboolean(L, 3) != 0;
    std::vector<uint8_t> bytes;
    if (!wide) {
        bytes.assign(text, text + length);
    } else {
        std::vector<uint8_t> encoded;
        if (!services::ValueScanner::Encode(std::string(text, length), services::ScanDataType::String, false, true,
                                            encoded, nullptr))
            return luaL_error(L, "writeString: cannot encode the text");
        bytes = std::move(encoded);
    }
    WriteRaw(L, address, bytes.data(), bytes.size());
    lua_pushboolean(L, 1);
    return 1;
}

// ---------------------------------------------------------------- addresses and scans

int GetAddress(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(ToAddress(L, 1)));
    return 1;
}

int GetAddressSafe(lua_State* L) {
    lua_pushcfunction(L, GetAddress);
    lua_pushvalue(L, 1);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
    return 1;
}

// registerSymbol(name, address): the name works in every address
// expression, in scripts and in the address list.
int RegisterSymbol(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    const uint64_t address = ToAddress(L, 2);
    Context& context = Ctx(L);
    if (!context.options->userSymbols) return luaL_error(L, "registerSymbol is not available here");
    if (name.empty() || name.find_first_of(" +-*[]()\"'") != std::string::npos)
        return luaL_error(L, "registerSymbol: invalid symbol name '%s'", name.c_str());
    context.options->userSymbols->Set(name, address);
    return 0;
}

int UnregisterSymbol(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    Context& context = Ctx(L);
    if (context.options->userSymbols) context.options->userSymbols->Remove(name);
    return 0;
}

const target::ModuleInfo* ModuleAt(Context& context, uint64_t address) {
    for (const auto& module : Modules(context))
        if (address >= module.base && address < module.base + module.size) return &module;
    return nullptr;
}

// getNameFromAddress(address): "game.exe+1A2B", or the hexadecimal address.
int GetNameFromAddress(lua_State* L) {
    const uint64_t address = ToAddress(L, 1);
    char buffer[64] = {};
    if (const auto* module = ModuleAt(Ctx(L), address)) {
        std::snprintf(buffer, sizeof(buffer), "+%llX", static_cast<unsigned long long>(address - module->base));
        lua_pushstring(L, (module->name + buffer).c_str());
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(address));
        lua_pushstring(L, buffer);
    }
    return 1;
}

int InModule(lua_State* L) {
    lua_pushboolean(L, ModuleAt(Ctx(L), ToAddress(L, 1)) != nullptr);
    return 1;
}

// A module loaded from the Windows directory.
int InSystemModule(lua_State* L) {
    const auto* module = ModuleAt(Ctx(L), ToAddress(L, 1));
    const std::string path = module ? Lower(module->path) : std::string();
    lua_pushboolean(L, path.find("\\windows\\") != std::string::npos || path.find("/windows/") != std::string::npos);
    return 1;
}

services::ScanStatePtr Scan(lua_State* L, const char* pattern, uint64_t start, uint64_t stop, size_t limit) {
    Context& context = Ctx(L);
    services::ScanQuery query;
    query.type = services::ScanDataType::ByteArray;
    query.value = pattern;
    services::ScanOptions options;
    options.start = start;
    options.stop = stop;
    options.writable = services::ScanTristate::Any;
    options.copyOnWrite = services::ScanTristate::Any;
    options.maxResults = limit;
    std::string error;
    auto state = services::ValueScanner::FirstScan(context.options->session, query, options, &error,
                                                   context.options->cancelled);
    if (!state) luaL_error(L, "AOBScan: %s", error.c_str());
    return state;
}

std::string Hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(value));
    return buffer;
}

int StringListCount(lua_State* L) {
    lua_getfield(L, 1, "Count");
    return 1;
}

int StringListGet(lua_State* L) {
    lua_rawgeti(L, 1, luaL_checkinteger(L, 2));
    return 1;
}

int StringListDestroy(lua_State*) {
    return 0;
}

// Cheat Engine returns a StringList: results[0] .. results[Count - 1],
// getCount(), getString(i) and destroy().
int AobScan(lua_State* L) {
    const char* pattern = luaL_checkstring(L, 1);
    const auto state = Scan(L, pattern, 0, ~0ull, 100000);
    if (state->Size() == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, static_cast<int>(state->Size()), 4);
    for (size_t i = 0; i < state->Size(); ++i) {
        lua_pushstring(L, Hex(state->addresses[i]).c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i));
    }
    lua_pushinteger(L, static_cast<lua_Integer>(state->Size()));
    lua_setfield(L, -2, "Count");
    lua_pushcfunction(L, StringListCount);
    lua_setfield(L, -2, "getCount");
    lua_pushcfunction(L, StringListGet);
    lua_setfield(L, -2, "getString");
    lua_pushcfunction(L, StringListDestroy);
    lua_setfield(L, -2, "destroy");
    return 1;
}

int AobScanUnique(lua_State* L) {
    const auto state = Scan(L, luaL_checkstring(L, 1), 0, ~0ull, 2);
    if (state->Size() != 1) lua_pushnil(L);
    else lua_pushinteger(L, static_cast<lua_Integer>(state->addresses.front()));
    return 1;
}

int AobScanModuleUnique(lua_State* L) {
    const std::string name = Lower(luaL_checkstring(L, 1));
    for (const auto& module : Modules(Ctx(L))) {
        if (Lower(module.name) != name) continue;
        const auto state = Scan(L, luaL_checkstring(L, 2), module.base, module.base + module.size, 2);
        if (state->Size() != 1) lua_pushnil(L);
        else lua_pushinteger(L, static_cast<lua_Integer>(state->addresses.front()));
        return 1;
    }
    return luaL_error(L, "AOBScanModuleUnique: module %s is not loaded", name.c_str());
}

int EnumModules(lua_State* L) {
    const auto& modules = Modules(Ctx(L));
    lua_createtable(L, static_cast<int>(modules.size()), 0);
    const bool is64 = Is64(Ctx(L));
    for (size_t i = 0; i < modules.size(); ++i) {
        lua_createtable(L, 0, 5);
        lua_pushstring(L, modules[i].name.c_str());
        lua_setfield(L, -2, "Name");
        lua_pushinteger(L, static_cast<lua_Integer>(modules[i].base));
        lua_setfield(L, -2, "Address");
        lua_pushinteger(L, static_cast<lua_Integer>(modules[i].size));
        lua_setfield(L, -2, "Size");
        lua_pushboolean(L, is64);
        lua_setfield(L, -2, "Is64Bit");
        lua_pushstring(L, modules[i].path.c_str());
        lua_setfield(L, -2, "PathToFile");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int GetModuleSize(lua_State* L) {
    const std::string name = Lower(luaL_checkstring(L, 1));
    for (const auto& module : Modules(Ctx(L))) {
        if (Lower(module.name) != name) continue;
        lua_pushinteger(L, static_cast<lua_Integer>(module.size));
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

// ---------------------------------------------------------------- process and misc

int GetProcessId(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(Ctx(L).options->session->Target().processId));
    return 1;
}

int GetProcessName(lua_State* L) {
    lua_pushstring(L, Ctx(L).options->session->Target().name.c_str());
    return 1;
}

int TargetIs64Bit(lua_State* L) {
    lua_pushboolean(L, Is64(Ctx(L)));
    return 1;
}

int Pause(lua_State* L) {
    RequireWrites(L);
    Context& context = Ctx(L);
    if (!context.paused) {
        std::string error;
        if (!process_control::Suspend(context.options->session->Target().processId, &error))
            return luaL_error(L, "pause: %s", error.c_str());
        context.paused = true;
    }
    return 0;
}

int Unpause(lua_State* L) {
    Context& context = Ctx(L);
    if (context.paused) {
        process_control::Resume(context.options->session->Target().processId);
        context.paused = false;
    }
    return 0;
}

int Sleep(lua_State* L) {
    const lua_Integer ms = luaL_checkinteger(L, 1);
    Context& context = Ctx(L);
    const auto until = clock_type::now() + std::chrono::milliseconds(std::max<lua_Integer>(0, ms));
    while (clock_type::now() < until) {
        if (Stopped(context)) return luaL_error(L, "script stopped");
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<lua_Integer>(10, std::max<lua_Integer>(1, ms))));
    }
    return 0;
}

int GetTickCount(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(std::chrono::duration_cast<std::chrono::milliseconds>(
                           clock_type::now().time_since_epoch()).count()));
    return 1;
}

std::string Join(lua_State* L, int first) {
    std::string line;
    for (int i = first; i <= lua_gettop(L); ++i) {
        if (i > first) line += '\t';
        size_t length = 0;
        const char* text = luaL_tolstring(L, i, &length);
        line.append(text, length);
        lua_pop(L, 1);
    }
    return line;
}

int Print(lua_State* L) {
    const std::string line = Join(L, 1);
    if (Ctx(L).options->output) Ctx(L).options->output(line);
    return 0;
}

int ShowMessage(lua_State* L) {
    return Print(L);
}

// ---------------------------------------------------------------- byte tables

template <typename T>
int ToByteTable(lua_State* L) {
    T value{};
    if constexpr (std::is_floating_point_v<T>) value = static_cast<T>(luaL_checknumber(L, 1));
    else value = static_cast<T>(luaL_checkinteger(L, 1));
    uint8_t bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    lua_createtable(L, sizeof(T), 0);
    for (size_t i = 0; i < sizeof(T); ++i) {
        lua_pushinteger(L, bytes[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

template <typename T>
int FromByteTable(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    uint8_t bytes[sizeof(T)] = {};
    for (size_t i = 0; i < sizeof(T); ++i) {
        lua_rawgeti(L, 1, static_cast<lua_Integer>(i + 1));
        bytes[i] = static_cast<uint8_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
    T value{};
    std::memcpy(&value, bytes, sizeof(T));
    if constexpr (std::is_floating_point_v<T>) lua_pushnumber(L, value);
    else lua_pushinteger(L, static_cast<lua_Integer>(value));
    return 1;
}

int StringToByteTable(lua_State* L) {
    size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    lua_createtable(L, static_cast<int>(length), 0);
    for (size_t i = 0; i < length; ++i) {
        lua_pushinteger(L, static_cast<unsigned char>(text[i]));
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int ByteTableToString(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string text;
    const lua_Integer length = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= length; ++i) {
        lua_rawgeti(L, 1, i);
        text += static_cast<char>(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

struct Function {
    const char* name;
    lua_CFunction function;
};

const Function kFunctions[] = {
    {"readByte", ReadNumber<uint8_t>},
    {"readSmallInteger", ReadNumber<uint16_t>},
    {"readShortInteger", ReadNumber<uint16_t>},
    {"readInteger", ReadNumber<uint32_t>},
    {"readQword", ReadNumber<uint64_t>},
    {"readFloat", ReadNumber<float>},
    {"readDouble", ReadNumber<double>},
    {"readPointer", ReadPointer},
    {"readBytes", ReadBytes},
    {"readString", ReadString},
    {"writeByte", WriteNumber<uint8_t>},
    {"writeSmallInteger", WriteNumber<uint16_t>},
    {"writeShortInteger", WriteNumber<uint16_t>},
    {"writeInteger", WriteNumber<uint32_t>},
    {"writeQword", WriteNumber<uint64_t>},
    {"writeFloat", WriteNumber<float>},
    {"writeDouble", WriteNumber<double>},
    {"writePointer", WritePointer},
    {"writeBytes", WriteBytes},
    {"writeString", WriteString},
    {"getAddress", GetAddress},
    {"getAddressSafe", GetAddressSafe},
    {"registerSymbol", RegisterSymbol},
    {"unregisterSymbol", UnregisterSymbol},
    {"getNameFromAddress", GetNameFromAddress},
    {"inModule", InModule},
    {"inSystemModule", InSystemModule},
    {"AOBScan", AobScan},
    {"AOBScanUnique", AobScanUnique},
    {"AOBScanModuleUnique", AobScanModuleUnique},
    {"enumModules", EnumModules},
    {"getModuleSize", GetModuleSize},
    {"getProcessId", GetProcessId},
    {"getOpenedProcessID", GetProcessId},
    {"getProcessName", GetProcessName},
    {"targetIs64Bit", TargetIs64Bit},
    {"pause", Pause},
    {"unpause", Unpause},
    {"sleep", Sleep},
    {"getTickCount", GetTickCount},
    {"print", Print},
    {"showMessage", ShowMessage},
    {"wordToByteTable", ToByteTable<uint16_t>},
    {"dwordToByteTable", ToByteTable<uint32_t>},
    {"qwordToByteTable", ToByteTable<uint64_t>},
    {"floatToByteTable", ToByteTable<float>},
    {"doubleToByteTable", ToByteTable<double>},
    {"byteTableToWord", FromByteTable<uint16_t>},
    {"byteTableToDword", FromByteTable<uint32_t>},
    {"byteTableToQword", FromByteTable<uint64_t>},
    {"byteTableToFloat", FromByteTable<float>},
    {"byteTableToDouble", FromByteTable<double>},
    {"stringToByteTable", StringToByteTable},
    {"byteTableToString", ByteTableToString},
};

void OpenLibraries(lua_State* L) {
    const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base},     {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (const auto& library : libraries) {
        luaL_requiref(L, library.name, library.func, 1);
        lua_pop(L, 1);
    }
    // No io, os, package or debug; no loading code from files.
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    for (const auto& function : kFunctions) {
        lua_pushcfunction(L, function.function);
        lua_setglobal(L, function.name);
    }
}

} // namespace

const std::vector<const char*>& DesktopLuaFunctions() {
    static const std::vector<const char*> names = [] {
        std::vector<const char*> list;
        for (const auto& function : kFunctions) list.push_back(function.name);
        return list;
    }();
    return names;
}

LuaRunResult RunDesktopLua(const std::string& source, const LuaRunOptions& options) {
    LuaRunResult result;
    const auto started = clock_type::now();
    if (!options.session || !options.session->Alive()) {
        result.error = "Select a process first";
        return result;
    }
    Context context;
    context.options = &options;
    if (options.timeoutMs > 0) {
        context.hasDeadline = true;
        context.deadline = started + std::chrono::milliseconds(options.timeoutMs);
    }
    lua_State* L = luaL_newstate();
    if (!L) {
        result.error = "Lua could not start";
        return result;
    }
    lua_pushlightuserdata(L, &context);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kContextKey);
    OpenLibraries(L);
    lua_sethook(L, Hook, LUA_MASKCOUNT, 1000);

    if (luaL_loadbufferx(L, source.data(), source.size(), "=script", "t") != LUA_OK) {
        result.error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
    } else {
        const int base = lua_gettop(L) - 1;
        if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
            result.error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        } else {
            result.ok = true;
            for (int i = base + 1; i <= lua_gettop(L); ++i) {
                if (i > base + 1) result.returned += '\t';
                size_t length = 0;
                const char* text = luaL_tolstring(L, i, &length);
                result.returned.append(text, length);
                lua_pop(L, 1);
            }
        }
    }
    if (context.paused) process_control::Resume(options.session->Target().processId);
    lua_close(L);
    result.milliseconds = std::chrono::duration<double, std::milli>(clock_type::now() - started).count();
    return result;
}

} // namespace cortex::application
