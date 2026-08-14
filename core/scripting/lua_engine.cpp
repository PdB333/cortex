#include "lua_engine.h"
#include "../memory/memory.h"
#include "../process/address.h"
#include "../process/modules.h"
#include "../overlay/overlay.h"
#include "../config.h"
#include "../action/action.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <windows.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <chrono>
#include <vector>
#include <algorithm>

namespace scripting {

namespace {

using json = nlohmann::json;
using clock_type = std::chrono::steady_clock;

constexpr size_t kMaxCodeBytes = 256u * 1024u;
constexpr size_t kMaxOutputBytes = 1024u * 1024u;
constexpr size_t kMaxReadBytes = 1024u * 1024u;
constexpr int kDefaultTimeoutMs = 5000;
constexpr int kMaxTimeoutMs = 60000;

thread_local std::string* g_output = nullptr;
thread_local clock_type::time_point g_deadline{};

void TimeoutHook(lua_State* L, lua_Debug*) {
    if (clock_type::now() >= g_deadline) luaL_error(L, "script_timeout");
}

void OpenSandboxLibraries(lua_State* L) {
    struct Library { const char* name; lua_CFunction open; };
    const Library libraries[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8}
    };
    for (const auto& lib : libraries) {
        luaL_requiref(L, lib.name, lib.open, 1);
        lua_pop(L, 1);
    }
    // io/os/package/debug are deliberately never opened. Removing loadfile
    // and dofile also prevents filesystem execution through the base library.
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "dofile");
}

uintptr_t AddrFromLua(lua_State* L, int idx) {
    json j;
    if (lua_isinteger(L, idx))      j = static_cast<uint64_t>(lua_tointeger(L, idx));
    else if (lua_isnumber(L, idx))  j = static_cast<uint64_t>(lua_tonumber(L, idx));
    else if (lua_isstring(L, idx))  j = std::string(lua_tostring(L, idx));
    else                            return 0;
    std::string err;
    return process::ResolveAddress(j, &err);
}

int L_memory_read(lua_State* L) {
    uintptr_t addr = AddrFromLua(L, 1);
    const char* type = luaL_checkstring(L, 2);
    if (!addr) { lua_pushnil(L); lua_pushstring(L, "invalid_address"); return 2; }

    size_t sz = 0;
    if      (!std::strcmp(type,"u8")  || !std::strcmp(type,"i8"))    sz = 1;
    else if (!std::strcmp(type,"u16") || !std::strcmp(type,"i16"))   sz = 2;
    else if (!std::strcmp(type,"u32") || !std::strcmp(type,"i32") || !std::strcmp(type,"float"))  sz = 4;
    else if (!std::strcmp(type,"u64") || !std::strcmp(type,"i64") || !std::strcmp(type,"double")) sz = 8;
    else { lua_pushnil(L); lua_pushstring(L, "invalid_type"); return 2; }

    std::vector<uint8_t> buf;
    if (!memory::ReadBytes(addr, sz, buf)) { lua_pushnil(L); lua_pushstring(L, "read_failed"); return 2; }

    if      (!std::strcmp(type,"u8"))     lua_pushinteger(L, *reinterpret_cast<uint8_t*>(buf.data()));
    else if (!std::strcmp(type,"i8"))     lua_pushinteger(L, *reinterpret_cast<int8_t*>(buf.data()));
    else if (!std::strcmp(type,"u16"))    lua_pushinteger(L, *reinterpret_cast<uint16_t*>(buf.data()));
    else if (!std::strcmp(type,"i16"))    lua_pushinteger(L, *reinterpret_cast<int16_t*>(buf.data()));
    else if (!std::strcmp(type,"u32"))    lua_pushinteger(L, *reinterpret_cast<uint32_t*>(buf.data()));
    else if (!std::strcmp(type,"i32"))    lua_pushinteger(L, *reinterpret_cast<int32_t*>(buf.data()));
    else if (!std::strcmp(type,"u64"))    lua_pushinteger(L, static_cast<lua_Integer>(*reinterpret_cast<uint64_t*>(buf.data())));
    else if (!std::strcmp(type,"i64"))    lua_pushinteger(L, *reinterpret_cast<int64_t*>(buf.data()));
    else if (!std::strcmp(type,"float"))  lua_pushnumber(L, *reinterpret_cast<float*>(buf.data()));
    else                                  lua_pushnumber(L, *reinterpret_cast<double*>(buf.data()));
    return 1;
}

