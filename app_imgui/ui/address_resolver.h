#pragma once

#include "ui_context.h"

#include "application/address_expression.h"
#include "services/memory_tools.h"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace cortex::ui {

// Reads a module's export table from the target, for module.Export names.
inline application::TargetSymbols::ExportReader MakeExportReader(target::SessionPtr session) {
    return [session](const target::ModuleInfo& module) {
        std::vector<application::TargetSymbols::ExportEntry> exports;
        if (!session) return exports;
        services::PeImage image;
        const auto read = [&session](uint64_t address, void* buffer, size_t size) {
            return session->ReadMemory(address, buffer, size, nullptr);
        };
        if (!services::ParsePeImage(read, module.base, image)) return exports;
        for (const auto& item : image.exports)
            if (!item.name.empty()) exports.push_back({item.name, module.base + item.rva, item.forwarder});
        return exports;
    };
}

// The symbols of the active target; the module list is refreshed every few
// seconds, or right away with refreshModules.
inline application::TargetSymbols& ContextSymbols(UiContext& context, bool refreshModules = false) {
    auto& state = context.addressSymbols;
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    const std::string targetId = session ? session->Target().id : std::string();
    const auto now = std::chrono::steady_clock::now();
    if (targetId != state.targetId) {
        state.symbols = application::TargetSymbols{};
        state.symbols.SetExportReader(MakeExportReader(session));
        state.targetId = targetId;
        state.refreshed = {};
    }
    state.symbols.SetUserSymbols(context.userSymbols);
    if (refreshModules || state.refreshed.time_since_epoch().count() == 0 ||
        now - state.refreshed > std::chrono::seconds(5)) {
        std::vector<target::ModuleInfo> modules;
        std::string error;
        if (session && context.modules) modules = context.modules->List(&error);
        state.symbols.SetModules(std::move(modules));
        state.refreshed = now;
    }
    return state.symbols;
}

// Evaluates a Cheat Engine address expression against the active target:
// 7FF6A1B20010, game.exe+1A2B, [[game.exe+10]+20]+8, kernel32.Sleep, a
// user-defined symbol... With retry, an unknown name refreshes the module
// list once (a module may just have loaded); refresh loops pass false.
inline bool EvaluateContextAddress(UiContext& context, const std::string& text, uint64_t& address,
                                   std::string* error = nullptr, bool retry = true) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    const size_t pointerSize = session && session->Target().architecture == target::Architecture::X86 ? 4 : 8;
    application::AddressResolver resolver;
    resolver.readPointer = [&](uint64_t at, uint64_t& value) {
        value = 0;
        return session && session->ReadMemory(at, &value, pointerSize, nullptr);
    };
    auto* symbols = &ContextSymbols(context);
    resolver.symbol = [&](const std::string& name, uint64_t& value) { return symbols->Resolve(name, value); };
    if (application::EvaluateAddress(text, resolver, address, error)) return true;
    if (!retry) return false;
    symbols = &ContextSymbols(context, true);
    return application::EvaluateAddress(text, resolver, address, error);
}

} // namespace cortex::ui
