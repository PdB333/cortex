#pragma once

#include "workspace.h"
#include "services/value_scanner.h"
#include "target/module_provider.h"
#include "target/session.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::ui {

// Hex editor over the target's memory: a 64 KB window that follows the
// current address, several display types, bytes that changed shown in red,
// in-place editing, a data inspector for the selected address and a search.
class MemoryBrowserWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "memory-browser"; }
    const char* Title() const override { return "Memory viewer"; }
    void Draw(UiContext& context) override;

private:
    enum class Display { Byte = 0, Word, Dword, Qword, Int32, Float, Double };

    void GoTo(uint64_t address, bool select = true);
    void Refresh(UiContext& context, bool force);
    bool ResolveAddress(UiContext& context, const std::string& text, uint64_t& address);
    bool Readable(uint64_t address, size_t size) const;
    bool ReadCached(uint64_t address, void* buffer, size_t size) const;
    bool WriteBytes(UiContext& context, uint64_t address, const std::vector<uint8_t>& bytes);
    size_t CellSize() const;
    std::string CellText(uint64_t address) const;
    std::string AddressLabel(uint64_t address, bool& isStatic) const;
    const target::MemoryRegion* RegionAt(uint64_t address) const;
    void HandleKeyboard(UiContext& context);
    void Find(UiContext& context, bool fromStart);

    void DrawToolbar(UiContext& context);
    void DrawHex(UiContext& context, float width, float height);
    // Monospace layout of one hex row: address column, cell and total width.
    struct RowLayout {
        float addressWidth = 0;
        float cellWidth = 0;
        float width = 0;
    };
    RowLayout MeasureRow(UiContext& context) const;
    void DrawInspector(UiContext& context);
    void DrawEditPopup(UiContext& context);

    static constexpr uint64_t kWindow = 0x10000;
    static constexpr uint64_t kPage = 0x1000;

    std::string targetId_;
    char address_[128] = "";
    char find_[256] = "";
    int display_ = 0;
    uint64_t base_ = 0;             // start of the visible window
    uint64_t selected_ = 0;
    bool hasSelection_ = false;
    bool scrollToSelection_ = false;
    int pendingNibble_ = -1;        // first hex digit typed into a byte

    std::vector<uint8_t> bytes_;
    std::vector<uint8_t> pageValid_;
    std::vector<uint8_t> previous_;
    std::vector<std::chrono::steady_clock::time_point> changedAt_;
    bool liveRefresh_ = true;
    int liveRateIndex_ = 1;
    std::chrono::steady_clock::time_point lastRefresh_{};

    std::vector<target::MemoryRegion> regions_;
    std::vector<target::ModuleInfo> modules_;
    std::chrono::steady_clock::time_point lastLayout_{};

    uint64_t menuAddress_ = 0;
    bool openEdit_ = false;
    int editType_ = 0;
    uint64_t editAddress_ = 0;
    char editValue_[256] = "";
    std::string findInfo_;
};

} // namespace cortex::ui
