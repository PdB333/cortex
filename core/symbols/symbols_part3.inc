    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initialized;
}

bool AddSearchPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    AppendSearchPath(g_searchPath, path);
    return !g_initialized || ApplySearchPathLocked();
}

std::string GetSearchPath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_searchPath;
}

ModuleIdentity InspectModule(const std::string& imagePath) {
    MappedImage image;
    if (!MapImage(imagePath, image)) return {};
    return InspectMappedImage(image);
}

ModuleLoadInfo LoadModule(uintptr_t base, size_t imageSize,
                          const std::string& imagePath,
                          const std::string& preferredSymbolPath) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        ModuleLoadInfo result;
        result.base = base;
        result.imageSize = imageSize;
        result.imagePath = imagePath;
        result.error = ERROR_INVALID_STATE;
        result.verification = "symbol_session_not_initialized";
        return result;
    }
    return LoadModuleLocked(base, imageSize, imagePath, preferredSymbolPath);
}

std::optional<ModuleLoadInfo> GetModuleInfo(uintptr_t base) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto found = g_modules.find(base);
    if (found == g_modules.end()) return std::nullopt;
    return found->second;
}

std::optional<ResolvedLocation> ResolveDetailed(uintptr_t address) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return std::nullopt;
    return ResolveDetailedLocked(address);
}

std::optional<SymbolInfo> Resolve(uintptr_t address) {
    auto detailed = ResolveDetailed(address);
    if (!detailed || !detailed->hasSymbol) return std::nullopt;
    SymbolInfo out;
    out.name = detailed->symbolName;
    out.symbolAddress = detailed->symbolAddress;
    out.displacement = detailed->displacement;
    return out;
}

std::optional<LineInfo> ResolveLine(uintptr_t address) {
    auto detailed = ResolveDetailed(address);
    if (!detailed || !detailed->hasLine) return std::nullopt;
    LineInfo out;
    out.file = detailed->file;
    out.line = detailed->line;
    return out;
}

std::optional<uintptr_t> Lookup(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return std::nullopt;
    std::array<uint8_t, sizeof(SYMBOL_INFO) + MAX_SYM_NAME + 1> buffer{};
    auto* symbol = reinterpret_cast<PSYMBOL_INFO>(buffer.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    if (!SymFromName(g_process, name.c_str(), symbol)) return std::nullopt;
    return static_cast<uintptr_t>(symbol->Address);
}

size_t CaptureStack(const CONTEXT& sourceContext, HANDLE thread,
                    StackFrame* output, size_t capacity,
                    StackCapture* capture) {
    StackCapture local;
    StackCapture& state = capture ? *capture : local;
    state = {};
    if (!output || capacity == 0) return 0;

    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    state.lockAcquired = lock.owns_lock();
    state.initialized = state.lockAcquired && g_initialized;
    if (!state.lockAcquired || !g_initialized) {
        const uintptr_t address = InstructionPointer(sourceContext);
        if (address) {
            output[0] = {};
            output[0].index = 0;
            output[0].address = address;
            uintptr_t base = 0;
            size_t imageSize = 0;
            std::string path;
            if (ModuleForAddress(address, base, imageSize, path)) {
                output[0].moduleBase = base;
                output[0].moduleRva = address - base;
                output[0].modulePath = path;
                output[0].moduleName = BaseName(path);
                output[0].buildId = InspectModule(path).buildId;
            }
            state.frameCount = 1;
            state.error = ERROR_BUSY;
            return 1;
        }
        state.error = ERROR_BUSY;
        return 0;
    }

    CONTEXT context = sourceContext;
    STACKFRAME64 frame{};
    DWORD machine = 0;
#if defined(_M_X64) || defined(__x86_64__)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_IX86) || defined(__i386__)
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrStack.Offset = context.Esp;
#else
    state.error = ERROR_NOT_SUPPORTED;
    return 0;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    size_t count = 0;
    const uintptr_t firstAddress = InstructionPointer(context);
    if (firstAddress) {
        output[count] = {};
        static_cast<ResolvedLocation&>(output[count]) = ResolveDetailedLocked(firstAddress);
        output[count].index = static_cast<uint32_t>(count);
        ++count;
    }

    while (count < capacity) {
        SetLastError(ERROR_SUCCESS);
        const BOOL walked = StackWalk64(machine, g_process,
                                        thread ? thread : GetCurrentThread(),
                                        &frame, &context, nullptr,
                                        SymFunctionTableAccess64,
                                        SymGetModuleBase64, nullptr);
        if (!walked || frame.AddrPC.Offset == 0) {
            state.error = GetLastError();
            break;
        }
        const uintptr_t address = static_cast<uintptr_t>(frame.AddrPC.Offset);
        if (count > 0 && output[count - 1].address == address) break;
        output[count] = {};
        static_cast<ResolvedLocation&>(output[count]) = ResolveDetailedLocked(address);
        output[count].index = static_cast<uint32_t>(count);
        ++count;
        state.stackWalkSucceeded = true;
    }
    state.frameCount = count;
    return count;
}

} // namespace symbols
