#pragma once

#include "workspace.h"
#include "services/scan_service.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace cortex::ui {

class MemoryWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "memory"; }
    const char* Title() const override { return "Memory"; }
    void Draw(UiContext& context) override;

private:
    struct ScanTaskResult {
        bool ok = false;
        std::string error;
        std::vector<services::ScanResult> results;
    };

    struct AddressEntry {
        uint64_t address = 0;
        services::ScanValueKind kind = services::ScanValueKind::I32;
        std::string description;
        std::vector<uint8_t> lastValue;
        std::vector<uint8_t> frozenValue;
        bool freeze = false;
    };

    void DrawWelcome(UiContext& context);
    void DrawResults(UiContext& context, float height);
    void DrawScanPanel(UiContext& context, float height);
    void DrawAddressList(UiContext& context);
    void DrawDialogs(UiContext& context);
    void PollScan(UiContext& context);
    void StartScan(UiContext& context);
    void ResetForTarget(const std::string& targetId);
    void NewScan(UiContext& context);
    void RefreshAddressValues(UiContext& context);

    std::vector<uint8_t> EncodeValue(std::string& error) const;
    std::vector<uint8_t> EncodeText(const char* text, services::ScanValueKind kind,
                                    std::string& error) const;
    std::string FormatValue(const std::vector<uint8_t>& value,
                            services::ScanValueKind kind) const;
    services::ScanValueKind SelectedKind() const;
    services::ScanComparison SelectedComparison() const;
    void AddAddress(const services::ScanResult& result);
    bool AddManualAddress(UiContext& context);
    bool CommitValueEdit(UiContext& context);
    void BeginValueEdit(size_t index);
    void NavigateAddress(UiContext& context, uint64_t address, const char* workspace);
    void FindWriter(UiContext& context, uint64_t address);
    bool HasAddress(uint64_t address) const;

    char scanValue_[160] = "100";
    int typeIndex_ = 0;
    int comparisonIndex_ = 0;
    bool firstScanDone_ = false;
    bool scanRunning_ = false;
    std::string activeTargetId_;

    std::vector<services::ScanResult> scanResults_;
    std::vector<AddressEntry> addresses_;

    std::future<ScanTaskResult> scanFuture_;
    std::shared_ptr<std::atomic_bool> scanCancel_;
    std::chrono::steady_clock::time_point lastAddressRefresh_{};

    bool openAddAddress_ = false;
    bool openEditValue_ = false;
    int editAddressIndex_ = -1;
    char editValue_[256] = {};
    char addAddress_[64] = {};
    char addDescription_[160] = {};
    int addTypeIndex_ = 0;
};

} // namespace cortex::ui
