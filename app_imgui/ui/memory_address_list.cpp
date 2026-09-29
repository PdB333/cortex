#include "memory_workspace.h"
#include "memory_workspace_internal.h"
#include "widgets.h"
#include "address_context_menu.h"
#include "address_resolver.h"
#include "auto_assembler_host.h"
#include "hotkey_capture.h"

#include "application/address_expression.h"
#include "application/cheat_table.h"
#include "application/hotkeys.h"
#include "file_dialog.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

// The Memory workspace address list: entries, freezing, pointers and
// address expressions, groups, cheat tables, clipboard, per-entry hotkeys.

namespace cortex::ui {
namespace {

using namespace memory_internal;

constexpr int kHotkeyActionCount = 8;
const char* const kHotkeyActionNames[kHotkeyActionCount] = {
    "Toggle freeze", "Toggle freeze (allow increase)", "Toggle freeze (allow decrease)",
    "Freeze", "Unfreeze", "Set value", "Increase value by", "Decrease value by"};
// Cheat Engine's names for the same actions, in the same order.
const char* const kCheatEngineHotkeyActions[kHotkeyActionCount] = {
    "Toggle Activation", "Toggle Activation Allow Increase", "Toggle Activation Allow Decrease",
    "Activate", "Deactivate", "Set Value", "Increase Value", "Decrease Value"};

bool ActionNeedsValue(int action) {
    return action >= 5;
}

struct NamedColor {
    const char* name;
    uint32_t rgb;
};
const NamedColor kColors[] = {
    {"Red", 0xE8574F}, {"Orange", 0xEE9A3A}, {"Yellow", 0xE8D44D}, {"Green", 0x5BC46A},
    {"Teal", 0x3FBFB0}, {"Blue", 0x5A9BF0}, {"Purple", 0xA077E6}, {"Pink", 0xE36FBE}, {"Gray", 0x9AA0A6}};

ImVec4 ColorFromRgb(int64_t rgb) {
    return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f);
}

std::string HotkeyCommand(uint32_t uid, size_t index) {
    return "entry_hotkey " + std::to_string(uid) + " " + std::to_string(index);
}

const char* VariableTypeName(ScanDataType type) {
    switch (type) {
        case ScanDataType::Byte: return "Byte";
        case ScanDataType::Int16: return "2 Bytes";
        case ScanDataType::Int64: return "8 Bytes";
        case ScanDataType::Float: return "Float";
        case ScanDataType::Double: return "Double";
        case ScanDataType::String: return "String";
        case ScanDataType::ByteArray: return "Array of byte";
        default: return "4 Bytes";
    }
}

ScanDataType TypeFromVariableType(const std::string& type) {
    return type == "Byte" ? ScanDataType::Byte : type == "2 Bytes" ? ScanDataType::Int16
         : type == "8 Bytes" ? ScanDataType::Int64 : type == "Float" ? ScanDataType::Float
         : type == "Double" ? ScanDataType::Double : type == "String" ? ScanDataType::String
         : type == "Array of byte" ? ScanDataType::ByteArray : ScanDataType::Int32;
}

std::string OffsetsText(const std::vector<uint32_t>& offsets) {
    std::string text;
    char buffer[16] = {};
    for (const auto offset : offsets) {
        std::snprintf(buffer, sizeof(buffer), "%s%X", text.empty() ? "" : " ", offset);
        text += buffer;
    }
    return text;
}

} // namespace

// ------------------------------------------------------------------ model

bool MemoryWorkspace::HasAddress(uint64_t address, ScanDataType type) const {
    return std::any_of(addresses_.begin(), addresses_.end(), [&](const AddressEntry& entry) {
        return !entry.group && entry.module.empty() && !entry.pointer && entry.expression.empty() &&
               entry.address == address && entry.type == type;
    });
}

int MemoryWorkspace::EntryIndex(uint32_t uid) const {
    for (size_t i = 0; i < addresses_.size(); ++i)
        if (addresses_[i].uid == uid) return static_cast<int>(i);
    return -1;
}

// A group header and the entries nested under it.
size_t MemoryWorkspace::BlockEnd(size_t index) const {
    size_t end = index + 1;
    if (index < addresses_.size() && addresses_[index].group)
        while (end < addresses_.size() && addresses_[end].depth > addresses_[index].depth) ++end;
    return end;
}

const services::CustomType* MemoryWorkspace::EntryCustomType(const AddressEntry& entry) const {
    if (entry.customType.empty() || !customTypes_) return nullptr;
    return customTypes_->Find(entry.customType);
}

std::string MemoryWorkspace::FormatEntry(const AddressEntry& entry) const {
    if (!entry.readable) return "??";
    if (const auto* custom = EntryCustomType(entry)) {
        double value = 0;
        if (!services::ReadCustomType(*custom, entry.lastValue.data(), entry.lastValue.size(), value))
            return "??";
        return services::FormatCustomValue(*custom, value);
    }
    return ValueScanner::Format(entry.lastValue.data(), entry.lastValue.size(), entry.type, entry.hex,
                                entry.unsignedValue, entry.utf16);
}

// The value, or its dropdown label ("100 : Full").
std::string MemoryWorkspace::DisplayValue(const AddressEntry& entry) const {
    const std::string text = FormatEntry(entry);
    // A dropdown lists values of a standard type; a custom type reads them
    // differently, so its entries show the value alone.
    if (entry.dropDown.empty() || !entry.readable || EntryCustomType(entry)) return text;
    for (const auto& item : entry.dropDown) {
        std::vector<uint8_t> bytes;
        if (!ValueScanner::Encode(item.value, entry.type, entry.hex, entry.utf16, bytes)) continue;
        if (bytes.size() > entry.lastValue.size() ||
            !std::equal(bytes.begin(), bytes.end(), entry.lastValue.begin()))
            continue;
        return entry.dropDownOnly ? item.label : text + " : " + item.label;
    }
    return text;
}

std::string MemoryWorkspace::PointerText(const AddressEntry& entry) const {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "+%llX", static_cast<unsigned long long>(entry.baseOffset));
    std::string text = !entry.expression.empty() ? entry.expression
                     : entry.module.empty() ? Hex(entry.baseOffset) : entry.module + buffer;
    for (const auto offset : entry.offsets) {
        std::snprintf(buffer, sizeof(buffer), " -> %X", offset);
        text += buffer;
    }
    return text;
}

bool MemoryWorkspace::ResolveEntry(UiContext& context, AddressEntry& entry, std::string* error) {
    if (entry.group || !context.memory) return false;
    auto readPointer = [&](uint64_t at, uint64_t& value) {
        std::vector<uint8_t> bytes;
        std::string readError;
        if (!context.memory->Read(at, entry.pointerSize, bytes, &readError) || bytes.size() != entry.pointerSize)
            return false;
        value = 0;
        std::memcpy(&value, bytes.data(), entry.pointerSize);
        return true;
    };
    uint64_t address = entry.baseOffset;
    if (!entry.expression.empty()) {
        // Refreshes run every 100 ms: no module list refresh on failure.
        if (!EvaluateContextAddress(context, entry.expression, address, error, false)) return false;
    } else if (entry.module.empty() && entry.offsets.empty()) {
        return true;
    } else if (!entry.module.empty()) {
        const target::ModuleInfo* module = nullptr;
        for (const auto& candidate : modules_)
            if (Lower(candidate.name) == Lower(entry.module)) module = &candidate;
        uint64_t base = 0;
        if (module) {
            base = module->base;
        } else if (!ContextSymbols(context).Resolve(entry.module, base)) {
            // Not a module: an export (kernel32.Sleep) or a user symbol.
            if (error) *error = "Module or symbol " + entry.module + " is not known";
            return false;
        }
        address = base + entry.baseOffset;
    }
    for (const auto offset : entry.offsets) {
        uint64_t value = 0;
        if (!readPointer(address, value)) {
            if (error) *error = "The pointer at " + Hex(address) + " cannot be read";
            return false;
        }
        address = value + offset;
    }
    entry.address = address;
    return true;
}

// An absolute address inside a module becomes module+offset, which stays
// valid after the game restarts.
void MemoryWorkspace::MakeStatic(AddressEntry& entry) const {
    if (!entry.module.empty() || !entry.offsets.empty() || entry.group || !entry.expression.empty()) return;
    if (const auto* module = ModuleFor(entry.address)) {
        entry.module = module->name;
        entry.baseOffset = entry.address - module->base;
    }
}

void MemoryWorkspace::SelectEntry(size_t index) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift && selectionAnchor_ < addresses_.size()) {
        const size_t from = std::min(selectionAnchor_, index);
        const size_t to = std::max(selectionAnchor_, index);
        for (size_t i = 0; i < addresses_.size(); ++i) addresses_[i].selected = i >= from && i <= to;
        return;
    }
    if (io.KeyCtrl) {
        addresses_[index].selected = !addresses_[index].selected;
    } else {
        for (auto& entry : addresses_) entry.selected = false;
        addresses_[index].selected = true;
    }
    selectionAnchor_ = index;
}

void MemoryWorkspace::SetFreeze(AddressEntry& entry, bool freeze) {
    if (entry.group) return;
    if (freeze && !entry.freeze) entry.frozenValue = entry.lastValue;
    entry.freeze = freeze;
}

