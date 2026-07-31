    if (out.hasCodeView) {
        std::snprintf(build, sizeof(build), "pe-%08lX-%08lX-pdb-%s-%lu",
                      static_cast<unsigned long>(out.timeDateStamp),
                      static_cast<unsigned long>(out.imageSize),
                      out.pdbGuid.c_str(), static_cast<unsigned long>(out.pdbAge));
    } else {
        std::snprintf(build, sizeof(build), "pe-%08lX-%08lX",
                      static_cast<unsigned long>(out.timeDateStamp),
                      static_cast<unsigned long>(out.imageSize));
    }
    out.buildId = build;
    return out;
}

bool ModuleForAddress(uintptr_t address, uintptr_t& base, size_t& imageSize,
                      std::string& path) {
    HMODULE module = nullptr;
    if (!address || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         reinterpret_cast<LPCSTR>(address), &module)) {
        return false;
    }
    char buffer[32768]{};
    const DWORD length = GetModuleFileNameA(module, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (!length) return false;
    path.assign(buffer, length);
    base = reinterpret_cast<uintptr_t>(module);
    imageSize = 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) imageSize = nt->OptionalHeader.SizeOfImage;
    }
    return true;
}

ModuleLoadInfo ReadModuleInfoLocked(uintptr_t base, const std::string& imagePath,
                                    size_t imageSize) {
    ModuleLoadInfo result;
    result.base = base;
    result.imageSize = imageSize;
    result.imagePath = imagePath;
    result.identity = InspectModule(imagePath);

    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    if (!SymGetModuleInfo64(g_process, static_cast<DWORD64>(base), &module)) {
        result.error = GetLastError();
        result.verification = "module_not_loaded";
        return result;
    }

    result.loaded = true;
    result.hasSymbols = module.SymType != SymNone && module.SymType != SymDeferred;
    result.loadedImage = module.LoadedImageName;
    result.loadedPdb = module.LoadedPdbName;
    result.symbolType = SymbolTypeName(module.SymType);

    if (result.identity.hasCodeView) {
        const std::string loadedGuid = GuidToString(module.PdbSig70);
        result.exactMatch = module.PdbAge == result.identity.pdbAge &&
                            _stricmp(loadedGuid.c_str(), result.identity.pdbGuid.c_str()) == 0;
        result.verification = result.exactMatch ? "pdb_guid_age_match" : "pdb_guid_age_mismatch";
    } else if (module.SymType == SymExport || module.SymType == SymCoff) {
        result.exactMatch = true;
        result.verification = "image_symbols_no_pdb_identity";
    } else if (result.hasSymbols) {
        result.verification = "symbols_loaded_identity_unavailable";
    } else {
        result.verification = "no_symbols";
    }
    return result;
}