int L_memory_write(lua_State* L) {
    uintptr_t addr = AddrFromLua(L, 1);
    const char* type = luaL_checkstring(L, 2);
    if (!addr) { lua_pushboolean(L, 0); return 1; }
    std::vector<uint8_t> buf;

    auto pushInt = [&](auto tag) {
        using T = decltype(tag);
        T v = static_cast<T>(luaL_checkinteger(L, 3));
        buf.resize(sizeof(T));
        std::memcpy(buf.data(), &v, sizeof(T));
    };
    auto pushFlt = [&](auto tag) {
        using T = decltype(tag);
        T v = static_cast<T>(luaL_checknumber(L, 3));
        buf.resize(sizeof(T));
        std::memcpy(buf.data(), &v, sizeof(T));
    };

    if      (!std::strcmp(type,"u8"))     pushInt(uint8_t{});
    else if (!std::strcmp(type,"i8"))     pushInt(int8_t{});
    else if (!std::strcmp(type,"u16"))    pushInt(uint16_t{});
    else if (!std::strcmp(type,"i16"))    pushInt(int16_t{});
    else if (!std::strcmp(type,"u32"))    pushInt(uint32_t{});
    else if (!std::strcmp(type,"i32"))    pushInt(int32_t{});
    else if (!std::strcmp(type,"u64"))    pushInt(uint64_t{});
    else if (!std::strcmp(type,"i64"))    pushInt(int64_t{});
    else if (!std::strcmp(type,"float"))  pushFlt(float{});
    else if (!std::strcmp(type,"double")) pushFlt(double{});
    else { lua_pushboolean(L, 0); return 1; }

    auto mutation = action::LockMutations();
    std::vector<uint8_t> original;
    if (!memory::ReadBytes(addr, buf.size(), original) || !memory::WriteBytes(addr, buf)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    action::Record("lua memory_write " + process::DescribeAddress(addr),
                   [addr, original] { return memory::WriteBytes(addr, original); });
    lua_pushboolean(L, 1);
    return 1;
}

int L_memory_read_bytes(lua_State* L) {
    uintptr_t addr = AddrFromLua(L, 1);
    lua_Integer sz = luaL_checkinteger(L, 2);
    if (!addr || sz <= 0 || static_cast<uint64_t>(sz) > kMaxReadBytes) {
        lua_pushnil(L); lua_pushstring(L, "read_size_out_of_range"); return 2;
    }
    std::vector<uint8_t> buf;
    if (!memory::ReadBytes(addr, static_cast<size_t>(sz), buf)) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, reinterpret_cast<const char*>(buf.data()), buf.size());
    return 1;
}

int L_module_base(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(process::GetModuleBase(name)));
    return 1;
}

int L_resolve(lua_State* L) { lua_pushinteger(L, static_cast<lua_Integer>(AddrFromLua(L, 1))); return 1; }
int L_describe(lua_State* L) { lua_pushstring(L, process::DescribeAddress(AddrFromLua(L, 1)).c_str()); return 1; }

int L_log(lua_State* L) {
    const char* m = luaL_checkstring(L, 1);
    overlay::LogApiCall(std::string("[lua] ") + m);
    return 0;
}

int L_sleep(lua_State* L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;
    const auto now = clock_type::now();
    if (now >= g_deadline) return luaL_error(L, "script_timeout");
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(g_deadline - now).count();
    if (ms > remaining) return luaL_error(L, "script_timeout");
    Sleep(static_cast<DWORD>(ms));
    return 0;
}

int L_print(lua_State* L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (g_output) {
            if (i > 1 && g_output->size() < kMaxOutputBytes) g_output->push_back('\t');
            const size_t room = g_output->size() < kMaxOutputBytes ? kMaxOutputBytes - g_output->size() : 0;
            g_output->append(s, (std::min)(len, room));
        }
        lua_pop(L, 1);
    }
    if (g_output && g_output->size() < kMaxOutputBytes) g_output->push_back('\n');
    return 0;
}

void RegisterCortexTable(lua_State* L) {
    lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, L_memory_read);        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, L_memory_write);       lua_setfield(L, -2, "write");
        lua_pushcfunction(L, L_memory_read_bytes);  lua_setfield(L, -2, "read_bytes");
        lua_setfield(L, -2, "memory");
    lua_pushcfunction(L, L_module_base); lua_setfield(L, -2, "module_base");
    lua_pushcfunction(L, L_resolve); lua_setfield(L, -2, "resolve");
    lua_pushcfunction(L, L_describe); lua_setfield(L, -2, "describe");
    lua_pushcfunction(L, L_log); lua_setfield(L, -2, "log");
    lua_pushcfunction(L, L_sleep); lua_setfield(L, -2, "sleep");
    lua_setglobal(L, "cortex");
    lua_pushcfunction(L, L_print); lua_setglobal(L, "print");
}

} // namespace

std::string GetScriptsDir() {
    std::string dir = config::GetModuleDir() + "\\cortex_scripts";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

ExecResult Exec(const std::string& code, int timeoutMs) {
    ExecResult r{false, "", "", ""};
    if (code.size() > kMaxCodeBytes) { r.error = "script_too_large"; return r; }
    if (timeoutMs <= 0) timeoutMs = kDefaultTimeoutMs;
    if (timeoutMs > kMaxTimeoutMs) { r.error = "timeout_out_of_range"; return r; }

    std::string output;
    g_output = &output;
    g_deadline = clock_type::now() + std::chrono::milliseconds(timeoutMs);

    lua_State* L = luaL_newstate();
    if (!L) { r.error = "lua_newstate_failed"; g_output = nullptr; return r; }
    OpenSandboxLibraries(L);
    RegisterCortexTable(L);
    lua_sethook(L, TimeoutHook, LUA_MASKCOUNT, 1000);

    if (luaL_loadbuffer(L, code.data(), code.size(), "cortex_script") != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        r.error = e ? e : "load_failed";
        lua_close(L); g_output = nullptr; r.output = std::move(output); return r;
    }
    int status = lua_pcall(L, 0, 1, 0);
    if (status != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        r.error = e ? e : "runtime_error";
    } else {
        r.ok = true;
        if (!lua_isnil(L, -1)) {
            size_t len = 0;
            const char* s = luaL_tolstring(L, -1, &len);
            if (s) r.result.assign(s, (std::min)(len, kMaxOutputBytes));
            lua_pop(L, 1);
        }
    }
    lua_close(L);
    g_output = nullptr;
    r.output = std::move(output);
    return r;
}

} // namespace scripting