unsigned MemoryWorkspace::TargetPointerSize(UiContext& context) const {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    return session && session->Target().architecture == target::Architecture::X86 ? 4u : 8u;
}

// ------------------------------------------------------------------ tables

application::CheatTableEntry MemoryWorkspace::ToTableEntry(const AddressEntry& entry) const {
    application::CheatTableEntry item;
    item.description = entry.description;
    item.depth = entry.depth;
    item.groupHeader = entry.group;
    item.collapsed = entry.collapsed;
    item.color = entry.color;
    item.showAsHex = entry.hex;
    item.showAsSigned = !entry.unsignedValue;
    item.unicode = entry.utf16;
    for (const auto& value : entry.dropDown) item.dropDown.push_back({value.value, value.label});
    item.dropDownDescriptionOnly = entry.dropDownOnly;
    for (const auto& hotkey : entry.hotkeys) {
        application::HotkeyChord chord;
        if (!application::ParseHotkeyChord(hotkey.chord, chord)) continue;
        application::CheatTableHotkey out;
        out.action = kCheatEngineHotkeyActions[static_cast<int>(hotkey.action)];
        out.keys = application::HotkeyChordKeys(chord);
        out.value = hotkey.value;
        item.hotkeys.push_back(std::move(out));
    }
    if (entry.script) {
        item.script = true;
        item.assemblerScript = entry.assemblerScript;
        item.variableType = "Auto Assembler Script";
    } else if (!entry.group) {
        item.variableType = entry.customType.empty() ? VariableTypeName(entry.type) : "Custom";
        item.customType = entry.customType;
        item.length = static_cast<int>(entry.type == ScanDataType::String && entry.utf16 ? entry.size / 2 : entry.size);
        item.offsets = entry.offsets;
        if (!entry.expression.empty()) item.address = entry.expression;
        else if (!entry.module.empty() || entry.pointer) item.address = application::FormatCheatAddress(entry.module, entry.baseOffset);
        else item.address = application::FormatCheatAddress("", entry.address);
    }
    return item;
}

MemoryWorkspace::AddressEntry MemoryWorkspace::FromTableEntry(const application::CheatTableEntry& item,
                                                              unsigned pointerSize) const {
    AddressEntry entry;
    entry.description = item.description.empty() ? "No description" : item.description;
    entry.depth = item.depth;
    entry.group = item.groupHeader;
    entry.collapsed = item.collapsed;
    entry.color = item.color;
    entry.hex = item.showAsHex;
    entry.unsignedValue = !item.showAsSigned;
    entry.utf16 = item.unicode;
    entry.pointerSize = pointerSize;
    for (const auto& value : item.dropDown) entry.dropDown.push_back({value.value, value.label});
    entry.dropDownOnly = item.dropDownDescriptionOnly;
    for (const auto& hotkey : item.hotkeys) {
        const auto* action = std::find_if(std::begin(kCheatEngineHotkeyActions), std::end(kCheatEngineHotkeyActions),
                                          [&](const char* name) { return hotkey.action == name; });
        application::HotkeyChord chord;
        if (action == std::end(kCheatEngineHotkeyActions) || !application::HotkeyChordFromKeys(hotkey.keys, chord))
            continue;
        EntryHotkey out;
        out.chord = application::FormatHotkeyChord(chord);
        out.action = static_cast<EntryHotkeyAction>(action - std::begin(kCheatEngineHotkeyActions));
        out.value = hotkey.value;
        entry.hotkeys.push_back(std::move(out));
    }
    if (item.script) {
        entry.script = true;
        entry.assemblerScript = item.assemblerScript;
    } else if (!entry.group) {
        entry.type = TypeFromVariableType(item.variableType);
        const size_t fixed = ValueScanner::TypeSize(entry.type);
        entry.size = fixed ? fixed
            : static_cast<size_t>(std::max(1, item.length)) * (entry.type == ScanDataType::String && item.unicode ? 2 : 1);
        // A user-defined type decides its own width; a table that names one
        // Cortex does not have keeps the name, and the entry reads as the
        // standard type until the type is defined.
        if (item.variableType == "Custom" && !item.customType.empty()) {
            entry.customType = item.customType;
            if (customTypes_)
                if (const auto* custom = customTypes_->Find(item.customType)) entry.size = custom->size;
        }
        entry.offsets = item.offsets;
        entry.pointer = !item.offsets.empty();
        std::string module;
        uint64_t offset = 0;
        if (application::ParseCheatAddress(item.address, module, offset)) {
            entry.module = module;
            entry.baseOffset = offset;
            if (module.empty() && !entry.pointer) entry.address = offset;
        } else {
            entry.expression = item.address;
        }
    }
    return entry;
}

void MemoryWorkspace::OpenTable(UiContext& context) {
    std::string path = tablePath_;
    if (!ShowOpenFileDialog(L"Cheat tables (*.CT)\0*.CT\0All files\0*.*\0", path)) return;
    application::CheatTable table;
    std::string error;
    if (!application::LoadCheatTable(path, table, &error)) {
        context.status = "Open table failed: " + error;
        return;
    }
    const unsigned pointerSize = TargetPointerSize(context);
    // Symbols first: entries may use them.
    size_t unresolved = 0;
    for (const auto& symbol : table.userSymbols) {
        uint64_t address = 0;
        if (context.userSymbols && EvaluateContextAddress(context, symbol.second, address)) context.userSymbols->Set(symbol.first, address);
        else ++unresolved;
    }
    size_t skippedHotkeys = 0;
    for (const auto& item : table.entries) {
        auto entry = FromTableEntry(item, pointerSize);
        skippedHotkeys += item.hotkeys.size() - entry.hotkeys.size();
        addresses_.push_back(std::move(entry));
    }
    const size_t imported = table.entries.size();
    tablePath_ = path;
    lastAddressRefresh_ = {};
    context.status = "Opened " + std::to_string(imported) + " entr" + (imported == 1 ? "y" : "ies") + " from " + path;
    if (table.scripts) context.status += "; " + std::to_string(table.scripts) + " Auto Assembler script(s) not imported";
    if (table.unsupported) context.status += "; " + std::to_string(table.unsupported) + " unsupported entr(ies) skipped";
    if (skippedHotkeys) context.status += "; " + std::to_string(skippedHotkeys) + " multi-key hotkey(s) skipped";
    if (!table.userSymbols.empty())
        context.status += "; " + std::to_string(table.userSymbols.size() - unresolved) + " symbol(s) registered";
    if (unresolved) context.status += " (" + std::to_string(unresolved) + " could not be resolved)";
    if (!table.luaScript.empty()) {
        tableLuaScript_ = table.luaScript;
        openLuaPrompt_ = true;
    }
}

void MemoryWorkspace::SaveTable(UiContext& context) {
    std::string path = tablePath_;
    if (!ShowSaveFileDialog(L"Cheat tables (*.CT)\0*.CT\0All files\0*.*\0", L"CT", path)) return;
    application::CheatTable table;
    for (const auto& entry : addresses_) table.entries.push_back(ToTableEntry(entry));
    table.luaScript = tableLuaScript_;
    if (context.userSymbols)
        for (const auto& symbol : context.userSymbols->List()) {
            char text[32] = {};
            std::snprintf(text, sizeof(text), "%llX", static_cast<unsigned long long>(symbol.second));
            table.userSymbols.emplace_back(symbol.first, text);
        }
    std::string error;
    if (!application::SaveCheatTable(path, table, &error)) {
        context.status = "Save table failed: " + error;
        return;
    }
    tablePath_ = path;
    context.status = "Saved " + std::to_string(table.entries.size()) + " entr" +
                     (table.entries.size() == 1 ? "y" : "ies") + " to " + path;
}

// The clipboard carries entries as a cheat table, the format Cheat Engine
// itself copies, so entries paste both ways.
void MemoryWorkspace::CopyEntries(bool selectedOnly) {
    application::CheatTable table;
    int minDepth = INT_MAX;
    for (size_t i = 0; i < addresses_.size();) {
        if (selectedOnly && !addresses_[i].selected) {
            ++i;
            continue;
        }
        const size_t end = BlockEnd(i);
        for (size_t j = i; j < end; ++j) {
            table.entries.push_back(ToTableEntry(addresses_[j]));
            minDepth = std::min(minDepth, addresses_[j].depth);
        }
        i = end;
    }
    if (table.entries.empty()) return;
    for (auto& item : table.entries) item.depth -= minDepth;
    ImGui::SetClipboardText(application::WriteCheatTable(table).c_str());
}

void MemoryWorkspace::PasteEntries(UiContext& context) {
    const char* text = ImGui::GetClipboardText();
    application::CheatTable table;
    std::string error;
    if (!text || !application::ParseCheatTable(text, table, &error) || table.entries.empty()) {
        context.status = "The clipboard holds no address list entries";
        return;
    }
    for (auto& entry : addresses_) entry.selected = false;
    const unsigned pointerSize = TargetPointerSize(context);
    for (const auto& item : table.entries) {
        auto entry = FromTableEntry(item, pointerSize);
        entry.selected = true;
        addresses_.push_back(std::move(entry));
    }
    lastAddressRefresh_ = {};
    context.status = "Pasted " + std::to_string(table.entries.size()) + " entr" +
                     (table.entries.size() == 1 ? "y" : "ies");
}

