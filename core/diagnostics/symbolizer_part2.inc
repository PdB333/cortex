const symbols::ModuleLoadInfo* LoadInfoForAddress(uintptr_t address,
                                                   const std::vector<PreparedModule>& modules) {
    for (const auto& prepared : modules) {
        const ModSnapshot& mod = prepared.mod;
        if (mod.base && mod.imageSize && address >= mod.base && address < mod.base + mod.imageSize)
            return &prepared.loaded;
    }
    return nullptr;
}

std::vector<PreparedModule> PrepareModules() {
    std::vector<ModSnapshot> snapshots(kMaxRegisteredMods);
    const size_t count = SnapshotMods(snapshots.data(), snapshots.size());
    snapshots.resize(count);
    std::vector<PreparedModule> modules;
    modules.reserve(count);
    for (const ModSnapshot& mod : snapshots) {
        if (mod.symbolPath[0]) {
            const DWORD attributes = GetFileAttributesA(mod.symbolPath);
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
                symbols::AddSearchPath(mod.symbolPath);
            else
                symbols::AddSearchPath(DirectoryOf(mod.symbolPath));
        }
        PreparedModule prepared;
        prepared.mod = mod;
        prepared.loaded = symbols::LoadModule(mod.base, mod.imageSize, mod.path, mod.symbolPath);
        modules.push_back(std::move(prepared));
    }
    return modules;
}

bool WriteStackJson(const char* directory,
                    const std::vector<symbols::StackFrame>& frames,
                    const symbols::StackCapture& capture,
                    const std::vector<PreparedModule>& modules) {
    char path[2048]{};
    std::snprintf(path, sizeof(path), "%s\\stack.json", directory);
    FILE* file = nullptr;
    fopen_s(&file, path, "wb");
    if (!file) return false;
    std::fprintf(file,
                 "{\n  \"schema_version\":1,\n  \"backend\":\"dbghelp\",\n"
                 "  \"capture\":{\"initialized\":%s,\"lock_acquired\":%s,"
                 "\"stack_walk_succeeded\":%s,\"error\":%lu,\"frame_count\":%llu},\n"
                 "  \"frames\":[\n",
                 capture.initialized ? "true" : "false",
                 capture.lockAcquired ? "true" : "false",
                 capture.stackWalkSucceeded ? "true" : "false",
                 static_cast<unsigned long>(capture.error),
                 static_cast<unsigned long long>(frames.size()));
    for (size_t i = 0; i < frames.size(); ++i) {
        const symbols::StackFrame& frame = frames[i];
        const ModSnapshot* mod = ModForAddress(frame.address, modules);
        bool sourceMapped = false;
        const std::string mappedFile = RemapSource(frame.file, mod, sourceMapped);
        const std::string named = frame.hasSymbol
            ? frame.moduleName + "!" + frame.symbolName + "+" + Hex(frame.displacement)
            : frame.moduleName + "+" + Hex(frame.moduleRva);
        std::fprintf(file,
                     "    {\"index\":%u,\"address\":\"%s\",\"module\":\"%s\","
                     "\"module_path\":\"%s\",\"module_base\":\"%s\",\"rva\":\"%s\","
                     "\"named_address\":\"%s\",\"build_id\":\"%s\","
                     "\"has_symbol\":%s,\"symbol\":\"%s\",\"symbol_address\":\"%s\","
                     "\"displacement\":%llu,\"has_line\":%s,\"file\":\"%s\","
                     "\"original_file\":\"%s\",\"source_mapped\":%s,\"line\":%u,"
                     "\"loaded_pdb\":\"%s\",\"symbol_type\":\"%s\","
                     "\"exact_symbols\":%s,\"verification\":\"%s\","
                     "\"registered_mod_id\":%llu,\"registered_mod\":\"%s\"}%s\n",
                     frame.index,
                     Hex(frame.address).c_str(), JsonEscape(frame.moduleName).c_str(),
                     JsonEscape(frame.modulePath).c_str(), Hex(frame.moduleBase).c_str(),
                     Hex(frame.moduleRva).c_str(), JsonEscape(named).c_str(),
                     JsonEscape(frame.buildId).c_str(), frame.hasSymbol ? "true" : "false",
                     JsonEscape(frame.symbolName).c_str(), Hex(frame.symbolAddress).c_str(),
                     static_cast<unsigned long long>(frame.displacement),
                     frame.hasLine ? "true" : "false", JsonEscape(mappedFile).c_str(),
                     JsonEscape(frame.file).c_str(), sourceMapped ? "true" : "false", frame.line,
                     JsonEscape(frame.loadedPdb).c_str(), JsonEscape(frame.symbolType).c_str(),
                     frame.exactSymbols ? "true" : "false", JsonEscape(frame.verification).c_str(),
                     static_cast<unsigned long long>(mod ? mod->id : 0),
                     JsonEscape(mod ? mod->name : "").c_str(),
                     i + 1 == frames.size() ? "" : ",");
    }
    std::fputs("  ]\n}\n", file);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool WriteBuildInfo(const char* directory, const std::vector<PreparedModule>& modules) {
    char path[2048]{};
    std::snprintf(path, sizeof(path), "%s\\build_info.json", directory);
    FILE* file = nullptr;
    fopen_s(&file, path, "wb");
    if (!file) return false;
    std::fputs("{\n  \"schema_version\":1,\n  \"modules\":[\n", file);
    for (size_t i = 0; i < modules.size(); ++i) {
        const PreparedModule& prepared = modules[i];
        const ModSnapshot& mod = prepared.mod;
        const symbols::ModuleLoadInfo& loaded = prepared.loaded;
        const symbols::ModuleIdentity& identity = loaded.identity;
        std::fprintf(file,
                     "    {\"mod_id\":%llu,\"name\":\"%s\",\"path\":\"%s\","
                     "\"base\":\"%s\",\"image_size\":%llu,\"registered_build_id\":\"%s\","
                     "\"actual_build_id\":\"%s\",\"pe_timestamp\":%lu,"
                     "\"preferred_image_base\":\"%s\",\"has_codeview\":%s,"
                     "\"pdb_guid\":\"%s\",\"pdb_age\":%lu,\"pdb_path_hint\":\"%s\","
                     "\"configured_symbol_path\":\"%s\",\"loaded_pdb\":\"%s\","
                     "\"symbol_type\":\"%s\",\"symbols_loaded\":%s,"
                     "\"exact_match\":%s,\"verification\":\"%s\",\"error\":%lu}%s\n",
                     static_cast<unsigned long long>(mod.id), JsonEscape(mod.name).c_str(),
                     JsonEscape(mod.path).c_str(), Hex(mod.base).c_str(),
                     static_cast<unsigned long long>(mod.imageSize),
                     JsonEscape(mod.buildId).c_str(), JsonEscape(identity.buildId).c_str(),
                     static_cast<unsigned long>(identity.timeDateStamp),
                     Hex(identity.preferredImageBase).c_str(),
                     identity.hasCodeView ? "true" : "false", JsonEscape(identity.pdbGuid).c_str(),
                     static_cast<unsigned long>(identity.pdbAge),
                     JsonEscape(identity.pdbPathHint).c_str(), JsonEscape(mod.symbolPath).c_str(),
                     JsonEscape(loaded.loadedPdb).c_str(), JsonEscape(loaded.symbolType).c_str(),
                     loaded.hasSymbols ? "true" : "false", loaded.exactMatch ? "true" : "false",
                     JsonEscape(loaded.verification).c_str(), static_cast<unsigned long>(loaded.error),
                     i + 1 == modules.size() ? "" : ",");
    }
    std::fputs("  ]\n}\n", file);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

std::string ValueText(const ValueSnapshot& value) {
    char buffer[256]{};
    switch (value.type) {
        case DiagnosticValueType::Pointer:
            return Hex(value.pointerValue);
        case DiagnosticValueType::Int64:
            std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value.int64Value));
            return buffer;
        case DiagnosticValueType::UInt64:
            std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value.uint64Value));
            return buffer;
        case DiagnosticValueType::Double:
            std::snprintf(buffer, sizeof(buffer), "%.17g", value.doubleValue);
            return buffer;
        case DiagnosticValueType::Bool:
            return value.boolValue ? "true" : "false";
        case DiagnosticValueType::Text:
            return value.textValue;
        default:
            return "?";
    }
}

