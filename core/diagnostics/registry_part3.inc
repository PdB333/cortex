    return g_previousRegistryFilter ? g_previousRegistryFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void RegistryWorker() {
    while (g_registryRunning.load(std::memory_order_acquire)) {
        RefreshLoadedMods();
        for (int i = 0; i < 20 && g_registryRunning.load(std::memory_order_acquire); ++i)
            Sleep(50);
    }
}

} // namespace

bool RegistryInit(const char* crashOutputDirectory) {
    bool expected = false;
    if (!g_registryRunning.compare_exchange_strong(expected, true)) return true;
    g_cortexModule = ModuleFromAddress(reinterpret_cast<uintptr_t>(&RegistryInit));
    RegistryCopyText(g_outputDirectory, sizeof(g_outputDirectory), crashOutputDirectory ? crashOutputDirectory : "");
    GetWindowsDirectoryA(g_windowsDirectory, static_cast<UINT>(sizeof(g_windowsDirectory)));
    char executablePath[kCrashPathSize]{};
    GetModuleFileNameA(nullptr, executablePath, static_cast<DWORD>(sizeof(executablePath)));
    DirectoryOf(executablePath, g_executableDirectory, sizeof(g_executableDirectory));
    RefreshLoadedMods();
    g_previousRegistryFilter = SetUnhandledExceptionFilter(RegistryUnhandledFilter);
    try {
        g_registryThread = std::thread(RegistryWorker);
    } catch (...) {
        SetUnhandledExceptionFilter(g_previousRegistryFilter);
        g_previousRegistryFilter = nullptr;
        g_registryRunning.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void RegistryShutdown() {
    if (!g_registryRunning.exchange(false, std::memory_order_acq_rel)) return;
    if (g_registryThread.joinable()) g_registryThread.join();
    SetUnhandledExceptionFilter(g_previousRegistryFilter);
    g_previousRegistryFilter = nullptr;
}

void RefreshLoadedMods() {
    const uint64_t generation = g_scanGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Module32First(snapshot, &entry)) {
        do {
            if (IsLikelyMod(entry.hModule, entry.szExePath)) {
                RegisterModuleInternal(entry.hModule, nullptr, false, generation,
                                       entry.szExePath, entry.modBaseSize);
            }
        } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);

    RegistrySpinGuard guard(g_modLock);
    for (auto& mod : g_mods) {
        if (mod.used && !mod.manuallyRegistered && mod.scanGeneration != generation) mod = {};
    }
}

size_t SnapshotMods(ModSnapshot* output, size_t capacity) {
    if (!output || capacity == 0) return 0;
    TrySpinGuard guard(g_modLock);
    if (!guard) return 0;
    size_t count = 0;
    for (const auto& mod : g_mods) {
        if (!mod.used || count == capacity) continue;
        output[count++] = static_cast<const ModSnapshot&>(mod);
    }
    return count;
}

size_t SnapshotScopes(ScopeSnapshot* output, size_t capacity) {
    if (!output || capacity == 0) return 0;
    TrySpinGuard guard(g_scopeLock);
    if (!guard) return 0;
    size_t count = 0;
    for (const auto& scope : g_scopes) {
        if (!scope.used || count == capacity) continue;
        output[count++] = static_cast<const ScopeSnapshot&>(scope);
    }
    std::sort(output, output + count, [](const ScopeSnapshot& a, const ScopeSnapshot& b) {
        if (a.threadId != b.threadId) return a.threadId < b.threadId;
        return a.depth < b.depth;
    });
    return count;
}

size_t SnapshotValues(ValueSnapshot* output, size_t capacity, uint64_t* dropped) {
    if (dropped) *dropped = 0;
    if (!output || capacity == 0) return 0;
    TrySpinGuard guard(g_valueLock);
    if (!guard) return 0;
    const uint64_t next = g_nextValueSequence.load(std::memory_order_acquire);
    const uint64_t retained = (std::min)(next, static_cast<uint64_t>(kValueCapacity));
    const size_t count = static_cast<size_t>((std::min)(retained, static_cast<uint64_t>(capacity)));
    const uint64_t first = next - count;
    if (dropped) *dropped = first;
    for (size_t i = 0; i < count; ++i) output[i] = g_values[(first + i) % kValueCapacity];
    return count;
}

bool FindModForAddress(uintptr_t address, ModSnapshot& output) {
    TrySpinGuard guard(g_modLock);
    if (!guard) return false;
    for (const auto& mod : g_mods) {
        if (!mod.used || !mod.base || !mod.imageSize) continue;
        if (address >= mod.base && address < mod.base + mod.imageSize) {
            output = static_cast<const ModSnapshot&>(mod);
            return true;
        }
    }
    return false;
}

bool WriteRegistrySnapshots(const char* directory) {
    const bool mods = WriteMods(directory);
    const bool scopes = WriteScopes(directory);
    const bool values = WriteValues(directory);
    return mods && scopes && values;
}

#ifdef CORTEX_DIAGNOSTICS_TESTING
namespace testing {
void ResetRegistry() {
    RegistryShutdown();
    {
        RegistrySpinGuard guard(g_modLock);
        for (auto& mod : g_mods) mod = {};
        g_nextModId.store(1, std::memory_order_release);
        g_scanGeneration.store(0, std::memory_order_release);
    }
    {
        RegistrySpinGuard guard(g_scopeLock);
        for (auto& scope : g_scopes) scope = {};
        g_nextScopeId.store(1, std::memory_order_release);
    }
    {
        RegistrySpinGuard guard(g_valueLock);
        for (auto& value : g_values) value = {};
        g_nextValueSequence.store(0, std::memory_order_release);
    }
    g_cortexModule = ModuleFromAddress(reinterpret_cast<uintptr_t>(&ResetRegistry));
    GetWindowsDirectoryA(g_windowsDirectory, static_cast<UINT>(sizeof(g_windowsDirectory)));
    char executablePath[kCrashPathSize]{};
    GetModuleFileNameA(nullptr, executablePath, static_cast<DWORD>(sizeof(executablePath)));
    DirectoryOf(executablePath, g_executableDirectory, sizeof(g_executableDirectory));
}
} // namespace testing
#endif

} // namespace diagnostics

