#include "symbols/symbols.h"
#include "symbols/external.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::string Escape(const std::string& value) {
    std::string output;
    for (unsigned char character : value) {
        switch (character) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output.push_back(static_cast<char>(character)); break;
        }
    }
    return output;
}

std::string DirectoryOf(const std::string& path) {
    const size_t position = path.find_last_of("\\/");
    return position == std::string::npos ? std::string() : path.substr(0, position);
}

void Usage() {
    std::fputs(
        "Usage: cortex_symbolize --image <dll-or-exe> --rva <hex> "
        "[--symbols <pdb-or-dir>] [--tool <llvm-symbolizer-or-addr2line>]\n",
        stderr);
}

} // namespace

int main(int argc, char** argv) {
    std::string image;
    std::string symbolPath;
    std::string toolPath;
    uintptr_t rva = 0;
    bool hasRva = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--image" && index + 1 < argc) image = argv[++index];
        else if (argument == "--rva" && index + 1 < argc) {
            rva = static_cast<uintptr_t>(std::strtoull(argv[++index], nullptr, 0));
            hasRva = true;
        } else if (argument == "--symbols" && index + 1 < argc) symbolPath = argv[++index];
        else if (argument == "--tool" && index + 1 < argc) toolPath = argv[++index];
        else if (argument == "--help" || argument == "-h") {
            Usage();
            return 0;
        } else {
            Usage();
            return 2;
        }
    }
    if (image.empty() || !hasRva) {
        Usage();
        return 2;
    }

    const symbols::ModuleIdentity identity = symbols::InspectModule(image);
    if (!identity.validPe) {
        std::fprintf(stderr, "Not a readable PE image: %s\n", image.c_str());
        return 3;
    }

    std::string searchPath = DirectoryOf(image);
    if (!symbolPath.empty()) {
        const DWORD attributes = GetFileAttributesA(symbolPath.c_str());
        const std::string symbolDirectory =
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)
                ? symbolPath : DirectoryOf(symbolPath);
        if (!symbolDirectory.empty()) {
            if (!searchPath.empty()) searchPath.push_back(';');
            searchPath += symbolDirectory;
        }
    }

    const uintptr_t base = static_cast<uintptr_t>(identity.preferredImageBase);
    if (toolPath.empty()) {
        symbols::Init(searchPath, false);
        const symbols::ModuleLoadInfo loaded = symbols::LoadModule(
            base, identity.imageSize, image, symbolPath);
        const auto resolved = symbols::ResolveDetailed(base + rva);
        if (resolved && (resolved->hasSymbol || resolved->hasLine)) {
            std::printf(
                "{\"ok\":true,\"backend\":\"dbghelp\",\"image\":\"%s\","
                "\"rva\":\"0x%llX\",\"build_id\":\"%s\",\"symbol\":\"%s\","
                "\"displacement\":%llu,\"file\":\"%s\",\"line\":%u,"
                "\"loaded_pdb\":\"%s\",\"exact_match\":%s,\"verification\":\"%s\"}\n",
                Escape(image).c_str(), static_cast<unsigned long long>(rva),
                Escape(identity.buildId).c_str(), Escape(resolved->symbolName).c_str(),
                static_cast<unsigned long long>(resolved->displacement),
                Escape(resolved->file).c_str(), resolved->line,
                Escape(loaded.loadedPdb).c_str(), loaded.exactMatch ? "true" : "false",
                Escape(loaded.verification).c_str());
            symbols::Shutdown();
            return 0;
        }
        symbols::Shutdown();
    }

    const auto external = symbols::ResolveExternal(image, rva, toolPath);
    if (external) {
        std::printf(
            "{\"ok\":true,\"backend\":\"external\",\"image\":\"%s\","
            "\"rva\":\"0x%llX\",\"build_id\":\"%s\",\"symbol\":\"%s\","
            "\"file\":\"%s\",\"line\":%u,\"tool\":\"%s\"}\n",
            Escape(image).c_str(), static_cast<unsigned long long>(rva),
            Escape(identity.buildId).c_str(), Escape(external->function).c_str(),
            Escape(external->file).c_str(), external->line, Escape(external->tool).c_str());
        return 0;
    }

    std::printf(
        "{\"ok\":false,\"image\":\"%s\",\"rva\":\"0x%llX\","
        "\"build_id\":\"%s\",\"error\":\"no_matching_symbols\"}\n",
        Escape(image).c_str(), static_cast<unsigned long long>(rva),
        Escape(identity.buildId).c_str());
    return 1;
}