bool WriteTextReport(const char* directory, PEXCEPTION_POINTERS pointers,
                     const std::vector<symbols::StackFrame>& frames,
                     const symbols::StackCapture& capture,
                     const std::vector<PreparedModule>& modules) {
    char path[2048]{};
    std::snprintf(path, sizeof(path), "%s\\report.txt", directory);
    FILE* file = nullptr;
    fopen_s(&file, path, "wb");
    if (!file) return false;

    PEXCEPTION_RECORD record = pointers ? pointers->ExceptionRecord : nullptr;
    const DWORD code = record ? record->ExceptionCode : 0;
    const uintptr_t instruction = record
        ? reinterpret_cast<uintptr_t>(record->ExceptionAddress)
        : (frames.empty() ? 0 : frames.front().address);
    uintptr_t accessed = 0;
    const char* access = AccessName(record, accessed);
    const symbols::StackFrame* crash = frames.empty() ? nullptr : &frames.front();
    const ModSnapshot* crashMod = ModForAddress(instruction, modules);
    const symbols::ModuleLoadInfo* loadInfo = LoadInfoForAddress(instruction, modules);

    std::fputs("Cortex crash report (symbolized)\r\n\r\n", file);
    std::fprintf(file, "Exception: %s (0x%08lX)\r\n",
                 ExceptionName(code), static_cast<unsigned long>(code));
    std::fprintf(file, "Instruction: %s\r\n", Hex(instruction).c_str());
    if (accessed) std::fprintf(file, "Invalid memory %s: %s\r\n", access, Hex(accessed).c_str());
    if (crashMod) {
        std::fprintf(file, "Suspected mod: %s", crashMod->name);
        if (crashMod->version[0]) std::fprintf(file, " %s", crashMod->version);
        std::fputs("\r\n", file);
        if (crashMod->gitCommit[0]) std::fprintf(file, "Commit: %s\r\n", crashMod->gitCommit);
    }
    if (crash) {
        if (crash->hasSymbol) {
            std::fprintf(file, "Location: %s!%s+%s\r\n",
                         crash->moduleName.c_str(), crash->symbolName.c_str(),
