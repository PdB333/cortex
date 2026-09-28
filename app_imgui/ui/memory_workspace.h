#pragma once

#include "workspace.h"
#include "services/value_scanner.h"
#include "target/module_provider.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ImGuiMultiSelectIO;

namespace cortex::application {
struct CheatTableEntry;
}

namespace cortex::ui {

class MemoryWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "memory"; }
    const char* Title() const override { return "Memory"; }
    void Draw(UiContext& context) override;
    void HandleCommand(UiContext& context, const std::string& command) override;
    void Tick(UiContext& context) override;

private:
    struct ScanTask {
        services::ScanStatePtr state;
        std::string error;
        std::string pauseError;
    };

    enum class FreezeMode : uint8_t { Always = 0, AllowIncrease, AllowDecrease };

    // Cheat Engine's per-entry hotkey actions, in its order.
    enum class EntryHotkeyAction : uint8_t {
        ToggleFreeze = 0, ToggleFreezeAllowIncrease, ToggleFreezeAllowDecrease,
        Freeze, Unfreeze, SetValue, IncreaseValue, DecreaseValue
    };

    struct EntryHotkey {
        std::string chord;  // "Ctrl+F1"
        EntryHotkeyAction action = EntryHotkeyAction::ToggleFreeze;
        std::string value;  // Set / Increase / Decrease value
    };

    struct DropDownItem {
        std::string value;
        std::string label;
    };

    struct AddressEntry {
        uint64_t address = 0;
        services::ScanDataType type = services::ScanDataType::Int32;
        size_t size = 4;              // String and Array of bytes length
        bool utf16 = false;
        bool hex = false;
        bool unsignedValue = false;
        std::string description;
        std::vector<uint8_t> lastValue;
        std::vector<uint8_t> frozenValue;
        bool freeze = false;
        FreezeMode freezeMode = FreezeMode::Always;
        bool readable = true;
        bool selected = false;
        // module+baseOffset entries (static addresses and pointer bases) are
        // resolved on every refresh, then the offsets are followed. Without
        // a module, baseOffset is an absolute pointer base.
        bool pointer = false;
        std::string module;
        uint64_t baseOffset = 0;
        std::vector<uint32_t> offsets;
        unsigned pointerSize = 8;
        // Cheat tables: group headers, nesting, and an address Cortex could
        // not parse (a symbol), kept as written.
        bool group = false;
        int depth = 0;
        bool collapsed = false;
        // A Cheat Engine address expression ([[game.exe+10]+20]+8, symbols),
        // evaluated on every refresh.
        std::string expression;
        uint32_t uid = 0;              // stable id for hotkey commands
        std::vector<EntryHotkey> hotkeys;
        int64_t color = -1;            // description color, 0xRRGGBB
        std::vector<DropDownItem> dropDown;
        bool dropDownOnly = false;     // show the label without the value
    };

    // Layout
    void DrawWelcome(UiContext& context);
    void DrawResults(UiContext& context, float height);
    void DrawScanPanel(UiContext& context, float height);
    void DrawScanOptions(UiContext& context);
    void DrawAddressList(UiContext& context);
    void DrawDialogs(UiContext& context);

    // Scanning
    void ApplyDefaults(UiContext& context);
    void ResetForTarget(UiContext& context, const std::string& targetId);
    void PollScan(UiContext& context);
    void StartScan(UiContext& context);
    void NewScan(UiContext& context);
    void UndoScan(UiContext& context);
    void CancelScan();
    services::ScanDataType SelectedType() const;
    services::ScanQuery BuildQuery() const;
    bool BuildOptions(UiContext& context, services::ScanOptions& options, std::string& error) const;
    std::vector<services::ScanCompare> AvailableCompares() const;
    void SetScan(services::ScanStatePtr state);

    // Results
    void RefreshModules(UiContext& context, bool force = false);
    const target::ModuleInfo* ModuleFor(uint64_t address) const;
    std::string AddressText(uint64_t address, bool& isStatic) const;
    bool ResolveAddress(UiContext& context, const std::string& text, uint64_t& address);
    const std::vector<uint8_t>& LiveValue(UiContext& context, size_t index);
    std::string FormatHit(const uint8_t* data, size_t index) const;
    std::vector<size_t> ActionRows(size_t clicked) const;
    void ApplySelection(ImGuiMultiSelectIO* io);
    void AddResultsToList(const std::vector<size_t>& rows);
    void SaveResultsToProject(UiContext& context, const std::vector<size_t>& rows);
    void RemoveResults(UiContext& context, const std::vector<size_t>& rows);
    void CopyResults(const std::vector<size_t>& rows) const;

    // Address list
    void RefreshAddressValues(UiContext& context);
    bool AddManualAddress(UiContext& context);
    bool CommitValueEdit(UiContext& context);
    void BeginValueEdit(size_t index);
    void SaveEntryToProject(UiContext& context, const AddressEntry& entry);
    std::string FormatEntry(const AddressEntry& entry) const;
    bool HasAddress(uint64_t address, services::ScanDataType type) const;
    bool ResolveEntry(UiContext& context, AddressEntry& entry, std::string* error = nullptr);
    void MakeStatic(AddressEntry& entry) const;
    void OpenTable(UiContext& context);
    void SaveTable(UiContext& context);
    void SelectEntry(size_t index);
    void TakePendingAddresses(UiContext& context);
    std::string PointerText(const AddressEntry& entry) const;
    std::string DisplayValue(const AddressEntry& entry) const;
    bool HandleAddressCommand(UiContext& context, const std::string& command);
    void RunEntryHotkey(UiContext& context, size_t index, const EntryHotkey& hotkey);
    void SetFreeze(AddressEntry& entry, bool freeze);
    void PublishHotkeys(UiContext& context);
    application::CheatTableEntry ToTableEntry(const AddressEntry& entry) const;
    AddressEntry FromTableEntry(const application::CheatTableEntry& item, unsigned pointerSize) const;
    unsigned TargetPointerSize(UiContext& context) const;
    void CopyEntries(bool selectedOnly);
    void PasteEntries(UiContext& context);
    void GroupSelected(UiContext& context);
    size_t BlockEnd(size_t index) const;
    void MoveBlock(size_t from, size_t before, int depth = -1);
    void BeginAddressEdit(size_t index);
    int EntryIndex(uint32_t uid) const;
    void DrawEntryMenu(UiContext& context, size_t index, size_t groupEnd, bool& remove);
    void DrawHotkeyDialog(UiContext& context);
    void DrawDropDownDialog(UiContext& context);

    // Scan inputs
    char scanValue_[256] = "100";
    char scanValue2_[256] = "";
    int typeIndex_ = static_cast<int>(services::ScanDataType::Int32);
    services::ScanCompare compare_ = services::ScanCompare::Exact;
    bool hex_ = false;
    bool unsigned_ = false;
    bool invert_ = false;
    bool compareToFirst_ = false;
    bool utf16_ = false;
    bool caseSensitive_ = false;
    int rounding_ = 0;

    // Memory scan options
    int regionIndex_ = 0;             // 0 = all memory, n = modules_[n - 1]
    char start_[24] = "0";
    char stop_[24] = "7FFFFFFFFFFF";
    services::ScanTristate writable_ = services::ScanTristate::Yes;
    services::ScanTristate executable_ = services::ScanTristate::Any;
    services::ScanTristate copyOnWrite_ = services::ScanTristate::No;
    bool fastScan_ = true;
    int alignment_ = 0;
    bool pauseWhileScanning_ = false;
    bool includePrivate_ = true;
    bool includeImage_ = true;
    bool includeMapped_ = false;

    // Scan state
    services::ScanStatePtr scan_;
    services::ScanStatePtr undo_;
    bool scanRunning_ = false;
    bool scanWasFirst_ = false;
    std::future<ScanTask> scanFuture_;
    std::shared_ptr<std::atomic_bool> scanCancel_;
    std::shared_ptr<services::ScanProgress> scanProgress_;
    std::string activeTargetId_;
    std::string lastScanError_;

    // Results view
    std::vector<uint8_t> selected_;
    int selectedCount_ = 0;
    std::unordered_map<size_t, std::vector<uint8_t>> liveCache_;
    std::chrono::steady_clock::time_point lastLiveRefresh_{};
    std::vector<target::ModuleInfo> modules_;
    std::chrono::steady_clock::time_point lastModuleRefresh_{};
    bool focusValue_ = false;

    // Address list
    std::vector<AddressEntry> addresses_;
    std::chrono::steady_clock::time_point lastAddressRefresh_{};
    bool openAddAddress_ = false;
    bool openEditValue_ = false;
    int editAddressIndex_ = -1;
    char editValue_[512] = {};
    char addAddress_[128] = {};
    char addDescription_[160] = {};
    int addTypeIndex_ = static_cast<int>(services::ScanDataType::Int32);
    int addLength_ = 16;
    bool addUtf16_ = false;
    bool addPointer_ = false;
    char addOffsets_[128] = {};
    size_t selectionAnchor_ = 0;
    std::string tablePath_;
    uint32_t nextUid_ = 1;
    uint32_t editEntryUid_ = 0;       // Change address: the entry edited, 0 = Add
    std::map<std::string, std::string> publishedHotkeys_;

    // Entry hotkeys dialog
    bool openHotkeys_ = false;
    uint32_t hotkeyEntryUid_ = 0;
    bool capturingEntryHotkey_ = false;
    std::string newHotkeyChord_;
    int newHotkeyAction_ = 0;
    char newHotkeyValue_[64] = {};

    // Dropdown list dialog
    bool openDropDown_ = false;
    uint32_t dropDownEntryUid_ = 0;
    char dropDownText_[8192] = {};
    bool dropDownOnlyEdit_ = false;

    // A cheat table's Lua script, offered to the Lua engine.
    std::string tableLuaScript_;
    bool openLuaPrompt_ = false;

    float upperRatio_ = 0.55f;        // scanner height / workspace height
};

} // namespace cortex::ui
