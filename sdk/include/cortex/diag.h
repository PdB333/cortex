#pragma once

#include "diag_c.h"

#include <cstdint>

namespace cortex::diag {
namespace detail {

struct Api {
    HMODULE cortex = nullptr;
    decltype(&CortexDiagRegisterMod) registerMod = nullptr;
    decltype(&CortexDiagUnregisterMod) unregisterMod = nullptr;
    decltype(&CortexDiagBreadcrumb) breadcrumb = nullptr;
    decltype(&CortexDiagScopeEnter) scopeEnter = nullptr;
    decltype(&CortexDiagScopeExit) scopeExit = nullptr;
    decltype(&CortexDiagValuePointer) valuePointer = nullptr;
    decltype(&CortexDiagValueInt64) valueInt64 = nullptr;
    decltype(&CortexDiagValueUInt64) valueUInt64 = nullptr;
    decltype(&CortexDiagValueDouble) valueDouble = nullptr;
    decltype(&CortexDiagValueBool) valueBool = nullptr;
    decltype(&CortexDiagValueText) valueText = nullptr;
};

inline Api* Resolve() {
    static Api api{};
    static SRWLOCK lock = SRWLOCK_INIT;

    HMODULE loaded = GetModuleHandleW(L"cortex_core.dll");
    if (!loaded) return nullptr;

    AcquireSRWLockShared(&lock);
    const bool ready = api.cortex == loaded && api.breadcrumb;
    ReleaseSRWLockShared(&lock);
    if (ready) return &api;

    AcquireSRWLockExclusive(&lock);
    if (api.cortex != loaded || !api.breadcrumb) {
        Api next{};
        next.cortex = loaded;
#define CORTEX_DIAG_RESOLVE(member, name) \
        next.member = reinterpret_cast<decltype(next.member)>(GetProcAddress(loaded, name))
        CORTEX_DIAG_RESOLVE(registerMod, "CortexDiagRegisterMod");
        CORTEX_DIAG_RESOLVE(unregisterMod, "CortexDiagUnregisterMod");
        CORTEX_DIAG_RESOLVE(breadcrumb, "CortexDiagBreadcrumb");
        CORTEX_DIAG_RESOLVE(scopeEnter, "CortexDiagScopeEnter");
        CORTEX_DIAG_RESOLVE(scopeExit, "CortexDiagScopeExit");
        CORTEX_DIAG_RESOLVE(valuePointer, "CortexDiagValuePointer");
        CORTEX_DIAG_RESOLVE(valueInt64, "CortexDiagValueInt64");
        CORTEX_DIAG_RESOLVE(valueUInt64, "CortexDiagValueUInt64");
        CORTEX_DIAG_RESOLVE(valueDouble, "CortexDiagValueDouble");
        CORTEX_DIAG_RESOLVE(valueBool, "CortexDiagValueBool");
        CORTEX_DIAG_RESOLVE(valueText, "CortexDiagValueText");
#undef CORTEX_DIAG_RESOLVE
        api = next;
    }
    ReleaseSRWLockExclusive(&lock);
    return api.breadcrumb ? &api : nullptr;
}

} // namespace detail

inline bool RegisterMod(HMODULE module, const char* name, const char* version = "",
                        const char* author = "", const char* gitCommit = "",
                        const char* buildId = "", const char* sourceRoot = "",
                        const char* symbolPath = "") {
    auto* api = detail::Resolve();
    if (!api || !api->registerMod) return false;
    CortexDiagModInfo info{};
    info.struct_size = sizeof(info);
    info.abi_version = CORTEX_DIAG_ABI_VERSION;
    info.module = module;
    info.name = name;
    info.version = version;
    info.author = author;
    info.git_commit = gitCommit;
    info.build_id = buildId;
    info.source_root = sourceRoot;
    info.symbol_path = symbolPath;
    return api->registerMod(&info) != FALSE;
}

inline void UnregisterMod(HMODULE module) {
    auto* api = detail::Resolve();
    if (api && api->unregisterMod) api->unregisterMod(module);
}

inline void Breadcrumb(const char* message, const char* category = "user") {
    auto* api = detail::Resolve();
    if (api && api->breadcrumb) api->breadcrumb(category, message);
}

class Scope {
public:
    Scope(const char* name, const char* file, int line) {
        api_ = detail::Resolve();
        if (api_ && api_->scopeEnter) id_ = api_->scopeEnter(name, file, line);
    }
    ~Scope() {
        if (id_ && api_ && api_->scopeExit) api_->scopeExit(id_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&& other) noexcept : api_(other.api_), id_(other.id_) {
        other.api_ = nullptr;
        other.id_ = 0;
    }
private:
    detail::Api* api_ = nullptr;
    uint64_t id_ = 0;
};

inline void Value(const char* name, const void* value) {
    auto* api = detail::Resolve();
    if (api && api->valuePointer) api->valuePointer(name, value);
}
inline void Value(const char* name, int64_t value) {
    auto* api = detail::Resolve();
    if (api && api->valueInt64) api->valueInt64(name, value);
}
inline void Value(const char* name, uint64_t value) {
    auto* api = detail::Resolve();
    if (api && api->valueUInt64) api->valueUInt64(name, value);
}
inline void Value(const char* name, int value) { Value(name, static_cast<int64_t>(value)); }
inline void Value(const char* name, unsigned value) { Value(name, static_cast<uint64_t>(value)); }
inline void Value(const char* name, double value) {
    auto* api = detail::Resolve();
    if (api && api->valueDouble) api->valueDouble(name, value);
}
inline void Value(const char* name, float value) { Value(name, static_cast<double>(value)); }
inline void Value(const char* name, bool value) {
    auto* api = detail::Resolve();
    if (api && api->valueBool) api->valueBool(name, value ? TRUE : FALSE);
}
inline void Value(const char* name, const char* value) {
    auto* api = detail::Resolve();
    if (api && api->valueText) api->valueText(name, value);
}

template <typename T>
inline void Pointer(const char* name, T* value) {
    Value(name, static_cast<const void*>(value));
}

} // namespace cortex::diag

#define CORTEX_DIAG_JOIN_INNER(a, b) a##b
#define CORTEX_DIAG_JOIN(a, b) CORTEX_DIAG_JOIN_INNER(a, b)
#define CORTEX_DIAG_SCOPE(name) \
    ::cortex::diag::Scope CORTEX_DIAG_JOIN(cortex_diag_scope_, __LINE__)((name), __FILE__, __LINE__)
#define CORTEX_DIAG_BREADCRUMB(message) ::cortex::diag::Breadcrumb((message))
#define CORTEX_DIAG_BREADCRUMB_AS(category, message) ::cortex::diag::Breadcrumb((message), (category))
#define CORTEX_DIAG_VALUE(name, value) ::cortex::diag::Value((name), (value))
#define CORTEX_DIAG_POINTER(name, value) ::cortex::diag::Pointer((name), (value))
#define CORTEX_DIAG_REGISTER_MOD(module, name, version, author, commit, build_id, source_root, symbol_path) \
    ::cortex::diag::RegisterMod((module), (name), (version), (author), (commit), (build_id), (source_root), (symbol_path))
#define CORTEX_DIAG_UNREGISTER_MOD(module) ::cortex::diag::UnregisterMod((module))