// Moves the selected entries under a new group header.
void MemoryWorkspace::GroupSelected(UiContext& context) {
    AddressEntry header;
    header.group = true;
    header.description = "New group";
    header.selected = true;
    std::vector<AddressEntry> rest;
    std::vector<AddressEntry> moved;
    size_t insertAt = SIZE_MAX;
    for (auto& entry : addresses_) {
        if (entry.selected && !entry.group) {
            if (insertAt == SIZE_MAX) {
                insertAt = rest.size();
                header.depth = entry.depth;
            }
            moved.push_back(std::move(entry));
        } else {
            rest.push_back(std::move(entry));
        }
    }
    if (moved.empty()) {
        header.depth = 0;
        rest.push_back(std::move(header));
        addresses_ = std::move(rest);
        context.status = "Group header added";
        return;
    }
    for (auto& entry : rest) entry.selected = false;
    for (auto& entry : moved) {
        entry.depth = header.depth + 1;
        entry.selected = false;
    }
    rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(insertAt), std::move(header));
    rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(insertAt) + 1, std::make_move_iterator(moved.begin()),
                std::make_move_iterator(moved.end()));
    addresses_ = std::move(rest);
    context.status = "Grouped " + std::to_string(moved.size()) + " entr" + (moved.size() == 1 ? "y" : "ies");
}

// Moves an entry (a group with its entries) before another row, at that
// row's depth or at `depth`. With before == size() it goes to the end.
void MemoryWorkspace::MoveBlock(size_t from, size_t before, int depth) {
    if (from >= addresses_.size() || before > addresses_.size()) return;
    const size_t end = BlockEnd(from);
    if (before >= from && before <= end) return;  // onto itself
    if (depth < 0) depth = before < addresses_.size() ? addresses_[before].depth : 0;
    std::vector<AddressEntry> block(std::make_move_iterator(addresses_.begin() + static_cast<std::ptrdiff_t>(from)),
                                    std::make_move_iterator(addresses_.begin() + static_cast<std::ptrdiff_t>(end)));
    const int delta = depth - block.front().depth;
    for (auto& entry : block) entry.depth = std::max(0, entry.depth + delta);
    addresses_.erase(addresses_.begin() + static_cast<std::ptrdiff_t>(from),
                     addresses_.begin() + static_cast<std::ptrdiff_t>(end));
    const size_t target = before > from ? before - (end - from) : before;
    addresses_.insert(addresses_.begin() + static_cast<std::ptrdiff_t>(target), std::make_move_iterator(block.begin()),
                      std::make_move_iterator(block.end()));
}

void MemoryWorkspace::TakePendingAddresses(UiContext& context) {
    if (context.pendingAddresses.empty()) return;
    for (auto& pending : context.pendingAddresses) {
        AddressEntry entry;
        entry.description = pending.description.empty() ? "No description" : pending.description;
        entry.type = pending.type;
        entry.size = std::max<size_t>(1, ValueScanner::TypeSize(pending.type));
        entry.address = pending.address;
        entry.pointer = !pending.offsets.empty();
        entry.module = pending.module;
        entry.baseOffset = pending.module.empty() && !entry.pointer ? pending.address : pending.baseOffset;
        entry.offsets = pending.offsets;
        entry.pointerSize = pending.pointerSize;
        MakeStatic(entry);
        addresses_.push_back(std::move(entry));
    }
    context.pendingAddresses.clear();
    lastAddressRefresh_ = {};
}

// ------------------------------------------------------------------ scripts

// Runs an entry's Auto Assembler script. [ENABLE] registers its symbols
// and allocations so [DISABLE] can find them again.
bool MemoryWorkspace::RunEntryScript(UiContext& context, size_t index, bool enable) {
    if (index >= addresses_.size()) return false;
    auto& entry = addresses_[index];
    if (!entry.script) return false;
    if (!context.mutationAllowed) {
        entry.scriptStatus = "Allow writes to run scripts";
        context.status = entry.scriptStatus;
        return false;
    }
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session) {
        entry.scriptStatus = "Select a process first";
        return false;
    }

    services::AutoAssembleOptions options;
    options.enable = enable;
    options.x64 = session->Target().architecture != target::Architecture::X86;
    services::AutoAssembleResult result;
    std::string error;
    const auto host = MakeAutoAssemblerHost(context);
    if (!services::RunAutoAssembler(entry.assemblerScript, host, options, result, &error)) {
        entry.scriptStatus = error;
        context.status = entry.description + ": " + error;
        return false;
    }

    if (enable) {
        for (const auto& allocation : result.allocations) {
            context.userSymbols->Set(allocation.first, allocation.second);
            entry.scriptSymbols.push_back(allocation.first);
        }
        for (const auto& symbol : result.registered) {
            context.userSymbols->Set(symbol.first, symbol.second);
            entry.scriptSymbols.push_back(symbol.first);
        }
    } else {
        for (const auto& name : result.unregistered) context.userSymbols->Remove(name);
        for (const auto& name : entry.scriptSymbols) context.userSymbols->Remove(name);
        entry.scriptSymbols.clear();
    }
    entry.scriptEnabled = enable;
    entry.scriptStatus = enable ? "enabled" : "disabled";
    if (!result.patches.empty())
        entry.scriptStatus += " (" + std::to_string(result.patches.size()) + " patch(es))";
    context.status = entry.description + ": " + entry.scriptStatus;
    lastAddressRefresh_ = {};
    return true;
}

// ------------------------------------------------------------------ refresh

void MemoryWorkspace::RefreshAddressValues(UiContext& context) {
    if (!context.memory) return;
    const int interval = context.settings ? context.settings->Values().freezeIntervalMs : 100;
    const auto now = std::chrono::steady_clock::now();
    if (lastAddressRefresh_.time_since_epoch().count() != 0 &&
        now - lastAddressRefresh_ < std::chrono::milliseconds(interval)) return;
    lastAddressRefresh_ = now;

    for (auto& entry : addresses_) {
        std::vector<uint8_t> value;
        std::string error;
        if (entry.group || entry.script) continue;
        if (!ResolveEntry(context, entry)) {
            entry.readable = false;
            continue;
        }
        entry.readable = context.memory->Read(entry.address, std::max<size_t>(1, entry.size), value, &error) &&
                         value.size() == std::max<size_t>(1, entry.size);
        if (!entry.readable) continue;
        entry.lastValue = std::move(value);

        if (!entry.freeze || !context.mutationAllowed) continue;
        if (entry.frozenValue.empty()) {
            entry.frozenValue = entry.lastValue;  // frozen before the first read
            continue;
        }
        if (entry.freezeMode != FreezeMode::Always && ValueScanner::IsNumeric(entry.type)) {
            const int order = CompareStored(entry.type, entry.unsignedValue, entry.lastValue, entry.frozenValue);
            if ((entry.freezeMode == FreezeMode::AllowIncrease && order > 0) ||
                (entry.freezeMode == FreezeMode::AllowDecrease && order < 0)) {
                entry.frozenValue = entry.lastValue;
                continue;
            }
        }
        if (entry.lastValue != entry.frozenValue)
            context.memory->Write(entry.address, entry.frozenValue, true, &error);
    }
}

void MemoryWorkspace::PublishHotkeys(UiContext& context) {
    std::map<std::string, std::string> bindings;
    for (const auto& entry : addresses_)
        for (size_t i = 0; i < entry.hotkeys.size(); ++i)
            if (!entry.hotkeys[i].chord.empty()) bindings[HotkeyCommand(entry.uid, i)] = entry.hotkeys[i].chord;
    if (bindings == publishedHotkeys_ && context.entryHotkeys == bindings) return;
    publishedHotkeys_ = bindings;
    context.entryHotkeys = std::move(bindings);
}

void MemoryWorkspace::Tick(UiContext& context) {
    PollScan(context);
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    const std::string targetId = session ? session->Target().id : std::string();
    if (targetId != activeTargetId_) ResetForTarget(context, targetId);
    for (auto& entry : addresses_)
        if (!entry.uid) entry.uid = nextUid_++;
    PublishHotkeys(context);
    if (!session) return;
    RefreshModules(context);
    TakePendingAddresses(context);
    RefreshAddressValues(context);
}

// ------------------------------------------------------------------ commands