extern "C" __declspec(dllexport) BOOL CortexDiagRegisterMod(const CortexDiagModInfo* info) {
    if (!info || info->abi_version != CORTEX_DIAG_ABI_VERSION ||
        info->struct_size < sizeof(CortexDiagModInfo)) return FALSE;
    HMODULE module = info->module;
    if (!module) module = diagnostics::ModuleFromAddress(CORTEX_DIAG_CALLER_ADDRESS());
    return diagnostics::RegisterModuleInternal(module, info, true,
             diagnostics::g_scanGeneration.load(std::memory_order_relaxed)) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) void CortexDiagUnregisterMod(HMODULE module) {
    if (!module) module = diagnostics::ModuleFromAddress(CORTEX_DIAG_CALLER_ADDRESS());
    diagnostics::RegistrySpinGuard guard(diagnostics::g_modLock);
    if (auto* record = diagnostics::FindModByModuleLocked(module)) *record = {};
}

extern "C" __declspec(dllexport) uint64_t CortexDiagScopeEnter(const char* name, const char* file, int line) {
    const uintptr_t caller = CORTEX_DIAG_CALLER_ADDRESS();
    const uint64_t modId = diagnostics::ModIdForModule(diagnostics::ModuleFromAddress(caller));
    const DWORD threadId = GetCurrentThreadId();
    diagnostics::RegistrySpinGuard guard(diagnostics::g_scopeLock);
    diagnostics::ScopeRecord* freeRecord = nullptr;
    for (auto& scope : diagnostics::g_scopes) {
        if (!scope.used) { freeRecord = &scope; break; }
    }
    if (!freeRecord) return 0;
    uint32_t parentDepth = 0;
    const uint64_t parent = diagnostics::ActiveScopeForThreadLocked(threadId, &parentDepth);
    *freeRecord = {};
    freeRecord->used = true;
    freeRecord->id = diagnostics::g_nextScopeId.fetch_add(1, std::memory_order_relaxed);
    freeRecord->parentId = parent;
    freeRecord->modId = modId;
    freeRecord->threadId = threadId;
    freeRecord->depth = parent ? parentDepth + 1 : 0;
    freeRecord->enteredAtMs = GetTickCount64();
    diagnostics::RegistryCopyText(freeRecord->name, sizeof(freeRecord->name), name ? name : "scope");
    diagnostics::RegistryCopyText(freeRecord->file, sizeof(freeRecord->file), file ? file : "");
    freeRecord->line = line;
    diagnostics::TouchMod(modId);
    return freeRecord->id;
}

extern "C" __declspec(dllexport) void CortexDiagScopeExit(uint64_t scopeId) {
    if (!scopeId) return;
    diagnostics::RegistrySpinGuard guard(diagnostics::g_scopeLock);
    for (auto& scope : diagnostics::g_scopes) {
        if (scope.used && scope.id == scopeId) {
            scope = {};
            return;
        }
    }
}

extern "C" __declspec(dllexport) void CortexDiagValuePointer(const char* name, const void* value) {
    diagnostics::RecordValue(diagnostics::DiagnosticValueType::Pointer, name,
        CORTEX_DIAG_CALLER_ADDRESS(), reinterpret_cast<uintptr_t>(value), 0, 0, 0.0, false, nullptr);
}
extern "C" __declspec(dllexport) void CortexDiagValueInt64(const char* name, int64_t value) {
    diagnostics::RecordValue(diagnostics::DiagnosticValueType::Int64, name,
        CORTEX_DIAG_CALLER_ADDRESS(), 0, value, 0, 0.0, false, nullptr);
}
extern "C" __declspec(dllexport) void CortexDiagValueUInt64(const char* name, uint64_t value) {
    diagnostics::RecordValue(diagnostics::DiagnosticValueType::UInt64, name,
        CORTEX_DIAG_CALLER_ADDRESS(), 0, 0, value, 0.0, false, nullptr);
}
extern "C" __declspec(dllexport) void CortexDiagValueDouble(const char* name, double value) {
    diagnostics::RecordValue(diagnostics::DiagnosticValueType::Double, name,
        CORTEX_DIAG_CALLER_ADDRESS(), 0, 0, 0, value, false, nullptr);
}
extern "C" __declspec(dllexport) void CortexDiagValueBool(const char* name, BOOL value) {
    diagnostics::RecordValue(diagnostics::DiagnosticValueType::Bool, name,
        CORTEX_DIAG_CALLER_ADDRESS(), 0, 0, 0, 0.0, value != FALSE, nullptr);
}
extern "C" __declspec(dllexport) void CortexDiagValueText(const char* name, const char* value) {
    diagnostics::RecordValue(diagnostics::DiagnosticValueType::Text, name,
        CORTEX_DIAG_CALLER_ADDRESS(), 0, 0, 0, 0.0, false, value);
}
