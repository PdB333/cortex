#pragma once

// Data, serialization and machine-learning files, read offline: SQLite
// databases and WAL journals, binary property lists (and NSKeyedArchiver
// archives), Python pickles, NumPy .npy arrays, safetensors, GGUF and ONNX
// models. A pickle is disassembled by a symbolic stack, never unpickled:
// the imports it would perform are listed and judged, nothing is called.

#include "artifact.h"

namespace cortex::artifacts {

#define CORTEX_DATA_PROVIDER(Class, id, fmt)                                                    \
    class Class final : public Provider {                                                      \
    public:                                                                                    \
        const char* Name() const override { return id; }                                       \
        const char* Version() const override { return "1"; }                                   \
        std::vector<std::string> Formats() const override { return fmt; }                      \
        std::vector<Capability> Capabilities() const override;                                 \
        int Detect(const ByteSource& source) const override;                                   \
        json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override; \
    };

CORTEX_DATA_PROVIDER(SqliteProvider, "sqlite", (std::vector<std::string>{"sqlite", "sqlite-wal"}))
CORTEX_DATA_PROVIDER(BinaryPlistProvider, "bplist", (std::vector<std::string>{"bplist"}))
CORTEX_DATA_PROVIDER(PickleProvider, "pickle", (std::vector<std::string>{"pickle"}))
CORTEX_DATA_PROVIDER(NpyProvider, "numpy", (std::vector<std::string>{"npy"}))
CORTEX_DATA_PROVIDER(SafetensorsProvider, "safetensors", (std::vector<std::string>{"safetensors"}))
CORTEX_DATA_PROVIDER(GgufProvider, "gguf", (std::vector<std::string>{"gguf"}))
CORTEX_DATA_PROVIDER(OnnxProvider, "onnx", (std::vector<std::string>{"onnx"}))

#undef CORTEX_DATA_PROVIDER

// Pickle opcodes as a bounded report (also used for pickles inside other
// containers, e.g. PyTorch checkpoints and NumPy object arrays).
json DisassemblePickle(const std::vector<uint8_t>& bytes, size_t maxInstructions, std::string& error);

// The value of a pickle made only of plain data (dict, list, tuple, str,
// bytes as {"$bytes": hex}, int, float, bool, None). Any import, call or
// object construction makes it fail with "pickle_not_plain_data": nothing
// is ever imported or called.
json PickleLiteral(const std::vector<uint8_t>& bytes, std::string& error);

} // namespace cortex::artifacts
