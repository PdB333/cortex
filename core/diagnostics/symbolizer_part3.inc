                         Hex(crash->displacement).c_str());
        } else {
            std::fprintf(file, "Location: %s+%s\r\n",
                         crash->moduleName.c_str(), Hex(crash->moduleRva).c_str());
        }
        bool mapped = false;
        const std::string source = RemapSource(crash->file, crashMod, mapped);
        if (crash->hasLine)
            std::fprintf(file, "Source: %s:%u%s\r\n", source.c_str(), crash->line,
                         mapped ? " (mapped to local source root)" : "");
        std::fprintf(file, "Symbols: %s (%s)\r\n",
                     crash->exactSymbols ? "exact match" : "not verified",
                     crash->verification.empty() ? "unknown" : crash->verification.c_str());
        if (!crash->loadedPdb.empty()) std::fprintf(file, "PDB: %s\r\n", crash->loadedPdb.c_str());
    }
    if (loadInfo && !loadInfo->exactMatch && loadInfo->identity.hasCodeView)
        std::fputs("WARNING: the loaded PDB GUID/age does not match this module build.\r\n", file);

    std::fprintf(file, "\r\nStack (%llu frame%s, DbgHelp lock %s):\r\n",
                 static_cast<unsigned long long>(frames.size()), frames.size() == 1 ? "" : "s",
                 capture.lockAcquired ? "acquired" : "busy");
    const size_t displayedFrames = (std::min)(frames.size(), static_cast<size_t>(32));
    for (size_t i = 0; i < displayedFrames; ++i) {
        const symbols::StackFrame& frame = frames[i];
        if (frame.hasSymbol)
            std::fprintf(file, "#%-2u %s!%s+%s", frame.index, frame.moduleName.c_str(),
                         frame.symbolName.c_str(), Hex(frame.displacement).c_str());
        else
            std::fprintf(file, "#%-2u %s+%s", frame.index, frame.moduleName.c_str(),
                         Hex(frame.moduleRva).c_str());
        const ModSnapshot* frameMod = ModForAddress(frame.address, modules);
        bool mapped = false;
        const std::string source = RemapSource(frame.file, frameMod, mapped);
        if (frame.hasLine) std::fprintf(file, "  [%s:%u]", source.c_str(), frame.line);
        std::fputs("\r\n", file);
    }

    const DWORD crashThread = pointers ? GetCurrentThreadId() : 0;
    std::vector<ScopeSnapshot> scopes(kMaxActiveScopes);
    scopes.resize(SnapshotScopes(scopes.data(), scopes.size()));
    std::fputs("\r\nActive scopes:\r\n", file);
    bool wroteScope = false;
    for (const ScopeSnapshot& scope : scopes) {
        if (crashThread && scope.threadId != crashThread) continue;
        wroteScope = true;
        for (uint32_t depth = 0; depth < scope.depth; ++depth) std::fputs("  ", file);
        std::fprintf(file, "%s", scope.name);
        if (scope.file[0]) std::fprintf(file, "  [%s:%d]", scope.file, scope.line);
        std::fputs("\r\n", file);
    }
    if (!wroteScope) std::fputs("(none recorded)\r\n", file);

    std::vector<ValueSnapshot> values(kValueCapacity);
    uint64_t dropped = 0;
    values.resize(SnapshotValues(values.data(), values.size(), &dropped));
    std::fputs("\r\nRecent values:\r\n", file);
    size_t writtenValues = 0;
    for (auto iterator = values.rbegin(); iterator != values.rend() && writtenValues < 16; ++iterator) {
        if (crashThread && iterator->threadId != crashThread) continue;
        std::fprintf(file, "%s = %s\r\n", iterator->name, ValueText(*iterator).c_str());
        ++writtenValues;
    }
    if (!writtenValues) std::fputs("(none recorded)\r\n", file);
    if (dropped) std::fprintf(file, "(%llu older value entries overwritten)\r\n",
                              static_cast<unsigned long long>(dropped));

    if (!g_symbolizerOptions.externalToolPath.empty()) {
        std::fprintf(file,
                     "\r\nOffline fallback: use cortex_symbolize with %s for unresolved MinGW/DWARF frames.\r\n",
                     g_symbolizerOptions.externalToolPath.c_str());
    }
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

LONG WINAPI SymbolizerUnhandledFilter(PEXCEPTION_POINTERS pointers) {
    const LONG previousResult = g_previousSymbolizerFilter
        ? g_previousSymbolizerFilter(pointers)
        : EXCEPTION_CONTINUE_SEARCH;
    if (!g_symbolizerEnabled.load(std::memory_order_acquire) || !pointers)
        return previousResult;
    if (g_symbolCaptureActive.test_and_set(std::memory_order_acq_rel)) return previousResult;
    char directory[2048]{};
    if (FindNewestCrashDirectory(directory, sizeof(directory)))
        WriteSymbolizedCrash(directory, pointers);
    g_symbolCaptureActive.clear(std::memory_order_release);
    return previousResult;
}

} // namespace

bool SymbolizerInit(const SymbolizerOptions& options) {
    if (g_symbolizerEnabled.load(std::memory_order_acquire)) return true;
    g_symbolizerOptions = options;
    if (!g_symbolizerOptions.enabled) return true;
    if (g_symbolizerOptions.maxFrames == 0) g_symbolizerOptions.maxFrames = 64;
    if (g_symbolizerOptions.maxFrames > kMaximumSymbolFrames)
        g_symbolizerOptions.maxFrames = kMaximumSymbolFrames;
    if (!g_symbolizerOptions.symbolSearchPath.empty())
        symbols::AddSearchPath(g_symbolizerOptions.symbolSearchPath);
    g_symbolCaptureActive.clear(std::memory_order_release);
    g_previousSymbolizerFilter = SetUnhandledExceptionFilter(SymbolizerUnhandledFilter);
    g_symbolizerEnabled.store(true, std::memory_order_release);
    return true;
}

void SymbolizerShutdown() {
    if (!g_symbolizerEnabled.exchange(false, std::memory_order_acq_rel)) return;
    SetUnhandledExceptionFilter(g_previousSymbolizerFilter);
    g_previousSymbolizerFilter = nullptr;
}

bool IsSymbolizerEnabled() {
    return g_symbolizerEnabled.load(std::memory_order_acquire);
}

bool WriteSymbolizedCrash(const char* directory, PEXCEPTION_POINTERS exceptionPointers) {
    if (!directory || !*directory || !exceptionPointers || !exceptionPointers->ContextRecord)
        return false;
    std::vector<PreparedModule> modules = PrepareModules();
    std::vector<symbols::StackFrame> frameBuffer(g_symbolizerOptions.maxFrames
                                                  ? g_symbolizerOptions.maxFrames : 64);
    symbols::StackCapture capture{};
    const size_t count = symbols::CaptureStack(*exceptionPointers->ContextRecord,
                                               GetCurrentThread(), frameBuffer.data(),
                                               frameBuffer.size(), &capture);
    frameBuffer.resize(count);
    const bool stack = WriteStackJson(directory, frameBuffer, capture, modules);
    const bool builds = WriteBuildInfo(directory, modules);
    const bool report = WriteTextReport(directory, exceptionPointers, frameBuffer, capture, modules);
    return stack && builds && report;
}

#ifdef CORTEX_DIAGNOSTICS_TESTING
namespace testing {
void ResetSymbolizer() {
    SymbolizerShutdown();
    g_symbolizerOptions = {};
    g_symbolCaptureActive.clear(std::memory_order_release);
}
} // namespace testing
#endif

} // namespace diagnostics
