#pragma once

#include "workspace.h"
#include "services/memory_tools.h"
#include "services/pointer_scanner.h"
#include "services/signature.h"
#include "services/assembler.h"
#include "services/speedhack.h"
#include "target/module_provider.h"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace cortex::ui {

// Memory tools in the spirit of Cheat Engine's Memory View > Tools: memory
// regions, PE headers, strings, code caves, AOB signatures and the pointer
// scanner. Everything reads the target from outside; nothing is injected.
class ToolsWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "tools"; }
    const char* Title() const override { return "Memory tools"; }
    void Draw(UiContext& context) override;

private:
    enum class Tab { Regions, Pe, Strings, Caves, Signature, Pointers, Symbols, Assembler, Speed, Grouped };

    void Reset(const std::string& targetId);
    void RefreshModules(UiContext& context);
    bool ModuleCombo(const char* id, int& index);
    const target::ModuleInfo* ModuleAt(int index) const;
    std::string AddressText(uint64_t address) const;
    bool Resolve(UiContext& context, const char* text, uint64_t& address);
    services::MemoryReader Reader(UiContext& context) const;
    bool ReadModuleSections(UiContext& context, const target::ModuleInfo& module, bool executableOnly,
                            std::vector<std::pair<uint64_t, std::vector<uint8_t>>>& sections, std::string& error);

    void DrawRegions(UiContext& context);
    void DrawPe(UiContext& context);
    void DrawStrings(UiContext& context);
    void DrawCaves(UiContext& context);
    void DrawSignature(UiContext& context);
    void DrawPointers(UiContext& context);
    void DrawSymbols(UiContext& context);
    void PollPointerScan(UiContext& context);
    std::vector<services::PointerModule> CurrentPointerModules() const;

    std::string targetId_;
    Tab active_ = Tab::Regions;
    Tab requested_ = Tab::Regions;
    bool tabPending_ = false;
    std::vector<target::ModuleInfo> modules_;
    std::chrono::steady_clock::time_point lastModules_{};

    // Regions
    std::vector<target::MemoryRegion> regions_;
    char regionFilter_[64] = {};
    bool regionsWritableOnly_ = false;
    bool regionsLoaded_ = false;

    // PE headers
    int peModule_ = 0;
    services::PeImage pe_;
    std::string peError_;
    bool peLoaded_ = false;
    char peFilter_[96] = {};

    // Strings
    int stringsModule_ = 0;
    int stringsMin_ = 5;
    bool stringsAscii_ = true;
    bool stringsUtf16_ = true;
    char stringsFilter_[128] = {};
    std::vector<services::FoundString> strings_;
    std::vector<size_t> stringsView_;
    std::string stringsViewKey_ = "\x01";
    std::string stringsInfo_;

    // Code caves
    int cavesModule_ = 0;
    int cavesMin_ = 32;
    bool cavesExecutableOnly_ = true;
    std::vector<services::CodeCave> caves_;
    std::string cavesInfo_;

    // Signature
    char signatureAddress_[96] = {};
    bool signatureDisplacements_ = true;
    bool signatureImmediates_ = true;
    services::Signature signature_;
    std::string signatureInfo_;
    char testPattern_[512] = {};
    std::string testInfo_;

    // Pointer scan
    char pointerTarget_[96] = {};
    int pointerLevel_ = 5;
    char pointerOffset_[16] = "1000";
    int pointerMaxResults_ = 100000;
    bool pointerMapped_ = false;
    bool pointerRunning_ = false;
    std::future<std::pair<bool, std::string>> pointerFuture_;
    std::shared_ptr<services::PointerScanResult> pointerPending_;
    std::shared_ptr<std::atomic_bool> pointerCancel_;
    std::shared_ptr<services::ScanProgress> pointerProgress_;
    services::PointerScanResult pointers_;
    bool pointersLoaded_ = false;
    char pointerRescan_[96] = {};
    char pointerFile_[512] = {};
    std::string pointerInfo_;
    std::chrono::steady_clock::time_point lastPointerRefresh_{};
    std::vector<uint64_t> pointerLive_;
    int pointerLiveFirst_ = -1;

    // Symbols
    char symbolName_[96] = {};
    char symbolAddress_[160] = {};
    char symbolSearch_[96] = {};
    std::vector<std::pair<std::string, uint64_t>> symbolMatches_;
    std::string symbolSearched_;

    // Assembler
    int assemblerMode_ = 0;             // 0 = assemble & write, 1 = code injection
    char assemblerAddress_[96] = {};
    std::array<char, 16384> assemblerSource_ = {};
    std::vector<uint8_t> assembledBytes_;
    uint64_t assembledAt_ = 0;
    std::string assemblerInfo_;
    bool assemblerError_ = false;
    // Code injection record, for Restore.
    struct Injection {
        uint64_t site = 0;
        uint64_t cave = 0;
        size_t caveSize = 0;
        std::vector<uint8_t> original;
        std::string description;
    };
    std::vector<Injection> injections_;
    int injectSteal_ = 5;

    // Grouped scan
    char groupedText_[256] = {};
    int groupedWindow_ = 64;
    int groupedDefaultSize_ = 4;
    bool groupedOrdered_ = true;
    bool groupedWritableOnly_ = true;
    std::vector<services::GroupedHit> grouped_;
    std::vector<services::GroupedElement> groupedElements_;
    std::string groupedInfo_;

    // Speedhack
    services::SpeedhackState speed_;
    uint64_t speedPid_ = 0;
    float speedValue_ = 1.0f;
    std::string speedInfo_;

    void DrawAssembler(UiContext& context);
    void DrawSpeedhack(UiContext& context);
    void DrawGroupedScan(UiContext& context);
    services::SpeedhackHost SpeedhackHostFor(UiContext& context, uint64_t pid) const;
    bool AssembleCurrent(UiContext& context, uint64_t address, services::AssembleBlockResult& result, std::string& error);
    int StealLength(UiContext& context, uint64_t address, int minimum) const;
    bool InjectCode(UiContext& context, uint64_t address, std::string& error);
};

} // namespace cortex::ui
