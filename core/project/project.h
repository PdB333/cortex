#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace project {

using json = nlohmann::json;

// Loads (or creates) a JSON project file for the current process, named
// after its executable and stored in a "projects" folder next to this DLL.
// This is the AI's persistent long-term memory for a given game across
// sessions: named addresses, pointer paths, and free-form notes it has
// worked out. Every mutation below saves to disk immediately (simplicity
// and crash-safety over batching).
void Init();

json GetAll();

bool SetAddress(const std::string& name, uintptr_t address, const std::string& type, const std::string& notes);
bool RemoveAddress(const std::string& name);

bool SetPointerPath(const std::string& name, const std::string& moduleName, int64_t baseOffset,
                     const std::vector<int64_t>& offsets, const std::string& finalType, const std::string& notes);
bool RemovePointerPath(const std::string& name);

// module_base(moduleName) + base_offset, then for each offset in `offsets`:
// deref the pointer at the current address and add the next offset. The
// address after the last offset is returned as-is (not dereferenced) -- it
// is the target address itself, ready for /memory/read or /memory/write.
std::optional<uintptr_t> ResolvePointerPath(const std::string& name);

int AddNote(const std::string& text, const std::vector<std::string>& tags);
bool RemoveNote(int id);

// Pass-through persistence for core/freeze and core/struct: project.cpp does
// not know the shape of these entries, it just stores whatever JSON it is
// handed under its own key and returns it unchanged on the next load. The
// owning module (freeze::Init()/structs::Init()) decodes it back into its
// own in-memory registry after project::Init() has run.
json GetFreezes();
void SetFreezes(const json& freezes);
json GetStructDefs();
void SetStructDefs(const json& defs);

} // namespace project
