        if (!scope.used || scope.threadId != threadId) continue;
        if (!best || scope.depth >= bestDepth) {
            best = scope.id;
            bestDepth = scope.depth;
        }
    }
    if (depth) *depth = best ? bestDepth : 0;
    return best;
}

uint64_t ScopeModIdLocked(uint64_t scopeId) {
    if (!scopeId) return 0;
    for (const auto& scope : g_scopes) {
        if (scope.used && scope.id == scopeId) return scope.modId;
    }
    return 0;
}

void RecordValue(DiagnosticValueType type, const char* name, uintptr_t caller,
                 uintptr_t pointerValue, int64_t int64Value, uint64_t uint64Value,
                 double doubleValue, bool boolValue, const char* textValue) {
    const DWORD threadId = GetCurrentThreadId();
    uint64_t scopeId = 0;
    uint64_t modId = 0;
    {
        TrySpinGuard guard(g_scopeLock);
        if (guard) {
            scopeId = ActiveScopeForThreadLocked(threadId);
            modId = ScopeModIdLocked(scopeId);
        }
    }
    if (!modId) modId = ModIdForModule(ModuleFromAddress(caller));
    TouchMod(modId);

    RegistrySpinGuard guard(g_valueLock);
    const uint64_t index = g_nextValueSequence.fetch_add(1, std::memory_order_relaxed);
    ValueSnapshot& value = g_values[index % kValueCapacity];
    value = {};
    value.sequence = index + 1;
    value.timestampMs = GetTickCount64();
    value.scopeId = scopeId;
    value.modId = modId;
    value.threadId = threadId;
    value.type = type;
    RegistryCopyText(value.name, sizeof(value.name), name ? name : "value");
    value.pointerValue = pointerValue;
    value.int64Value = int64Value;
    value.uint64Value = uint64Value;
    value.doubleValue = doubleValue;
    value.boolValue = boolValue;
    RegistryCopyText(value.textValue, sizeof(value.textValue), textValue ? textValue : "");
}

bool EscapeJson(const char* input, char* output, size_t outputSize) {
    if (!output || outputSize == 0) return false;
    output[0] = '\0';
    if (!input) return true;
    size_t used = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p; ++p) {
        char encoded[7]{};
        size_t count = 0;
        switch (*p) {
            case '"': encoded[0] = '\\'; encoded[1] = '"'; count = 2; break;
            case '\\': encoded[0] = '\\'; encoded[1] = '\\'; count = 2; break;
            case '\n': encoded[0] = '\\'; encoded[1] = 'n'; count = 2; break;
            case '\r': encoded[0] = '\\'; encoded[1] = 'r'; count = 2; break;
            case '\t': encoded[0] = '\\'; encoded[1] = 't'; count = 2; break;
            default:
                if (*p < 0x20) {
                    std::snprintf(encoded, sizeof(encoded), "\\u%04X", static_cast<unsigned>(*p));
                    count = 6;
                } else {
                    encoded[0] = static_cast<char>(*p);
                    count = 1;
                }
                break;
        }
        if (used + count + 1 > outputSize) return false;
        std::memcpy(output + used, encoded, count);
        used += count;
    }
    output[used] = '\0';
    return true;
}

const char* ValueTypeName(DiagnosticValueType type) {
    switch (type) {
        case DiagnosticValueType::Pointer: return "pointer";
        case DiagnosticValueType::Int64: return "int64";
        case DiagnosticValueType::UInt64: return "uint64";
        case DiagnosticValueType::Double: return "double";
        case DiagnosticValueType::Bool: return "bool";
        case DiagnosticValueType::Text: return "text";
        default: return "unknown";
    }
}