void MemoryWorkspace::RunEntryHotkey(UiContext& context, size_t index, const EntryHotkey& hotkey) {
    if (index >= addresses_.size()) return;
    if (!context.mutationAllowed || !context.memory) {
        context.status = "Allow writes to use address list hotkeys";
        return;
    }
    AddressEntry& entry = addresses_[index];
    const size_t end = BlockEnd(index);
    const auto action = hotkey.action;
    const int actionIndex = static_cast<int>(action);

    if (actionIndex <= static_cast<int>(EntryHotkeyAction::Unfreeze)) {
        bool frozen = true;
        for (size_t i = index; i < end; ++i)
            if (!addresses_[i].group) frozen &= addresses_[i].freeze;
        const bool freeze = action == EntryHotkeyAction::Freeze ? true
                          : action == EntryHotkeyAction::Unfreeze ? false : !frozen;
        for (size_t i = index; i < end; ++i) {
            auto& item = addresses_[i];
            if (item.group) continue;
            if (freeze && !item.freeze) {
                if (action == EntryHotkeyAction::ToggleFreezeAllowIncrease) item.freezeMode = FreezeMode::AllowIncrease;
                else if (action == EntryHotkeyAction::ToggleFreezeAllowDecrease) item.freezeMode = FreezeMode::AllowDecrease;
                else if (action == EntryHotkeyAction::ToggleFreeze) item.freezeMode = FreezeMode::Always;
            }
            SetFreeze(item, freeze);
        }
        context.status = entry.description + (freeze ? ": frozen" : ": unfrozen");
        return;
    }

    size_t written = 0;
    std::string error;
    for (size_t i = index; i < end; ++i) {
        auto& item = addresses_[i];
        if (item.group || !item.readable) continue;
        std::vector<uint8_t> bytes;
        const bool ok = action == EntryHotkeyAction::SetValue
            ? ValueScanner::Encode(hotkey.value, item.type, item.hex, item.utf16, bytes, &error)
            : ValueScanner::Adjust(item.lastValue, item.type, item.hex, hotkey.value,
                                   action == EntryHotkeyAction::IncreaseValue, bytes, &error);
        if (!ok || !context.memory->Write(item.address, bytes, true, &error)) continue;
        item.lastValue = bytes;
        if (item.freeze) item.frozenValue = bytes;
        ++written;
    }
    context.status = written ? entry.description + ": " + kHotkeyActionNames[actionIndex] + " " + hotkey.value
                             : entry.description + ": " + (error.empty() ? "nothing to write" : error);
}

bool MemoryWorkspace::HandleAddressCommand(UiContext& context, const std::string& command) {
    if (command.rfind("entry_hotkey ", 0) == 0) {
        std::istringstream words(command.substr(13));
        uint32_t uid = 0;
        size_t which = 0;
        if (!(words >> uid >> which)) return true;
        const int index = EntryIndex(uid);
        if (index < 0 || which >= addresses_[static_cast<size_t>(index)].hotkeys.size()) return true;
        const EntryHotkey hotkey = addresses_[static_cast<size_t>(index)].hotkeys[which];
        RunEntryHotkey(context, static_cast<size_t>(index), hotkey);
        return true;
    }
    if (command == "freeze_toggle_all") {
        if (!context.mutationAllowed) {
            context.status = "Allow writes to freeze values";
            return true;
        }
        const bool freeze = std::any_of(addresses_.begin(), addresses_.end(),
                                        [](const AddressEntry& entry) { return !entry.group && !entry.freeze; });
        for (auto& entry : addresses_) SetFreeze(entry, freeze);
        context.status = freeze ? "Address list frozen" : "Address list unfrozen";
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ editing

bool MemoryWorkspace::AddManualAddress(UiContext& context) {
    if (!context.memory) return false;
    const int editIndex = editEntryUid_ ? EntryIndex(editEntryUid_) : -1;
    if (editEntryUid_ && editIndex < 0) {
        context.status = "The entry no longer exists";
        return false;
    }
    AddressEntry entry;
    const std::string text = Trim(addAddress_);
    if (text.empty()) {
        context.status = "Enter an address, module+offset or an address expression";
        return false;
    }
    std::string module;
    uint64_t offset = 0;
    if (application::ParseCheatAddress(text, module, offset)) {
        entry.module = module;
        entry.baseOffset = offset;
        if (module.empty()) entry.address = offset;
    } else {
        entry.expression = text;  // [[game.exe+10]+20]+8, "game.exe"+10*2...
    }
    if (addPointer_) {
        std::string token;
        for (const char ch : std::string(addOffsets_) + " ") {
            if (std::isxdigit(static_cast<unsigned char>(ch)) || ch == 'x' || ch == 'X') {
                token += ch;
                continue;
            }
            uint64_t value = 0;
            if (!token.empty() && ParseHex(token, value)) entry.offsets.push_back(static_cast<uint32_t>(value));
            token.clear();
        }
        if (entry.offsets.empty()) {
            context.status = "Enter the pointer offsets, in the order they are applied (hex)";
            return false;
        }
        entry.pointer = true;
    }
    entry.pointerSize = TargetPointerSize(context);
    entry.type = static_cast<ScanDataType>(addTypeIndex_);
    entry.size = ValueScanner::TypeSize(entry.type);
    if (!entry.size) {
        const int length = std::clamp(addLength_, 1, 4096);
        entry.size = static_cast<size_t>(length) * (entry.type == ScanDataType::String && addUtf16_ ? 2 : 1);
    }
    entry.utf16 = entry.type == ScanDataType::String && addUtf16_;
    entry.description = *addDescription_ ? std::string(addDescription_) : "No description";
    RefreshModules(context, true);
    ContextSymbols(context, true);
    std::string error;
    if (!ResolveEntry(context, entry, &error)) {
        context.status = error.empty() ? "The address cannot be resolved" : error;
        return false;
    }
    if (editIndex < 0 && !entry.pointer && entry.module.empty() && entry.expression.empty() &&
        HasAddress(entry.address, entry.type)) {
        context.status = "Address is already in the list";
        return false;
    }
    std::vector<uint8_t> value;
    entry.readable = context.memory->Read(entry.address, entry.size, value, &error);
    entry.lastValue = value;
    entry.frozenValue = value;

    if (editIndex >= 0) {
        // Change address: keep the entry's settings, hotkeys and place.
        auto& target = addresses_[static_cast<size_t>(editIndex)];
        const bool typeChanged = target.type != entry.type || target.size != entry.size;
        target.address = entry.address;
        target.module = entry.module;
        target.baseOffset = entry.baseOffset;
        target.offsets = entry.offsets;
        target.pointer = entry.pointer;
        target.pointerSize = entry.pointerSize;
        target.expression = entry.expression;
        target.type = entry.type;
        target.size = entry.size;
        target.utf16 = entry.utf16;
        target.description = entry.description;
        target.readable = entry.readable;
        target.lastValue = entry.lastValue;
        if (typeChanged || !target.freeze) target.frozenValue = entry.frozenValue;
        if (typeChanged) target.freeze = false;
        context.status = "Address changed";
        return true;
    }
    addresses_.push_back(std::move(entry));
    context.status = addresses_.back().readable ? "Address added" : "Address added; it cannot be read right now";
    return true;
}

void MemoryWorkspace::BeginAddressEdit(size_t index) {
    if (index >= addresses_.size() || addresses_[index].group) return;
    const auto& entry = addresses_[index];
    editEntryUid_ = entry.uid;
    std::string address;
    if (!entry.expression.empty()) {
        address = entry.expression;
    } else if (!entry.module.empty()) {
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "+%llX", static_cast<unsigned long long>(entry.baseOffset));
        address = entry.module + buffer;
    } else {
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "%llX",
                      static_cast<unsigned long long>(entry.pointer ? entry.baseOffset : entry.address));
        address = buffer;
    }
    std::snprintf(addAddress_, sizeof(addAddress_), "%s", address.c_str());
    std::snprintf(addDescription_, sizeof(addDescription_), "%s", entry.description.c_str());
    addTypeIndex_ = static_cast<int>(entry.type);
    addUtf16_ = entry.utf16;
    addLength_ = static_cast<int>(entry.type == ScanDataType::String && entry.utf16 ? entry.size / 2 : entry.size);
    addPointer_ = !entry.offsets.empty();
    std::snprintf(addOffsets_, sizeof(addOffsets_), "%s", OffsetsText(entry.offsets).c_str());
    openAddAddress_ = true;
}

void MemoryWorkspace::BeginValueEdit(size_t index) {
    if (index >= addresses_.size() || addresses_[index].group) return;
    editAddressIndex_ = static_cast<int>(index);
    const std::string current = FormatEntry(addresses_[index]);
    std::snprintf(editValue_, sizeof(editValue_), "%s", current.c_str());
    openEditValue_ = true;
}

// Writes the edited entry, and every other selected entry of the same
// type when the edited one is part of the selection.
bool MemoryWorkspace::CommitValueEdit(UiContext& context) {
    if (!context.memory || !context.mutationAllowed || editAddressIndex_ < 0 ||
        editAddressIndex_ >= static_cast<int>(addresses_.size())) {
        context.status = "Allow writes before changing a value";
        return false;
    }
    auto& edited = addresses_[static_cast<size_t>(editAddressIndex_)];
    size_t written = 0;
    for (size_t i = 0; i < addresses_.size(); ++i) {
        auto& entry = addresses_[i];
        const bool target = i == static_cast<size_t>(editAddressIndex_) ||
                            (edited.selected && entry.selected && !entry.group && entry.type == edited.type &&
                             entry.customType == edited.customType);
        if (!target) continue;
        std::vector<uint8_t> bytes;
        std::string error;
        if (const auto* custom = EntryCustomType(entry)) {
            // A custom type may cover only part of the bytes, so what the
            // target holds is the starting point.
            bytes = entry.lastValue;
            bytes.resize(custom->size, 0);
            char* end = nullptr;
            const double value = std::strtod(editValue_, &end);
            if (!end || end == editValue_ || *end != '\0') {
                context.status = "\"" + std::string(editValue_) + "\" is not a number";
                return false;
            }
            if (!services::WriteCustomType(*custom, value, bytes.data(), bytes.size(), &error)) {
                context.status = error;
                return false;
            }
        } else if (!ValueScanner::Encode(editValue_, entry.type, entry.hex, entry.utf16, bytes, &error)) {
            context.status = error;
            return false;
        }
        if (!context.memory->Write(entry.address, bytes, true, &error)) {
            context.status = "Value write failed: " + error;
            return false;
        }
        if (IsText(entry.type)) entry.size = bytes.size();
        entry.lastValue = bytes;
        entry.frozenValue = bytes;
        ++written;
    }
    context.status = written > 1 ? "Value written to " + std::to_string(written) + " entries" : "Value written";
    return true;
}