ModuleLoadInfo LoadModuleLocked(uintptr_t base, size_t imageSize,
                                const std::string& imagePath,
                                const std::string& preferredSymbolPath) {
    if (!preferredSymbolPath.empty()) {
        const DWORD attrs = GetFileAttributesA(preferredSymbolPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            AppendSearchPath(g_searchPath, preferredSymbolPath);
        else
            AppendSearchPath(g_searchPath, DirectoryOf(preferredSymbolPath));
        ApplySearchPathLocked();
    }
    AppendSearchPath(g_searchPath, DirectoryOf(imagePath));
    ApplySearchPathLocked();

    SetLastError(ERROR_SUCCESS);
    const DWORD64 loadedBase = SymLoadModuleEx(g_process, nullptr,
                                                imagePath.empty() ? nullptr : imagePath.c_str(),
                                                nullptr,
                                                static_cast<DWORD64>(base),
                                                static_cast<DWORD>(imageSize),
                                                nullptr, 0);
    DWORD loadError = loadedBase ? ERROR_SUCCESS : GetLastError();
    if (!loadedBase) SymRefreshModuleList(g_process);

    // Trigger deferred loading so PDB GUID/age and symbol type are available
    // immediately to crash reports rather than only after the first lookup.
    std::array<uint8_t, sizeof(SYMBOL_INFO) + MAX_SYM_NAME + 1> probeBuffer{};
    auto* probe = reinterpret_cast<PSYMBOL_INFO>(probeBuffer.data());
    probe->SizeOfStruct = sizeof(SYMBOL_INFO);
    probe->MaxNameLen = MAX_SYM_NAME;
    DWORD64 probeDisplacement = 0;
    SymFromAddr(g_process, static_cast<DWORD64>(base), &probeDisplacement, probe);

    ModuleLoadInfo result = ReadModuleInfoLocked(base, imagePath, imageSize);
    if (!result.loaded && loadError != ERROR_SUCCESS) result.error = loadError;
    g_modules[base] = result;
    return result;
}

ModuleLoadInfo EnsureModuleLocked(uintptr_t address) {
    uintptr_t base = 0;
    size_t imageSize = 0;
    std::string path;
    if (!ModuleForAddress(address, base, imageSize, path)) {
        ModuleLoadInfo missing;
        missing.error = ERROR_MOD_NOT_FOUND;
        missing.verification = "address_not_in_module";
        return missing;
    }
    const auto existing = g_modules.find(base);
    if (existing != g_modules.end()) return existing->second;
    return LoadModuleLocked(base, imageSize, path, std::string());
}

ResolvedLocation ResolveDetailedLocked(uintptr_t address) {
    ResolvedLocation out;
    out.address = address;
    ModuleLoadInfo module = EnsureModuleLocked(address);
    out.moduleBase = module.base;
    out.moduleRva = module.base && address >= module.base ? address - module.base : 0;
    out.modulePath = module.imagePath;
    out.moduleName = BaseName(module.imagePath);
    out.loadedPdb = module.loadedPdb;
    out.symbolType = module.symbolType;
    out.verification = module.verification;
    out.exactSymbols = module.exactMatch;
    out.buildId = module.identity.buildId;

    std::array<uint8_t, sizeof(SYMBOL_INFO) + MAX_SYM_NAME + 1> buffer{};
    auto* symbol = reinterpret_cast<PSYMBOL_INFO>(buffer.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (SymFromAddr(g_process, static_cast<DWORD64>(address), &displacement, symbol)) {
        out.hasSymbol = true;
        out.symbolName.assign(symbol->Name, symbol->NameLen);
        out.symbolAddress = static_cast<uintptr_t>(symbol->Address);
        out.displacement = displacement;
    }

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddr64(g_process, static_cast<DWORD64>(address),
                             &lineDisplacement, &line)) {
        out.hasLine = true;
        out.file = line.FileName ? line.FileName : "";
        out.line = line.LineNumber;
    }
    return out;
}

uintptr_t InstructionPointer(const CONTEXT& context) {
#if defined(_M_X64) || defined(__x86_64__)
    return static_cast<uintptr_t>(context.Rip);
#elif defined(_M_IX86) || defined(__i386__)
    return static_cast<uintptr_t>(context.Eip);
#else
    (void)context;
    return 0;
#endif
}

} // namespace

void Init(const std::string& searchPath, bool invadeProcess) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) {
        AppendSearchPath(g_searchPath, searchPath);
        ApplySearchPathLocked();
        return;
    }
    g_searchPath.clear();
    AppendSearchPath(g_searchPath, searchPath);
    char environment[32768]{};
    const DWORD length = GetEnvironmentVariableA("_NT_SYMBOL_PATH", environment,
                                                  static_cast<DWORD>(sizeof(environment)));
    if (length && length < sizeof(environment)) AppendSearchPath(g_searchPath, environment);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS |
                  SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_EXACT_SYMBOLS);
    g_initialized = SymInitialize(g_process,
                                   g_searchPath.empty() ? nullptr : g_searchPath.c_str(),
                                   invadeProcess ? TRUE : FALSE) == TRUE;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;
    SymCleanup(g_process);
    g_modules.clear();
    g_searchPath.clear();
    g_initialized = false;
}

bool IsInitialized() {
