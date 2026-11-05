// Text and stream decoders: JavaScript/TypeScript lexer and analysis, source
// maps (including base64 data URLs) and DEFLATE.
#include "artifacts/inflate.h"
#include "artifacts/js_analysis.h"
#include "artifacts/js_lexer.h"
#include "artifacts/source_map.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace cortex::artifacts;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const uint8_t mode = data[0];
    std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);
    switch (mode % 4) {
        case 0: {
            js::Lex(text, 1u << 16);
            js::Analyze(text, (mode & 4) ? "input.ts" : "input.js", 256);
            break;
        }
        case 1: {
            SourceMap map;
            std::string error;
            if (ParseSourceMap(text, map, error, 1u << 16)) {
                LookupMapping(map, 0, 0);
                LookupMapping(map, 3, 17);
                LookupMapping(map, 0xFFFFFFFFu, 0xFFFFFFFFu);
            }
            break;
        }
        case 2: {
            std::string out;
            DecodeBase64(text, out);
            DecodeDataUrl(text, out);
            break;
        }
        case 3: {
            std::vector<uint8_t> output;
            std::string error;
            Inflate(data + 1, size - 1, output, 1u << 22, error);
            break;
        }
    }
    return 0;
}
