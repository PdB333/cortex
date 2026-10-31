#pragma once

// ECMA-335 metadata of a .NET assembly, read from the PE file (step 4c):
// the CLR header, the metadata root and streams, the #~ table schema (all
// 45 tables, so any assembly is laid out correctly even when only some
// tables are decoded), heaps, signatures and method bodies. The bytes are
// untrusted: every index is checked against its table or heap, signature
// decoding is depth-limited, and failures name what was wrong.
//
// Names follow ilasm: Namespace.Name, nested types as Outer/Inner, method
// signatures "ret (p1, p2)", generic parameters !n / !!n, Name`n<args>.

#include "artifact.h"
#include "pe_provider.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::artifacts::dotnet {

enum Table : uint8_t {
    Module = 0x00, TypeRef = 0x01, TypeDef = 0x02, FieldPtr = 0x03, Field = 0x04, MethodPtr = 0x05,
    MethodDef = 0x06, ParamPtr = 0x07, Param = 0x08, InterfaceImpl = 0x09, MemberRef = 0x0A, Constant = 0x0B,
    CustomAttribute = 0x0C, FieldMarshal = 0x0D, DeclSecurity = 0x0E, ClassLayout = 0x0F, FieldLayout = 0x10,
    StandAloneSig = 0x11, EventMap = 0x12, EventPtr = 0x13, Event = 0x14, PropertyMap = 0x15, PropertyPtr = 0x16,
    Property = 0x17, MethodSemantics = 0x18, MethodImpl = 0x19, ModuleRef = 0x1A, TypeSpec = 0x1B, ImplMap = 0x1C,
    FieldRva = 0x1D, EncLog = 0x1E, EncMap = 0x1F, Assembly = 0x20, AssemblyProcessor = 0x21, AssemblyOs = 0x22,
    AssemblyRef = 0x23, AssemblyRefProcessor = 0x24, AssemblyRefOs = 0x25, File = 0x26, ExportedType = 0x27,
    ManifestResource = 0x28, NestedClass = 0x29, GenericParam = 0x2A, MethodSpec = 0x2B,
    GenericParamConstraint = 0x2C, TableCount = 0x2D,
};

const char* TableName(uint8_t table);

struct ExceptionClause {
    std::string kind;            // catch | filter | finally | fault
    uint32_t tryOffset = 0, tryLength = 0, handlerOffset = 0, handlerLength = 0;
    uint32_t classTokenOrFilter = 0;
};

struct MethodBody {
    bool tiny = false;
    uint16_t maxStack = 0;
    uint32_t localSignature = 0;  // StandAloneSig token, 0 if none
    bool initLocals = false;
    std::vector<uint8_t> code;
    std::vector<ExceptionClause> clauses;
};

class Metadata {
public:
    // Reads the CLR header and metadata of `pe`. False with `error` set if
    // the PE has no usable metadata.
    bool Parse(const ByteSource& source, const PeFile& pe, std::string& error);

    uint32_t Rows(uint8_t table) const { return table < TableCount ? rows_[table] : 0; }
    // Column value of a 1-based row; 0 when out of range.
    uint32_t Cell(uint8_t table, uint32_t row, size_t column) const;

    std::string String(uint32_t index) const;
    std::string UserString(uint32_t index) const;
    std::string Guid(uint32_t index) const;
    // Blob bytes; false if the index or length is outside the heap.
    bool Blob(uint32_t index, const uint8_t*& data, uint32_t& size) const;

    // Names.
    std::string TypeDefName(uint32_t row) const;
    std::string TypeRefName(uint32_t row) const;
    // A TypeDefOrRef coded index, or a token of TypeDef/TypeRef/TypeSpec.
    std::string TypeName(uint32_t token) const;
    // Parent::Name for MethodDef, Field, MemberRef, MethodSpec tokens; the
    // type for type tokens; "sig" for StandAloneSig.
    std::string MemberName(uint32_t token) const;

    // Signatures as text.
    std::string MethodSignature(uint32_t blob) const;
    std::string FieldSignature(uint32_t blob) const;
    std::string TypeSpecSignature(uint32_t blob) const;

    // Declaring type of a MethodDef / Field row (0 if none).
    uint32_t MethodOwner(uint32_t methodRow) const;
    uint32_t FieldOwner(uint32_t fieldRow) const;
    // Member ranges of a TypeDef row: [first, end) in MethodDef / Field / Param.
    void Methods(uint32_t typeRow, uint32_t& first, uint32_t& end) const;
    void Fields(uint32_t typeRow, uint32_t& first, uint32_t& end) const;
    void Params(uint32_t methodRow, uint32_t& first, uint32_t& end) const;
    // Through MethodPtr/FieldPtr/ParamPtr when the tables are unsorted (#-).
    uint32_t MethodAt(uint32_t index) const;
    uint32_t FieldAt(uint32_t index) const;
    uint32_t ParamAt(uint32_t index) const;

    // Body of a MethodDef row (RVA != 0).
    bool ReadBody(const ByteSource& source, const PeFile& pe, uint32_t methodRow, MethodBody& body,
                  std::string& error) const;

    // The summary the provider returns: identity, references, types and
    // members (bounded), resources, P/Invoke, table row counts.
    json Summary(const InspectOptions& options) const;

    std::string version;          // metadata root version string ("v4.0.30319")
    uint16_t runtimeMajor = 0, runtimeMinor = 0;
    uint32_t clrFlags = 0;
    uint32_t entryPointToken = 0;
    PeDirectory resources{};
    PeDirectory strongNameSignature{};
    std::vector<std::pair<std::string, uint32_t>> streams;  // name, size
    std::vector<std::string> warnings;

private:
    struct Heap {
        uint32_t offset = 0, size = 0;  // inside metadata_
    };
    bool LayoutTables(std::string& error);
    uint32_t ReadIndex(uint64_t at, uint8_t width) const;
    std::string TypeFromSignature(const uint8_t*& p, const uint8_t* end, int depth) const;
    std::string TypeDefOrRefEncoded(uint32_t value) const;
    std::string ParentName(uint32_t codedMemberRefParent) const;

    std::vector<uint8_t> metadata_;
    Heap strings_, userStrings_, guids_, blobs_, tables_;
    std::array<uint32_t, TableCount> rows_{};
    std::array<uint32_t, TableCount> rowSize_{};
    std::array<uint64_t, TableCount> tableOffset_{};
    std::array<std::vector<uint8_t>, TableCount> columnWidths_{};
    bool wideStrings_ = false, wideGuids_ = false, wideBlobs_ = false;
    std::vector<uint32_t> nestedEnclosing_;  // TypeDef row -> enclosing row (0 if not nested)
    std::vector<uint32_t> methodOwner_;      // MethodDef row -> TypeDef row
    std::vector<uint32_t> fieldOwner_;       // Field row -> TypeDef row
};

// Compressed unsigned integer (ECMA-335 II.23.2); false if truncated.
bool ReadCompressed(const uint8_t*& p, const uint8_t* end, uint32_t& value);

} // namespace cortex::artifacts::dotnet
