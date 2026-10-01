#pragma once

#include "address_resolver.h"
#include "ui_context.h"

#include "process/remote_memory.h"
#include "services/auto_assembler.h"
#include "services/value_scanner.h"

#include <string>
#include <vector>

namespace cortex::ui {

// Binds an Auto Assembler script to the attached target: reads and writes
// through the session, allocates caves near the injection site, scans for
// byte patterns and resolves module names, exports and user symbols.
inline services::AutoAssembleHost MakeAutoAssemblerHost(UiContext& context) {
    services::AutoAssembleHost host;
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    const uint64_t pid = session ? session->Target().processId : 0;

    host.read = [session](uint64_t address, void* buffer, size_t size) {
        return session && session->ReadMemory(address, buffer, size, nullptr);
    };
    host.write = [pid](uint64_t address, const void* buffer, size_t size) {
        return cortex::remote_memory::WriteCode(pid, address, buffer, size, nullptr);
    };
    host.allocate = [pid](size_t size, uint64_t nearAddress, uint64_t& address, std::string& error) {
        return cortex::remote_memory::Allocate(pid, size, nearAddress, address, &error);
    };
    host.release = [pid](uint64_t address, std::string& error) {
        return cortex::remote_memory::Free(pid, address, &error);
    };
    host.fullAccess = [pid](uint64_t address, size_t size, std::string& error) {
        return cortex::remote_memory::FullAccess(pid, address, size, &error);
    };
    host.createThread = [pid](uint64_t address, std::string& error) {
        return cortex::remote_memory::CreateThread(pid, address, 0, &error);
    };
    host.symbol = [&context](const std::string& name, uint64_t& value) {
        return ContextSymbols(context).Resolve(name, value);
    };
    host.moduleRange = [&context](const std::string& name, uint64_t& base, uint64_t& size) {
        for (const auto& module : ContextSymbols(context).Modules()) {
            std::string left = module.name;
            std::string right = name;
            for (auto& ch : left) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (auto& ch : right) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (left != right) continue;
            base = module.base;
            size = module.size;
            return true;
        }
        return false;
    };
    host.scan = [session](const std::string& pattern, uint64_t start, uint64_t stop,
                          std::vector<uint64_t>& matches, std::string& error) {
        if (!session) {
            error = "no target";
            return false;
        }
        services::ScanQuery query;
        query.type = services::ScanDataType::ByteArray;
        query.value = pattern;
        services::ScanOptions options;
        options.start = start;
        options.stop = stop;
        options.writable = services::ScanTristate::Any;
        options.copyOnWrite = services::ScanTristate::Any;
        options.executable = services::ScanTristate::Any;
        options.includePrivate = true;
        options.includeImage = true;
        options.includeMapped = true;
        options.maxResults = 64;
        const auto state = services::ValueScanner::FirstScan(session, query, options, &error);
        if (!state) return false;
        matches.assign(state->addresses.begin(), state->addresses.end());
        return true;
    };
    return host;
}

} // namespace cortex::ui