void MemoryWorkspace::SaveEntryToProject(UiContext& context, const AddressEntry& entry) {
    const std::string type = PersistentType(entry.type, entry.unsignedValue);
    if (!context.projectModel || !context.mutationAllowed) {
        context.status = "Allow writes to save addresses to the project";
        return;
    }
    if (type.empty()) {
        context.status = "Only numeric entries can be saved to Addresses";
        return;
    }
    bool isStatic = false;
    const std::string address = AddressText(entry.address, isStatic);
    std::string error;
    if (!context.projectModel->SetAddress(entry.description, isStatic ? address : Hex(entry.address), type,
                                          "From the Memory address list", context.mutationAllowed, &error)) {
        context.status = "Save to Addresses failed: " + error;
        return;
    }
    context.status = "Saved to Addresses";
}

// ------------------------------------------------------------------ drawing

void MemoryWorkspace::DrawEntryMenu(UiContext& context, size_t index, size_t groupEnd, bool& remove) {
    auto& entry = addresses_[index];
    const bool writes = context.mutationAllowed;
    if (!entry.selected) {
        for (auto& other : addresses_) other.selected = false;
        entry.selected = true;
        selectionAnchor_ = index;
    }

    if (entry.script) {
        if (ImGui::MenuItem("Edit script...")) {
            scriptEntryUid_ = entry.uid;
            std::snprintf(scriptText_.data(), scriptText_.size(), "%s", entry.assemblerScript.c_str());
            openScript_ = true;
        }
        ImGui::BeginDisabled(!writes);
        if (ImGui::MenuItem(entry.scriptEnabled ? "Disable" : "Enable")) RunEntryScript(context, index, !entry.scriptEnabled);
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Check script")) {
            services::AutoAssembleOptions options;
            options.enable = true;
            options.dryRun = true;
            const auto session = context.sessions ? context.sessions->Active() : nullptr;
            options.x64 = !session || session->Target().architecture != target::Architecture::X86;
            services::AutoAssembleResult result;
            std::string error;
            const auto host = MakeAutoAssemblerHost(context);
            const bool ok = services::RunAutoAssembler(entry.assemblerScript, host, options, result, &error);
            entry.scriptStatus = ok ? "script looks valid" : error;
            context.status = entry.description + ": " + entry.scriptStatus;
        }
    } else if (entry.group) {
        if (ImGui::MenuItem(entry.collapsed ? "Expand" : "Collapse")) entry.collapsed = !entry.collapsed;
        ImGui::BeginDisabled(!writes);
        if (ImGui::MenuItem("Freeze all entries")) for (size_t i = index + 1; i < groupEnd; ++i) SetFreeze(addresses_[i], true);
        if (ImGui::MenuItem("Unfreeze all entries")) for (size_t i = index + 1; i < groupEnd; ++i) SetFreeze(addresses_[i], false);
        ImGui::EndDisabled();
    } else {
        ImGui::BeginDisabled(!writes);
        if (ImGui::MenuItem("Change value...", "Enter")) BeginValueEdit(index);
        if (ImGui::MenuItem(entry.freeze ? "Unfreeze" : "Freeze", "Space")) SetFreeze(entry, !entry.freeze);
        ImGui::EndDisabled();
        if (ValueScanner::IsNumeric(entry.type) && ImGui::BeginMenu("Freeze mode")) {
            if (ImGui::MenuItem("Always write the value", nullptr, entry.freezeMode == FreezeMode::Always))
                entry.freezeMode = FreezeMode::Always;
            if (ImGui::MenuItem("Allow increases", nullptr, entry.freezeMode == FreezeMode::AllowIncrease))
                entry.freezeMode = FreezeMode::AllowIncrease;
            if (ImGui::MenuItem("Allow decreases", nullptr, entry.freezeMode == FreezeMode::AllowDecrease))
                entry.freezeMode = FreezeMode::AllowDecrease;
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Change address...")) BeginAddressEdit(index);
        if (ValueScanner::IsInteger(entry.type)) {
            ImGui::MenuItem("Show as hexadecimal", nullptr, &entry.hex);
            ImGui::MenuItem("Show as unsigned", nullptr, &entry.unsignedValue);
        }
        if (ImGui::BeginMenu("Change type")) {
            for (const auto type : kTypes) {
                if (type == ScanDataType::AllNumeric) continue;
                const bool selected = entry.customType.empty() && entry.type == type;
                if (ImGui::MenuItem(ValueScanner::TypeName(type), nullptr, selected) && !selected) {
                    const size_t fixed = ValueScanner::TypeSize(type);
                    entry.size = fixed ? fixed : std::max<size_t>(entry.size, 8);
                    entry.type = type;
                    entry.customType.clear();
                    entry.freeze = false;
                    entry.lastValue.clear();
                    lastAddressRefresh_ = {};
                }
            }
            // User-defined types read the same bytes their own way.
            if (customTypes_ && !customTypes_->Types().empty()) {
                ImGui::Separator();
                for (const auto& custom : customTypes_->Types()) {
                    const bool selected = entry.customType == custom.name;
                    if (ImGui::MenuItem(custom.name.c_str(), nullptr, selected) && !selected) {
                        entry.customType = custom.name;
                        entry.size = custom.size;
                        entry.freeze = false;
                        entry.lastValue.clear();
                        lastAddressRefresh_ = {};
                    }
                }
            }
            ImGui::EndMenu();
        }
    }
    if (ImGui::BeginMenu("Change color")) {
        if (ImGui::MenuItem("Default", nullptr, entry.color < 0)) entry.color = -1;
        for (const auto& color : kColors) {
            ImGui::PushStyleColor(ImGuiCol_Text, ColorFromRgb(color.rgb));
            if (ImGui::MenuItem(color.name, nullptr, entry.color == color.rgb))
                for (auto& item : addresses_) if (item.selected) item.color = color.rgb;
            ImGui::PopStyleColor();
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Hotkeys...")) {
        hotkeyEntryUid_ = entry.uid;
        openHotkeys_ = true;
    }
    if (!entry.group && ImGui::MenuItem("Dropdown list...")) {
        dropDownEntryUid_ = entry.uid;
        std::string text;
        for (const auto& item : entry.dropDown) text += item.value + ":" + item.label + "\n";
        std::snprintf(dropDownText_, sizeof(dropDownText_), "%s", text.c_str());
        dropDownOnlyEdit_ = entry.dropDownOnly;
        openDropDown_ = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Group selected entries")) GroupSelected(context);
    if (ImGui::MenuItem("Copy entries", "Ctrl+C")) CopyEntries(true);
    if (ImGui::MenuItem("Paste entries", "Ctrl+V")) PasteEntries(context);
    if (!entry.group) {
        ImGui::BeginDisabled(!writes || !context.projectModel || PersistentType(entry.type, entry.unsignedValue).empty());
        if (ImGui::MenuItem("Save to Addresses")) SaveEntryToProject(context, entry);
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(Hex(entry.address).c_str());
        bool isStatic = false;
        const std::string moduleText = AddressText(entry.address, isStatic);
        if (isStatic && ImGui::MenuItem("Copy module+offset")) ImGui::SetClipboardText(moduleText.c_str());
        if (entry.pointer && ImGui::MenuItem("Copy pointer path")) ImGui::SetClipboardText(PointerText(entry).c_str());
        ImGui::Separator();
        AddressContextOptions options;
        options.label = entry.description;
        options.valueType = PersistentType(entry.type, entry.unsignedValue);
        if (options.valueType.empty()) options.valueType = "i32";
        options.valueSize = static_cast<int>(entry.size);
        DrawAddressContextActions(context, entry.address, options);
    }
    ImGui::Separator();
    if (entry.group) {
        if (ImGui::MenuItem("Remove the group header")) remove = true;
        if (ImGui::MenuItem("Remove the group and its entries")) {
            addresses_.erase(addresses_.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                             addresses_.begin() + static_cast<std::ptrdiff_t>(groupEnd));
            remove = true;
        }
    } else if (ImGui::MenuItem("Remove", "Delete")) {
        remove = true;
    }
}

void MemoryWorkspace::DrawAddressList(UiContext& context) {
    // The user-defined types can be edited from Memory tools while the list
    // is showing them, so their widths are picked up again every frame.
    customTypes_ = context.customTypes;
    if (customTypes_) {
        for (auto& entry : addresses_) {
            if (entry.customType.empty()) continue;
            const auto* custom = customTypes_->Find(entry.customType);
            if (custom && entry.size != custom->size) {
                entry.size = custom->size;
                entry.lastValue.clear();
                entry.freeze = false;
                lastAddressRefresh_ = {};
            }
        }
    }

    ImGui::BeginChild("AddressList", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Address list");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", addresses_.size());
    FlowSameLine(ButtonWidth("+ Add address"));
    if (ImGui::Button("+ Add address")) {
        editEntryUid_ = 0;
        addAddress_[0] = '\0';
        addDescription_[0] = '\0';
        addTypeIndex_ = static_cast<int>(ScanDataType::Int32);
        addLength_ = 16;
        addUtf16_ = false;
        addPointer_ = false;
        addOffsets_[0] = '\0';
        openAddAddress_ = true;
    }
    FlowSameLine(ButtonWidth("+ Script"));
    if (ImGui::Button("+ Script")) {
        AddressEntry entry;
        entry.script = true;
        entry.description = "New script";
        entry.uid = nextUid_++;
        entry.assemblerScript =
            "[ENABLE]\n"
            "// aobscanmodule(INJECT,game.exe,89 83 00 01 00 00)\n"
            "// alloc(newmem,$1000,INJECT)\n"
            "// label(code)\n"
            "// label(return)\n"
            "//\n"
            "// newmem:\n"
            "// code:\n"
            "//   mov [rbx+00000100],eax\n"
            "//   jmp return\n"
            "//\n"
            "// INJECT:\n"
            "//   jmp newmem\n"
            "// return:\n"
            "// registersymbol(INJECT)\n"
            "\n"
            "[DISABLE]\n"
            "// INJECT:\n"
            "//   db 89 83 00 01 00 00\n"
            "// unregistersymbol(INJECT)\n"
            "// dealloc(newmem)\n";
        addresses_.push_back(std::move(entry));
        scriptEntryUid_ = addresses_.back().uid;
        std::snprintf(scriptText_.data(), scriptText_.size(), "%s", addresses_.back().assemblerScript.c_str());
        openScript_ = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add an Auto Assembler script entry");
    FlowSameLine(ButtonWidth("Group"));
    if (ImGui::Button("Group")) GroupSelected(context);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Put the selected entries in a group header, or add an empty one");
    FlowSameLine(ButtonWidth("Open table..."));
    if (ImGui::Button("Open table...")) OpenTable(context);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add the entries of a Cheat Engine table (.CT)");
    if (!addresses_.empty()) {
        FlowSameLine(ButtonWidth("Save table..."));
        if (ImGui::Button("Save table...")) SaveTable(context);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the list as a Cheat Engine table (.CT)");
        FlowSameLine(ButtonWidth("Clear"));
        if (ImGui::Button("Clear")) {
            addresses_.clear();
            tableLuaScript_.clear();
            context.status = "Address list cleared";
        }
    }
    ImGui::Separator();

    if (addresses_.empty()) {
        ImGui::Dummy(ImVec2(0, Px(12)));
        HintText("Double-click scan results to collect them here, then freeze or edit their values. "
                 "Entries copied in Cheat Engine paste here with Ctrl+V, and the other way round.");
    }

    size_t moveFrom = SIZE_MAX;
    size_t moveBefore = SIZE_MAX;
    bool moveInto = false;
    if (!addresses_.empty() &&
        BeginDataTable("AddressTable", 5,
                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_ScrollY,
                       ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Active", ImGuiTableColumnFlags_WidthFixed, Px(56.0f));
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.26f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.14f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableHeadersRow();

        int hideDeeperThan = INT_MAX;  // inside a collapsed group
        for (size_t i = 0; i < addresses_.size();) {
            auto& entry = addresses_[i];
            if (entry.depth > hideDeeperThan) {
                ++i;
                continue;
            }
            hideDeeperThan = entry.group && entry.collapsed ? entry.depth : INT_MAX;

            bool remove = false;
            ImGui::PushID(static_cast<int>(entry.uid));
            ImGui::TableNextRow();
            const size_t groupEnd = BlockEnd(i);

            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (entry.script) {
                bool on = entry.scriptEnabled;
                if (ImGui::Checkbox("##Script", &on)) RunEntryScript(context, i, on);
            } else if (entry.group) {
                bool all = groupEnd > i + 1;
                bool any = false;
                for (size_t child = i + 1; child < groupEnd; ++child) {
                    if (addresses_[child].group) continue;
                    all &= addresses_[child].freeze;
                    any = true;
                }
                all &= any;
                if (ImGui::Checkbox("##Freeze", &all))
                    for (size_t child = i + 1; child < groupEnd; ++child) SetFreeze(addresses_[child], all);
            } else {
                bool freeze = entry.freeze;
                if (ImGui::Checkbox("##Freeze", &freeze)) SetFreeze(entry, freeze);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                std::string tip = !context.mutationAllowed ? "Allow writes to freeze values or run scripts"
                                  : entry.script ? "Run the script's [ENABLE] / [DISABLE] section"
                                                 : "Freeze: keep writing this value (Space)";
                for (const auto& hotkey : entry.hotkeys)
                    tip += "\n" + hotkey.chord + ": " + kHotkeyActionNames[static_cast<int>(hotkey.action)] +
                           (ActionNeedsValue(static_cast<int>(hotkey.action)) ? " " + hotkey.value : std::string());
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if (entry.freeze && entry.freezeMode != FreezeMode::Always) {
                ImGui::SameLine(0, Px(2));
                ImGui::TextDisabled(entry.freezeMode == FreezeMode::AllowIncrease ? "+" : "-");
            }
            if (!entry.hotkeys.empty()) {
                ImGui::SameLine(0, Px(2));
                ImGui::TextDisabled("K");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%zu hotkey(s): right-click > Hotkeys...", entry.hotkeys.size());
            }

            ImGui::TableSetColumnIndex(1);
            if (entry.depth > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(14) * static_cast<float>(entry.depth));
            if (entry.group) {
                if (ImGui::ArrowButton("##Collapse", entry.collapsed ? ImGuiDir_Right : ImGuiDir_Down))
                    entry.collapsed = !entry.collapsed;
                ImGui::SameLine(0, Px(4));
            }
            std::array<char, 160> description{};
            std::snprintf(description.data(), description.size(), "%s", entry.description.c_str());
            ImGui::SetNextItemWidth(-1);
            const bool colored = entry.color >= 0 || entry.group;
            if (colored) ImGui::PushStyleColor(ImGuiCol_Text, entry.color >= 0 ? ColorFromRgb(entry.color) : StaticAddressColor());
            if (ImGui::InputText("##Description", description.data(), description.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
                entry.description = description.data();
            if (ImGui::IsItemDeactivatedAfterEdit()) entry.description = description.data();
            if (colored) ImGui::PopStyleColor();
            ImGui::OpenPopupOnItemClick("EntryMenu");

            ImGui::TableSetColumnIndex(2);
            bool isStatic = !entry.module.empty() && !entry.pointer && entry.expression.empty();
            char staticText[64] = {};
            if (isStatic) std::snprintf(staticText, sizeof(staticText), "+%llX", static_cast<unsigned long long>(entry.baseOffset));
            const std::string addressText = entry.group ? std::string()
                : entry.script ? std::string("Auto Assembler script")
                : entry.pointer ? "P->" + (entry.readable ? Hex(entry.address) : std::string("????????"))
                : !entry.expression.empty() ? entry.expression
                : isStatic ? entry.module + staticText
                : AddressText(entry.address, isStatic);
            const bool green = !entry.script && (isStatic || entry.pointer || !entry.expression.empty());
            if (green) ImGui::PushStyleColor(ImGuiCol_Text, StaticAddressColor());
            {
                MonoFont mono;
                if (ImGui::Selectable((addressText + "##Address").c_str(), entry.selected,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    SelectEntry(i);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (entry.script) {
                            scriptEntryUid_ = entry.uid;
                            std::snprintf(scriptText_.data(), scriptText_.size(), "%s", entry.assemblerScript.c_str());
                            openScript_ = true;
                        } else if (!entry.group) {
                            BeginAddressEdit(i);
                        }
                    }
                }
            }
            if (green) ImGui::PopStyleColor();
            if (ImGui::BeginDragDropSource()) {
                const uint32_t uid = entry.uid;
                ImGui::SetDragDropPayload("CORTEX_ADDRESS_ENTRY", &uid, sizeof(uid));
                ImGui::TextUnformatted(entry.description.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CORTEX_ADDRESS_ENTRY")) {
                    uint32_t uid = 0;
                    std::memcpy(&uid, payload->Data, sizeof(uid));
                    const int from = EntryIndex(uid);
                    if (from >= 0) {
                        moveFrom = static_cast<size_t>(from);
                        // The lower half of a group header drops into the group.
                        const float middle = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
                        moveInto = entry.group && ImGui::GetMousePos().y > middle;
                        moveBefore = moveInto ? i + 1 : i;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (entry.script && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("Double-click to read or edit the script.");
            } else if (!entry.group && !entry.script && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                const std::string tip = entry.pointer || !entry.expression.empty()
                    ? PointerText(entry) + (entry.readable ? " = " + Hex(entry.address) : std::string(" (cannot be resolved)"))
                    : Hex(entry.address);
                ImGui::SetTooltip("%s\nDouble-click to change the address; drag to reorder.", tip.c_str());
            }
            ImGui::OpenPopupOnItemClick("EntryMenu");

            ImGui::TableSetColumnIndex(3);
            const std::string typeText = entry.group ? std::string("Group")
                : entry.script ? std::string("Script")
                : !entry.customType.empty() ? entry.customType
                : std::string(ValueScanner::TypeName(entry.type)) +
                      (entry.type == ScanDataType::String && entry.utf16 ? " (UTF-16)" : "");
            ImGui::TextUnformatted(typeText.c_str());
            ImGui::OpenPopupOnItemClick("EntryMenu");

            ImGui::TableSetColumnIndex(4);
            const std::string value = entry.group ? std::string()
                : entry.script ? entry.scriptStatus
                : DisplayValue(entry);
            const bool changed = entry.script ? (!entry.scriptEnabled && !entry.scriptStatus.empty() &&
                                                 entry.scriptStatus != "disabled")
                                              : (entry.readable && entry.freeze && entry.lastValue != entry.frozenValue);
            if (changed) ImGui::PushStyleColor(ImGuiCol_Text, ChangedValueColor());
            if (ImGui::Selectable((value + "##Value").c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !entry.script) {
                if (context.mutationAllowed) BeginValueEdit(i);
                else context.status = "Allow writes to edit a value";
            }
            if (changed) ImGui::PopStyleColor();
            ImGui::OpenPopupOnItemClick("EntryMenu");

            if (ImGui::BeginPopup("EntryMenu")) {
                DrawEntryMenu(context, i, groupEnd, remove);
                ImGui::EndPopup();
            }

            ImGui::PopID();
            if (remove) addresses_.erase(addresses_.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
        ImGui::EndTable();
    }

    if (moveFrom != SIZE_MAX && moveBefore != SIZE_MAX && moveBefore <= addresses_.size()) {
        int depth = -1;
        if (moveInto) {
            auto& header = addresses_[moveBefore - 1];
            header.collapsed = false;
            depth = header.depth + 1;
        }
        MoveBlock(moveFrom, moveBefore, depth);
    }

    // Right-click on the empty part of the list.
    if (ImGui::BeginPopupContextWindow("ListMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Add address...")) {
            editEntryUid_ = 0;
            addAddress_[0] = '\0';
            addDescription_[0] = '\0';
            addPointer_ = false;
            addOffsets_[0] = '\0';
            openAddAddress_ = true;
        }
        if (ImGui::MenuItem("Add group header")) {
            for (auto& entry : addresses_) entry.selected = false;
            GroupSelected(context);
        }
        if (ImGui::MenuItem("Paste entries", "Ctrl+V")) PasteEntries(context);
        ImGui::BeginDisabled(addresses_.empty());
        if (ImGui::MenuItem("Copy all entries")) CopyEntries(false);
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    // Keyboard actions on the list.
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !io.WantTextInput) {
        const bool anySelected = std::any_of(addresses_.begin(), addresses_.end(),
                                             [](const AddressEntry& entry) { return entry.selected; });
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            PasteEntries(context);
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            for (auto& entry : addresses_) entry.selected = true;
        } else if (anySelected && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            CopyEntries(true);
            context.status = "Entries copied in the Cheat Engine format";
        } else if (anySelected && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            std::vector<AddressEntry> kept;
            for (size_t i = 0; i < addresses_.size();) {
                // A selected group header takes its entries with it.
                const size_t end = addresses_[i].selected ? BlockEnd(i) : i + 1;
                if (!addresses_[i].selected) kept.push_back(std::move(addresses_[i]));
                i = end;
            }
            addresses_ = std::move(kept);
            context.status = "Selected entries removed";
        } else if (anySelected && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (!context.mutationAllowed) {
                context.status = "Allow writes to freeze values";
            } else {
                for (auto& entry : addresses_)
                    if (entry.selected) SetFreeze(entry, !entry.freeze);
            }
        } else if (anySelected && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            for (size_t i = 0; i < addresses_.size(); ++i) {
                if (!addresses_[i].selected || addresses_[i].group) continue;
                if (context.mutationAllowed) BeginValueEdit(i);
                else context.status = "Allow writes to edit a value";
                break;
            }
        }
    }
    ImGui::EndChild();
}

void MemoryWorkspace::DrawHotkeyDialog(UiContext& context) {
    if (openHotkeys_) {
        ImGui::OpenPopup("Entry hotkeys");
        openHotkeys_ = false;
        capturingEntryHotkey_ = false;
        newHotkeyChord_.clear();
        newHotkeyAction_ = 0;
        newHotkeyValue_[0] = '\0';
    }
    ImGui::SetNextWindowSize(ImVec2(Px(600), 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Entry hotkeys", nullptr, ImGuiWindowFlags_NoResize)) return;
    const int index = EntryIndex(hotkeyEntryUid_);
    if (index < 0) {
        ImGui::TextDisabled("The entry no longer exists.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    auto& entry = addresses_[static_cast<size_t>(index)];
    ImGui::Text("Hotkeys for \"%s\"", entry.description.c_str());
    HintText("They work system-wide, while the game has the focus. Writes must be allowed.");
    ImGui::Spacing();

    if (!entry.hotkeys.empty() &&
        BeginDataTable("EntryHotkeys", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.2f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ButtonWidth("Remove"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < entry.hotkeys.size();) {
            const auto& hotkey = entry.hotkeys[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool failed = std::find(context.hotkeyFailures.begin(), context.hotkeyFailures.end(),
                                          HotkeyCommand(entry.uid, i)) != context.hotkeyFailures.end();
            if (failed) ImGui::PushStyleColor(ImGuiCol_Text, WarningTextColor());
            ImGui::TextUnformatted(hotkey.chord.c_str());
            if (failed) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Another program already uses this shortcut");
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(kHotkeyActionNames[static_cast<int>(hotkey.action)]);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(hotkey.value.c_str());
            ImGui::TableSetColumnIndex(3);
            const bool removeRow = ImGui::SmallButton("Remove");
            ImGui::PopID();
            if (removeRow) entry.hotkeys.erase(entry.hotkeys.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("New hotkey");
    if (capturingEntryHotkey_) {
        std::string chord;
        std::string message;
        switch (CaptureHotkeyChord(chord, message)) {
            case HotkeyCaptureResult::Captured: newHotkeyChord_ = chord; capturingEntryHotkey_ = false; break;
            case HotkeyCaptureResult::Cleared: newHotkeyChord_.clear(); capturingEntryHotkey_ = false; break;
            case HotkeyCaptureResult::Cancelled: capturingEntryHotkey_ = false; break;
            case HotkeyCaptureResult::Rejected: context.status = message; break;
            case HotkeyCaptureResult::Waiting: break;
        }
    }
    const std::string keysLabel = capturingEntryHotkey_ ? "Press the keys..."
                                : newHotkeyChord_.empty() ? "Click, then press the keys" : newHotkeyChord_;
    if (ImGui::Button((keysLabel + "###CaptureKeys").c_str(), ImVec2(Px(190), 0))) capturingEntryHotkey_ = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(210));
    ImGui::Combo("##HotkeyAction", &newHotkeyAction_, kHotkeyActionNames, kHotkeyActionCount);
    const bool numeric = ValueScanner::IsNumeric(entry.type) || entry.group;
    if (ActionNeedsValue(newHotkeyAction_)) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##HotkeyValue", "value", newHotkeyValue_, sizeof(newHotkeyValue_));
    }
    const bool adjust = newHotkeyAction_ == static_cast<int>(EntryHotkeyAction::IncreaseValue) ||
                        newHotkeyAction_ == static_cast<int>(EntryHotkeyAction::DecreaseValue);
    const bool valid = !newHotkeyChord_.empty() && (!ActionNeedsValue(newHotkeyAction_) || *newHotkeyValue_) &&
                       (!adjust || numeric);
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Add hotkey")) {
        EntryHotkey hotkey;
        hotkey.chord = newHotkeyChord_;
        hotkey.action = static_cast<EntryHotkeyAction>(newHotkeyAction_);
        if (ActionNeedsValue(newHotkeyAction_)) hotkey.value = Trim(newHotkeyValue_);
        entry.hotkeys.push_back(std::move(hotkey));
        newHotkeyChord_.clear();
        newHotkeyValue_[0] = '\0';
    }
    ImGui::EndDisabled();
    if (adjust && !numeric) {
        ImGui::SameLine();
        ImGui::TextDisabled("Only numeric entries can be increased or decreased");
    }
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(Px(100), Px(34)))) {
        capturingEntryHotkey_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MemoryWorkspace::DrawScriptDialog(UiContext& context) {
    if (openScript_) {
        ImGui::OpenPopup("Auto Assembler script");
        openScript_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Px(720), Px(560)), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Auto Assembler script", nullptr, ImGuiWindowFlags_NoResize)) return;
    const int index = EntryIndex(scriptEntryUid_);
    if (index < 0) {
        ImGui::TextDisabled("The entry no longer exists.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    auto& entry = addresses_[static_cast<size_t>(index)];
    ImGui::Text("Script for \"%s\"", entry.description.c_str());
    HintText("Cheat Engine syntax: [ENABLE] and [DISABLE] sections, aobscanmodule, alloc, label, "
             "registersymbol, dealloc. Numbers are hexadecimal; $1000 and #4096 also work.");
    {
        MonoFont mono;
        ImGui::InputTextMultiline("##ScriptText", scriptText_.data(), scriptText_.size(),
                                  ImVec2(-1, Px(390)), ImGuiInputTextFlags_AllowTabInput);
    }
    if (!entry.scriptStatus.empty()) {
        const bool bad = entry.scriptStatus != "enabled" && entry.scriptStatus != "disabled" &&
                         entry.scriptStatus.rfind("enabled", 0) != 0 && entry.scriptStatus != "script looks valid";
        ImGui::PushStyleColor(ImGuiCol_Text, bad ? ChangedValueColor() : StaticAddressColor());
        ImGui::TextWrapped("%s", entry.scriptStatus.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    if (ImGui::Button("Save", ImVec2(Px(100), Px(32)))) {
        entry.assemblerScript = scriptText_.data();
        context.status = "Script saved";
    }
    FlowSameLine(Px(100));
    if (ImGui::Button("Check", ImVec2(Px(100), Px(32)))) {
        entry.assemblerScript = scriptText_.data();
        services::AutoAssembleOptions options;
        options.enable = true;
        options.dryRun = true;
        const auto session = context.sessions ? context.sessions->Active() : nullptr;
        options.x64 = !session || session->Target().architecture != target::Architecture::X86;
        services::AutoAssembleResult result;
        std::string error;
        const auto host = MakeAutoAssemblerHost(context);
        entry.scriptStatus = services::RunAutoAssembler(entry.assemblerScript, host, options, result, &error)
                                 ? "script looks valid"
                                 : error;
    }
    FlowSameLine(Px(110));
    ImGui::BeginDisabled(!context.mutationAllowed);
    if (ImGui::Button(entry.scriptEnabled ? "Disable" : "Enable", ImVec2(Px(110), Px(32)))) {
        entry.assemblerScript = scriptText_.data();
        RunEntryScript(context, static_cast<size_t>(index), !entry.scriptEnabled);
    }
    ImGui::EndDisabled();
    FlowSameLine(Px(100));
    if (ImGui::Button("Close", ImVec2(Px(100), Px(32)))) {
        entry.assemblerScript = scriptText_.data();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MemoryWorkspace::DrawDropDownDialog(UiContext& context) {
    if (openDropDown_) {
        ImGui::OpenPopup("Dropdown list");
        openDropDown_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Px(460), 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Dropdown list", nullptr, ImGuiWindowFlags_NoResize)) return;
    const int index = EntryIndex(dropDownEntryUid_);
    if (index < 0) {
        ImGui::TextDisabled("The entry no longer exists.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    auto& entry = addresses_[static_cast<size_t>(index)];
    HintText("One value:label per line, like 0:Off and 1:On. The list names the current value and offers the "
             "values in Change value.");
    ImGui::InputTextMultiline("##DropDownText", dropDownText_, sizeof(dropDownText_), ImVec2(-1, Px(180)));
    ImGui::Checkbox("Show only the label", &dropDownOnlyEdit_);
    ImGui::Spacing();
    if (ImGui::Button("Save", ImVec2(Px(100), Px(34)))) {
        entry.dropDown.clear();
        std::istringstream lines(dropDownText_);
        std::string line;
        while (std::getline(lines, line)) {
            const auto colon = line.find(':');
            if (colon == std::string::npos || Trim(line.substr(0, colon)).empty()) continue;
            entry.dropDown.push_back({Trim(line.substr(0, colon)), Trim(line.substr(colon + 1))});
        }
        entry.dropDownOnly = dropDownOnlyEdit_;
        context.status = entry.dropDown.empty() ? "Dropdown list removed"
                                                : "Dropdown list set: " + std::to_string(entry.dropDown.size()) + " value(s)";
        ImGui::CloseCurrentPopup();
    }
    FlowSameLine(Px(100));
    if (ImGui::Button("Cancel", ImVec2(Px(100), Px(34)))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void MemoryWorkspace::DrawDialogs(UiContext& context) {
    if (openAddAddress_) {
        ImGui::OpenPopup("###AddressDialog");
        openAddAddress_ = false;
    }
    // A fixed width with an automatic height: fields sized -1 would shrink
    // an auto-resized window.
    ImGui::SetNextWindowSize(ImVec2(Px(460), 0), ImGuiCond_Always);
    const char* addressTitle = editEntryUid_ ? "Change address###AddressDialog" : "Add address###AddressDialog";
    if (ImGui::BeginPopupModal(addressTitle, nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextDisabled("Address");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool submit = ImGui::InputTextWithHint("##ManualAddress", "7FF6A1B20010, game.exe+1A2B or [[game.exe+10]+20]+8",
                                               addAddress_, sizeof(addAddress_), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cheat Engine address expressions work: module names, [pointer] reads, + - *");
        ImGui::TextDisabled("Description");
        ImGui::SetNextItemWidth(-1);
        submit |= ImGui::InputTextWithHint("##ManualDescription", "Health", addDescription_, sizeof(addDescription_),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Type");
        ImGui::SetNextItemWidth(-1);
        TypeCombo("##ManualType", &addTypeIndex_, false);
        ImGui::Checkbox("Pointer", &addPointer_);
        if (addPointer_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##ManualOffsets", "offsets in hex, base first: 10 48", addOffsets_,
                                     sizeof(addOffsets_));
        }
        const auto type = static_cast<ScanDataType>(addTypeIndex_);
        if (IsText(type)) {
            ImGui::TextDisabled(type == ScanDataType::String ? "Length (characters)" : "Length (bytes)");
            ImGui::SetNextItemWidth(Px(140));
            if (ImGui::InputInt("##ManualLength", &addLength_)) addLength_ = std::clamp(addLength_, 1, 4096);
            if (type == ScanDataType::String) {
                ImGui::SameLine();
                ImGui::Checkbox("UTF-16", &addUtf16_);
            }
        }
        ImGui::Spacing();
        if ((ImGui::Button(editEntryUid_ ? "Change" : "Add", ImVec2(Px(120), Px(34))) || submit) &&
            AddManualAddress(context))
            ImGui::CloseCurrentPopup();
        FlowSameLine(Px(100));
        if (ImGui::Button("Cancel", ImVec2(Px(100), Px(34)))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (openEditValue_) {
        ImGui::OpenPopup("Change value");
        openEditValue_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Px(430), 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Change value", nullptr, ImGuiWindowFlags_NoResize)) {
        if (editAddressIndex_ >= 0 && editAddressIndex_ < static_cast<int>(addresses_.size())) {
            const auto& entry = addresses_[static_cast<size_t>(editAddressIndex_)];
            MonoText("%s  (%s%s)", Hex(entry.address).c_str(), ValueScanner::TypeName(entry.type),
                     entry.hex ? ", hexadecimal" : "");
            size_t others = 0;
            if (entry.selected)
                for (const auto& other : addresses_)
                    others += &other != &entry && other.selected && !other.group && other.type == entry.type;
            if (others) ImGui::TextDisabled("Also writes the %zu other selected %s entr%s", others,
                                            ValueScanner::TypeName(entry.type), others == 1 ? "y" : "ies");
            if (!entry.dropDown.empty()) {
                ImGui::TextDisabled("Values");
                for (size_t i = 0; i < entry.dropDown.size(); ++i) {
                    const auto& item = entry.dropDown[i];
                    const std::string label = item.value + " : " + item.label + "##Choice" + std::to_string(i);
                    if (i) FlowSameLine(ButtonWidth(label.c_str()));
                    if (ImGui::Button(label.c_str())) std::snprintf(editValue_, sizeof(editValue_), "%s", item.value.c_str());
                }
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##EditedValue", editValue_, sizeof(editValue_),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Spacing();
            ImGui::BeginDisabled(!context.mutationAllowed);
            if ((ImGui::Button("Write value", ImVec2(Px(130), Px(34))) || (enter && context.mutationAllowed)) &&
                CommitValueEdit(context))
                ImGui::CloseCurrentPopup();
            ImGui::EndDisabled();
            FlowSameLine(Px(100));
            if (ImGui::Button("Cancel", ImVec2(Px(100), Px(34)))) ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextDisabled("Address no longer exists.");
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    DrawHotkeyDialog(context);
    DrawScriptDialog(context);
    DrawDropDownDialog(context);

    if (openLuaPrompt_) {
        ImGui::OpenPopup("Cheat table script");
        openLuaPrompt_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Px(560), 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Cheat table script", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextUnformatted("This table has a Lua script.");
        HintText("Cortex does not run it on its own. Open it in the Lua engine to read it first, then Execute. "
                 "Scripts that use Cheat Engine functions Cortex lacks stop with an error.");
        ImGui::BeginChild("ScriptPreview", ImVec2(-1, Px(180)), ImGuiChildFlags_Borders);
        {
            MonoFont mono;
            ImGui::TextUnformatted(tableLuaScript_.c_str());
        }
        ImGui::EndChild();
        if (ImGui::Button("Open in the Lua engine", ImVec2(0, Px(34)))) {
            context.luaScriptRequest = tableLuaScript_;
            context.requestWorkspace = "lua";
            ImGui::CloseCurrentPopup();
        }
        FlowSameLine(Px(100));
        if (ImGui::Button("Not now", ImVec2(Px(100), Px(34)))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace cortex::ui
