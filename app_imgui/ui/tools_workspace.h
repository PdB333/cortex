#pragma once

#include "workspace.h"
#include "services/memory_tools.h"
#include "services/pointer_scanner.h"
#include "services/signature.h"
#include "target/module_provider.h"

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
    enum class Tab { Regions, Pe, Strings, Caves, Signature, Pointers };

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
    void PollPointerScan(UiContext& context);
    std::vector<services::PointerModule> CurrentPointerModules() const;

    std::string targetId_;
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
};

} // namespace cortex::ui