bool WriteMods(const char* directory) {
    char path[kCrashPathSize]{};
    std::snprintf(path, sizeof(path), "%s\\mods.json", directory);
    FILE* file = nullptr;
    fopen_s(&file, path, "wb");
    if (!file) return false;

    TrySpinGuard guard(g_modLock);
    if (!guard) {
        std::fputs("{\"capture_error\":\"registry_busy\",\"mods\":[]}", file);
        std::fclose(file);
        return false;
    }

    std::fputs("{\n  \"mods\": [\n", file);
    bool first = true;
    for (const auto& mod : g_mods) {
        if (!mod.used) continue;
        char name[kModNameSize * 6 + 1]{}, version[kModMetadataSize * 6 + 1]{};
        char author[kModMetadataSize * 6 + 1]{}, commit[kModMetadataSize * 6 + 1]{};
        char build[kModMetadataSize * 6 + 1]{}, source[kSourcePathSize * 6 + 1]{};
        char symbols[kCrashPathSize * 2]{}, modulePath[kCrashPathSize * 2]{};
        EscapeJson(mod.name, name, sizeof(name));
        EscapeJson(mod.version, version, sizeof(version));
        EscapeJson(mod.author, author, sizeof(author));
        EscapeJson(mod.gitCommit, commit, sizeof(commit));
        EscapeJson(mod.buildId, build, sizeof(build));
        EscapeJson(mod.sourceRoot, source, sizeof(source));
        EscapeJson(mod.symbolPath, symbols, sizeof(symbols));
        EscapeJson(mod.path, modulePath, sizeof(modulePath));
        if (!first) std::fputs(",\n", file);
        first = false;
        std::fprintf(file,
                     "    {\"id\":%llu,\"name\":\"%s\",\"version\":\"%s\","
                     "\"author\":\"%s\",\"git_commit\":\"%s\",\"build_id\":\"%s\","
                     "\"source_root\":\"%s\",\"symbol_path\":\"%s\",\"path\":\"%s\","
                     "\"base\":\"0x%llX\",\"image_size\":%llu,\"manual\":%s,"
                     "\"loaded_at_ms\":%llu,\"last_activity_ms\":%llu}",
                     static_cast<unsigned long long>(mod.id), name, version, author, commit, build,
                     source, symbols, modulePath,
                     static_cast<unsigned long long>(mod.base),
                     static_cast<unsigned long long>(mod.imageSize),
                     mod.manuallyRegistered ? "true" : "false",
                     static_cast<unsigned long long>(mod.loadedAtMs),
                     static_cast<unsigned long long>(mod.lastActivityMs));
    }
    std::fputs("\n  ]\n}\n", file);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool WriteScopes(const char* directory) {
    char path[kCrashPathSize]{};
    std::snprintf(path, sizeof(path), "%s\\scopes.json", directory);
    FILE* file = nullptr;
    fopen_s(&file, path, "wb");
    if (!file) return false;

    TrySpinGuard guard(g_scopeLock);
    if (!guard) {
        std::fputs("{\"capture_error\":\"registry_busy\",\"scopes\":[]}", file);
        std::fclose(file);
        return false;
    }

    std::fputs("{\n  \"scopes\": [\n", file);
    bool first = true;
    for (const auto& scope : g_scopes) {
        if (!scope.used) continue;
        char name[kScopeNameSize * 6 + 1]{}, source[kSourcePathSize * 6 + 1]{};
        EscapeJson(scope.name, name, sizeof(name));
        EscapeJson(scope.file, source, sizeof(source));
        if (!first) std::fputs(",\n", file);
        first = false;
        std::fprintf(file,
                     "    {\"id\":%llu,\"parent_id\":%llu,\"mod_id\":%llu,"
                     "\"thread_id\":%lu,\"depth\":%u,\"entered_at_ms\":%llu,"
                     "\"name\":\"%s\",\"file\":\"%s\",\"line\":%d}",
                     static_cast<unsigned long long>(scope.id),
                     static_cast<unsigned long long>(scope.parentId),
                     static_cast<unsigned long long>(scope.modId),
                     static_cast<unsigned long>(scope.threadId), scope.depth,
                     static_cast<unsigned long long>(scope.enteredAtMs), name, source, scope.line);
    }
    std::fputs("\n  ]\n}\n", file);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool WriteValues(const char* directory) {
    char path[kCrashPathSize]{};
    std::snprintf(path, sizeof(path), "%s\\values.json", directory);
    FILE* file = nullptr;
    fopen_s(&file, path, "wb");
    if (!file) return false;

    TrySpinGuard guard(g_valueLock);
    if (!guard) {
        std::fputs("{\"capture_error\":\"registry_busy\",\"values\":[]}", file);
        std::fclose(file);
        return false;
    }

    const uint64_t next = g_nextValueSequence.load(std::memory_order_acquire);
    const uint64_t retained = (std::min)(next, static_cast<uint64_t>(kValueCapacity));
    const uint64_t firstSequence = next - retained;
    std::fprintf(file, "{\n  \"capacity\":%llu,\n  \"dropped\":%llu,\n  \"values\": [\n",
                 static_cast<unsigned long long>(kValueCapacity),
                 static_cast<unsigned long long>(firstSequence));
    bool first = true;
    for (uint64_t i = firstSequence; i < next; ++i) {
        const ValueSnapshot& value = g_values[i % kValueCapacity];
        if (value.sequence != i + 1) continue;
        char name[kValueNameSize * 6 + 1]{}, text[kValueTextSize * 6 + 1]{};
        EscapeJson(value.name, name, sizeof(name));
        EscapeJson(value.textValue, text, sizeof(text));
        if (!first) std::fputs(",\n", file);
        first = false;
        std::fprintf(file,
                     "    {\"sequence\":%llu,\"timestamp_ms\":%llu,\"thread_id\":%lu,"
                     "\"scope_id\":%llu,\"mod_id\":%llu,\"name\":\"%s\","
                     "\"type\":\"%s\",\"value\":",
                     static_cast<unsigned long long>(value.sequence),
                     static_cast<unsigned long long>(value.timestampMs),
                     static_cast<unsigned long>(value.threadId),
                     static_cast<unsigned long long>(value.scopeId),
                     static_cast<unsigned long long>(value.modId), name, ValueTypeName(value.type));
        switch (value.type) {
            case DiagnosticValueType::Pointer:
                std::fprintf(file, "\"0x%llX\"", static_cast<unsigned long long>(value.pointerValue));
                break;
            case DiagnosticValueType::Int64:
                std::fprintf(file, "%lld", static_cast<long long>(value.int64Value));
                break;
            case DiagnosticValueType::UInt64:
                std::fprintf(file, "%llu", static_cast<unsigned long long>(value.uint64Value));
                break;
            case DiagnosticValueType::Double:
                std::fprintf(file, "%.17g", value.doubleValue);
                break;
            case DiagnosticValueType::Bool:
                std::fputs(value.boolValue ? "true" : "false", file);
                break;
            case DiagnosticValueType::Text:
                std::fprintf(file, "\"%s\"", text);
                break;
        }
        std::fputc('}', file);
    }
    std::fputs("\n  ]\n}\n", file);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool FindNewestCrashDirectory(char* output, size_t outputSize) {
    if (!output || outputSize == 0 || !g_outputDirectory[0]) return false;
    char pattern[kCrashPathSize]{};
    std::snprintf(pattern, sizeof(pattern), "%s\\crash_*_%lu",
                  g_outputDirectory, static_cast<unsigned long>(GetCurrentProcessId()));
    WIN32_FIND_DATAA data{};
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    char newest[MAX_PATH]{};
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (std::strcmp(data.cFileName, ".") == 0 || std::strcmp(data.cFileName, "..") == 0) continue;
        if (!newest[0] || std::strcmp(data.cFileName, newest) > 0)
            RegistryCopyText(newest, sizeof(newest), data.cFileName);
    } while (FindNextFileA(find, &data));
    FindClose(find);
    if (!newest[0]) return false;
    std::snprintf(output, outputSize, "%s\\%s", g_outputDirectory, newest);
    return true;
}

LONG WINAPI RegistryUnhandledFilter(PEXCEPTION_POINTERS info) {
    char directory[kCrashPathSize]{};
    if (FindNewestCrashDirectory(directory, sizeof(directory)))
        WriteRegistrySnapshots(directory);
