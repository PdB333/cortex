#include "tools_workspace.h"
#include "address_resolver.h"
#include "widgets.h"
#include "address_context_menu.h"

#include "process/remote_memory.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace cortex::ui {
namespace {

std::string Hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string Size(uint64_t bytes) {
    char buffer[48] = {};
    if (bytes >= 1024ull * 1024 * 1024) std::snprintf(buffer, sizeof(buffer), "%.2f GB", bytes / 1073741824.0);
    else if (bytes >= 1024ull * 1024) std::snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(buffer, sizeof(buffer), "%.1f KB", bytes / 1024.0);
    else std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    return buffer;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

bool ParseHex(const std::string& raw, uint64_t& value) {
    std::string text = Trim(raw);
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.erase(0, 2);
    if (text.empty() || text.size() > 16) return false;
    for (const char ch : text)
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    value = std::strtoull(text.c_str(), nullptr, 16);
    return true;
}

std::string Protection(const target::MemoryRegion& region) {
    std::string text;
    text += region.readable ? 'R' : '-';
    text += region.writable ? 'W' : '-';
    text += region.executable ? 'X' : '-';
    if (region.copyOnWrite) text += " CoW";
    if (region.protection & 0x100) text += " guard";
    if (!region.readable && region.protection == 0x01) text = "no access";
    return text;
}

const char* RegionType(target::MemoryRegionType type) {
    switch (type) {
        case target::MemoryRegionType::Private: return "Private";
        case target::MemoryRegionType::Image: return "Image";
        case target::MemoryRegionType::Mapped: return "Mapped";
        default: return "-";
    }
}

bool ToolTable(const char* id, int columns) {
    return BeginDataTable(id, columns,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable,
                          ImGui::GetContentRegionAvail(), 90.0f);
}

void KeyValue(const char* key, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", key);
    ImGui::TableSetColumnIndex(1);
    MonoTextUnformatted(value.c_str());
}

} // namespace

void ToolsWorkspace::Reset(const std::string& targetId) {
    targetId_ = targetId;
    modules_.clear();
    lastModules_ = {};
    regions_.clear();
    regionsLoaded_ = false;
    pe_ = services::PeImage{};
    peLoaded_ = false;
    peModule_ = 0;
    strings_.clear();
    stringsView_.clear();
    stringsViewKey_ = "\x01";
    stringsInfo_.clear();
    caves_.clear();
    cavesInfo_.clear();
    signature_ = services::Signature{};
    signatureInfo_.clear();
    testInfo_.clear();
    if (pointerRunning_ && pointerCancel_) pointerCancel_->store(true);
    pointers_ = services::PointerScanResult{};
    pointersLoaded_ = false;
    pointerInfo_.clear();
}

void ToolsWorkspace::RefreshModules(UiContext& context) {
    if (!context.modules) return;
    const auto now = std::chrono::steady_clock::now();
    if (lastModules_.time_since_epoch().count() != 0 && now - lastModules_ < std::chrono::seconds(5)) return;
    lastModules_ = now;
    std::string error;
    auto modules = context.modules->List(&error);
    std::sort(modules.begin(), modules.end(),
              [](const auto& left, const auto& right) { return left.base < right.base; });
    modules_ = std::move(modules);
}

const target::ModuleInfo* ToolsWorkspace::ModuleAt(int index) const {
    return index >= 0 && index < static_cast<int>(modules_.size()) ? &modules_[static_cast<size_t>(index)] : nullptr;
}

bool ToolsWorkspace::ModuleCombo(const char* id, int& index) {
    bool changed = false;
    const auto* current = ModuleAt(index);
    ImGui::SetNextItemWidth(std::min(Px(280), ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo(id, current ? current->name.c_str() : "(no module)", ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < static_cast<int>(modules_.size()); ++i) {
            if (ImGui::Selectable(modules_[static_cast<size_t>(i)].name.c_str(), i == index)) {
                index = i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::string ToolsWorkspace::AddressText(uint64_t address) const {
    for (const auto& module : modules_) {
        if (address < module.base || address >= module.base + module.size) continue;
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "+%llX", static_cast<unsigned long long>(address - module.base));
        return module.name + buffer;
    }
    return Hex(address);
}

bool ToolsWorkspace::Resolve(UiContext& context, const char* raw, uint64_t& address) {
    const std::string text = Trim(raw ? raw : "");
    if (ParseHex(text, address) && address) return true;
    return !text.empty() && EvaluateContextAddress(context, text, address);
}

services::MemoryReader ToolsWorkspace::Reader(UiContext& context) const {
    auto session = context.sessions ? context.sessions->Active() : nullptr;
    return [session](uint64_t address, void* buffer, size_t size) {
        return session && session->ReadMemory(address, buffer, size, nullptr);
    };
}

bool ToolsWorkspace::ReadModuleSections(UiContext& context, const target::ModuleInfo& module, bool executableOnly,
                                        std::vector<std::pair<uint64_t, std::vector<uint8_t>>>& sections,
                                        std::string& error) {
    sections.clear();
    const auto read = Reader(context);
    services::PeImage image;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    if (services::ParsePeImage(read, module.base, image, nullptr) && !image.sections.empty()) {
        for (const auto& section : image.sections) {
            if (executableOnly && !section.Executable()) continue;
            const uint64_t size = std::max(section.virtualSize, section.rawSize);
            if (size) ranges.push_back({module.base + section.virtualAddress, size});
        }
    } else if (!executableOnly) {
        ranges.push_back({module.base, module.size});
    }
    if (ranges.empty()) {
        error = executableOnly ? "The module has no executable section" : "Nothing to read";
        return false;
    }
    for (const auto& range : ranges) {
        std::vector<uint8_t> bytes(static_cast<size_t>(std::min<uint64_t>(range.second, 512ull * 1024 * 1024)));
        if (!read(range.first, bytes.data(), bytes.size())) {
            for (size_t offset = 0; offset < bytes.size(); offset += 4096) {
                const size_t count = std::min<size_t>(4096, bytes.size() - offset);
                if (!read(range.first + offset, bytes.data() + offset, count)) std::memset(bytes.data() + offset, 0, count);
            }
        }
        sections.emplace_back(range.first, std::move(bytes));
    }
    return true;
}

std::vector<services::PointerModule> ToolsWorkspace::CurrentPointerModules() const {
    std::vector<services::PointerModule> modules;
    modules.reserve(modules_.size());
    for (const auto& module : modules_) modules.push_back({module.name, module.base, module.size});
    return modules;
}

// ------------------------------------------------------------------ regions

void ToolsWorkspace::DrawRegions(UiContext& context) {
    const auto session = context.sessions->Active();
    if (!regionsLoaded_ || ImGui::Button("Refresh")) {
        regions_ = session->MemoryRegions();
        regionsLoaded_ = true;
    }
    FlowSameLine(Px(220));
    ImGui::SetNextItemWidth(Px(220));
    ImGui::InputTextWithHint("##RegionFilter", "filter: module, address, RWX", regionFilter_, sizeof(regionFilter_));
    FlowSameLine(CheckboxWidth("Writable only"));
    ImGui::Checkbox("Writable only", &regionsWritableOnly_);

    const std::string filter = Lower(regionFilter_);
    std::vector<size_t> view;
    uint64_t total = 0;
    uint64_t writable = 0;
    for (size_t i = 0; i < regions_.size(); ++i) {
        const auto& region = regions_[i];
        total += region.size;
        if (region.writable) writable += region.size;
        if (regionsWritableOnly_ && !region.writable) continue;
        if (!filter.empty()) {
            const std::string haystack = Lower(Hex(region.base) + " " + AddressText(region.base) + " " +
                                               Protection(region) + " " + RegionType(region.type));
            if (haystack.find(filter) == std::string::npos) continue;
        }
        view.push_back(i);
    }
    ImGui::TextDisabled("%zu regions, %s committed, %s writable", regions_.size(), Size(total).c_str(),
                        Size(writable).c_str());

    if (!ToolTable("RegionsTable", 5)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthStretch, 0.26f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.14f);
    ImGui::TableSetupColumn("Protection", ImGuiTableColumnFlags_WidthStretch, 0.16f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.12f);
    ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthStretch, 0.32f);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(view.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const auto& region = regions_[view[static_cast<size_t>(row)]];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(row);
            {
                MonoFont mono;
                if (ImGui::Selectable(Hex(region.base).c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    context.NavigateTo("memory-browser", region.base);
            }
            if (ImGui::BeginPopupContextItem("RegionMenu")) {
                if (ImGui::MenuItem("Browse memory")) context.NavigateTo("memory-browser", region.base);
                if (ImGui::MenuItem("Disassemble")) context.NavigateTo("disassembly", region.base);
                if (ImGui::MenuItem("Scan only this region")) {
                    context.commands.push_back("scan_range " + Hex(region.base) + " " +
                                               Hex(region.base + region.size - 1));
                    context.requestWorkspace = "memory";
                }
                if (ImGui::MenuItem("Copy base address")) ImGui::SetClipboardText(Hex(region.base).c_str());
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(Size(region.size).c_str());
            ImGui::TableSetColumnIndex(2);
            MonoTextUnformatted(Protection(region).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(RegionType(region.type));
            ImGui::TableSetColumnIndex(4);
            const std::string owner = AddressText(region.base);
            ImGui::TextUnformatted(owner.rfind("0x", 0) == 0 ? "" : owner.substr(0, owner.find('+')).c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

// ------------------------------------------------------------------ PE headers

void ToolsWorkspace::DrawPe(UiContext& context) {
    if (ModuleCombo("##PeModule", peModule_)) peLoaded_ = false;
    FlowSameLine(ButtonWidth("Reload"));
    if (ImGui::Button("Reload")) peLoaded_ = false;
    const auto* module = ModuleAt(peModule_);
    if (!module) {
        HintText("No module is loaded.");
        return;
    }
    if (!peLoaded_) {
        peError_.clear();
        if (!services::ParsePeImage(Reader(context), module->base, pe_, &peError_)) pe_ = services::PeImage{};
        peLoaded_ = true;
    }
    if (!peError_.empty()) {
        ImGui::TextColored(WarningTextColor(), "%s", peError_.c_str());
        return;
    }

    if (!ImGui::BeginTabBar("PeTabs")) return;
    if (ImGui::BeginTabItem("Summary")) {
        if (BeginDataTable("PeSummary", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, Px(170));
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            char text[96] = {};
            KeyValue("Module", module->name);
            KeyValue("Path", module->path);
            KeyValue("Loaded at", Hex(pe_.base) + (pe_.base != pe_.preferredBase ? "  (relocated from " + Hex(pe_.preferredBase) + ")" : ""));
            KeyValue("Format", pe_.pe32Plus ? "PE32+ (64-bit)" : "PE32 (32-bit)");
            KeyValue("Machine", services::PeMachineName(pe_.machine));
            const std::time_t stamp = static_cast<std::time_t>(pe_.timeDateStamp);
            if (std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&stamp)) == 0) text[0] = '\0';
            KeyValue("Link time", std::string(text) + "  (" + Hex(pe_.timeDateStamp) + ")");
            KeyValue("Entry point", AddressText(pe_.base + pe_.entryPoint) + "  (RVA " + Hex(pe_.entryPoint) + ")");
            KeyValue("Image size", Size(pe_.sizeOfImage) + "  (" + Hex(pe_.sizeOfImage) + ")");
            KeyValue("Subsystem", services::PeSubsystemName(pe_.subsystem));
            std::string flags;
            if (pe_.dllCharacteristics & 0x0040) flags += "ASLR ";
            if (pe_.dllCharacteristics & 0x0020) flags += "high-entropy-VA ";
            if (pe_.dllCharacteristics & 0x0100) flags += "DEP ";
            if (pe_.dllCharacteristics & 0x4000) flags += "CFG ";
            if (pe_.dllCharacteristics & 0x0400) flags += "no-SEH ";
            KeyValue("Security", flags.empty() ? std::string("none") : flags);
            KeyValue("Image type", (pe_.characteristics & 0x2000) ? "DLL" : "Executable");
            KeyValue("Checksum", Hex(pe_.checksum));
            KeyValue("Alignment", "section " + Hex(pe_.sectionAlignment) + ", file " + Hex(pe_.fileAlignment));
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Sections")) {
        if (ToolTable("PeSections", 5)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Virtual size");
            ImGui::TableSetupColumn("Raw size");
            ImGui::TableSetupColumn("Flags");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < pe_.sections.size(); ++i) {
                const auto& section = pe_.sections[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(section.name.c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    context.NavigateTo(section.Executable() ? "disassembly" : "memory-browser",
                                       pe_.base + section.virtualAddress);
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                MonoTextUnformatted(AddressText(pe_.base + section.virtualAddress).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(Size(section.virtualSize).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(Size(section.rawSize).c_str());
                ImGui::TableSetColumnIndex(4);
                MonoTextUnformatted(services::PeSectionFlags(section.characteristics).c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Directories")) {
        if (ToolTable("PeDirectories", 3)) {
            ImGui::TableSetupColumn("Directory");
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Size");
            ImGui::TableHeadersRow();
            for (const auto& directory : pe_.directories) {
                if (!directory.virtualAddress) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(directory.name);
                ImGui::TableSetColumnIndex(1);
                MonoTextUnformatted(AddressText(pe_.base + directory.virtualAddress).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(Size(directory.size).c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    const std::string exportsLabel = "Exports (" + std::to_string(pe_.exports.size()) + ")###Exports";
    if (ImGui::BeginTabItem(exportsLabel.c_str())) {
        ImGui::SetNextItemWidth(Px(260));
        ImGui::InputTextWithHint("##PeFilter", "filter", peFilter_, sizeof(peFilter_));
        const std::string filter = Lower(peFilter_);
        std::vector<size_t> view;
        for (size_t i = 0; i < pe_.exports.size(); ++i)
            if (filter.empty() || Lower(pe_.exports[i].name).find(filter) != std::string::npos) view.push_back(i);
        if (ToolTable("PeExports", 4)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.45f);
            ImGui::TableSetupColumn("Ordinal", ImGuiTableColumnFlags_WidthStretch, 0.1f);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.25f);
            ImGui::TableSetupColumn("Forwarded to", ImGuiTableColumnFlags_WidthStretch, 0.2f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(view.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const auto& entry = pe_.exports[view[static_cast<size_t>(row)]];
                    const uint64_t address = pe_.base + entry.rva;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(row);
                    const std::string name = entry.name.empty() ? "#" + std::to_string(entry.ordinal) : entry.name;
                    if (ImGui::Selectable(name.c_str(), false,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && entry.forwarder.empty())
                        context.NavigateTo("disassembly", address);
                    if (ImGui::BeginPopupContextItem("ExportMenu")) {
                        AddressContextOptions options;
                        options.label = name;
                        DrawAddressContextActions(context, address, options);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%u", entry.ordinal);
                    ImGui::TableSetColumnIndex(2);
                    MonoTextUnformatted(Hex(address).c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(entry.forwarder.c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    const std::string importsLabel = "Imports (" + std::to_string(pe_.imports.size()) + ")###Imports";
    if (ImGui::BeginTabItem(importsLabel.c_str())) {
        ImGui::SetNextItemWidth(Px(260));
        ImGui::InputTextWithHint("##PeImportFilter", "filter", peFilter_, sizeof(peFilter_));
        const std::string filter = Lower(peFilter_);
        std::vector<size_t> view;
        for (size_t i = 0; i < pe_.imports.size(); ++i) {
            const auto& entry = pe_.imports[i];
            if (filter.empty() || Lower(entry.name).find(filter) != std::string::npos ||
                Lower(entry.module).find(filter) != std::string::npos)
                view.push_back(i);
        }
        if (ToolTable("PeImports", 4)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch, 0.22f);
            ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 0.3f);
            ImGui::TableSetupColumn("IAT slot", ImGuiTableColumnFlags_WidthStretch, 0.24f);
            ImGui::TableSetupColumn("Points to", ImGuiTableColumnFlags_WidthStretch, 0.24f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(view.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const auto& entry = pe_.imports[view[static_cast<size_t>(row)]];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(row);
                    if (ImGui::Selectable(entry.module.c_str(), false,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && entry.value)
                        context.NavigateTo("disassembly", entry.value);
                    if (ImGui::BeginPopupContextItem("ImportMenu")) {
                        if (ImGui::MenuItem("Disassemble the imported function") && entry.value)
                            context.NavigateTo("disassembly", entry.value);
                        if (ImGui::MenuItem("Browse the IAT slot")) context.NavigateTo("memory-browser", entry.slot);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                    ImGui::TableSetColumnIndex(1);
                    if (entry.name.empty()) ImGui::Text("#%u", entry.ordinal);
                    else ImGui::TextUnformatted(entry.name.c_str());
                    ImGui::TableSetColumnIndex(2);
                    MonoTextUnformatted(AddressText(entry.slot).c_str());
                    ImGui::TableSetColumnIndex(3);
                    MonoTextUnformatted(AddressText(entry.value).c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

// ------------------------------------------------------------------ strings

void ToolsWorkspace::DrawStrings(UiContext& context) {
    ModuleCombo("##StringsModule", stringsModule_);
    FlowSameLine(Px(170));
    ImGui::SetNextItemWidth(Px(110));
    if (ImGui::InputInt("Min length", &stringsMin_)) stringsMin_ = std::clamp(stringsMin_, 2, 256);
    FlowSameLine(CheckboxWidth("ASCII"));
    ImGui::Checkbox("ASCII", &stringsAscii_);
    FlowSameLine(CheckboxWidth("UTF-16"));
    ImGui::Checkbox("UTF-16", &stringsUtf16_);
    FlowSameLine(ButtonWidth("Find strings"));
    if (ImGui::Button("Find strings")) {
        if (const auto* module = ModuleAt(stringsModule_)) {
            const auto chosen = *module;
            context.RunInBackground("Searching strings in " + chosen.name, [this, &context, chosen]() {
                std::vector<std::pair<uint64_t, std::vector<uint8_t>>> sections;
                std::string error;
                strings_.clear();
                if (!ReadModuleSections(context, chosen, false, sections, error)) {
                    stringsInfo_ = error;
                    return;
                }
                for (const auto& section : sections)
                    services::FindStrings(section.second.data(), section.second.size(), section.first,
                                          static_cast<size_t>(stringsMin_), stringsAscii_, stringsUtf16_, 200000,
                                          strings_);
                stringsInfo_ = std::to_string(strings_.size()) + " string(s) in " + chosen.name +
                               (strings_.size() >= 200000 ? " (limit reached)" : "");
                stringsViewKey_ = "\x01";
            });
        }
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##StringsFilter", "filter the strings", stringsFilter_, sizeof(stringsFilter_));
    if (!stringsInfo_.empty()) ImGui::TextDisabled("%s", stringsInfo_.c_str());

    const std::string key = Lower(stringsFilter_);
    if (key != stringsViewKey_) {
        stringsViewKey_ = key;
        stringsView_.clear();
        for (size_t i = 0; i < strings_.size(); ++i)
            if (key.empty() || Lower(strings_[i].text).find(key) != std::string::npos) stringsView_.push_back(i);
    }
    if (strings_.empty()) {
        HintText("Pick a module and press Find strings to list its ASCII and UTF-16 text.");
        return;
    }
    if (!ToolTable("StringsTable", 3)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.26f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, Px(60));
    ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch, 0.74f);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(stringsView_.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const auto& entry = strings_[stringsView_[static_cast<size_t>(row)]];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(row);
            {
                MonoFont mono;
                if (ImGui::Selectable(AddressText(entry.address).c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    context.NavigateTo("memory-browser", entry.address);
            }
            if (ImGui::BeginPopupContextItem("StringMenu")) {
                if (ImGui::MenuItem("Copy text")) ImGui::SetClipboardText(entry.text.c_str());
                if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(Hex(entry.address).c_str());
                ImGui::Separator();
                AddressContextOptions options;
                options.label = entry.text.substr(0, 40);
                DrawAddressContextActions(context, entry.address, options);
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(entry.utf16 ? "UTF-16" : "ASCII");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(entry.text.c_str());
        }
    }
    ImGui::EndTable();
}

// ------------------------------------------------------------------ code caves

void ToolsWorkspace::DrawCaves(UiContext& context) {
    ModuleCombo("##CavesModule", cavesModule_);
    FlowSameLine(Px(170));
    ImGui::SetNextItemWidth(Px(110));
    if (ImGui::InputInt("Min size", &cavesMin_)) cavesMin_ = std::clamp(cavesMin_, 4, 65536);
    FlowSameLine(CheckboxWidth("Executable sections only"));
    ImGui::Checkbox("Executable sections only", &cavesExecutableOnly_);
    FlowSameLine(ButtonWidth("Find code caves"));
    if (ImGui::Button("Find code caves")) {
        if (const auto* module = ModuleAt(cavesModule_)) {
            const auto chosen = *module;
            context.RunInBackground("Searching code caves in " + chosen.name, [this, &context, chosen]() {
                std::vector<std::pair<uint64_t, std::vector<uint8_t>>> sections;
                std::string error;
                caves_.clear();
                if (!ReadModuleSections(context, chosen, cavesExecutableOnly_, sections, error)) {
                    cavesInfo_ = error;
                    return;
                }
                for (const auto& section : sections)
                    services::FindCodeCaves(section.second.data(), section.second.size(), section.first,
                                            static_cast<size_t>(cavesMin_), 50000, caves_);
                std::sort(caves_.begin(), caves_.end(),
                          [](const auto& left, const auto& right) { return left.size > right.size; });
                cavesInfo_ = std::to_string(caves_.size()) + " cave(s) in " + chosen.name + ", largest first";
            });
        }
    }
    if (!cavesInfo_.empty()) ImGui::TextDisabled("%s", cavesInfo_.c_str());
    if (caves_.empty()) {
        HintText("Code caves are runs of 00 or CC bytes that a patch can jump to. Pick a module and search.");
        return;
    }
    if (!ToolTable("CavesTable", 3)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.25f);
    ImGui::TableSetupColumn("Filler", ImGuiTableColumnFlags_WidthStretch, 0.25f);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(caves_.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const auto& cave = caves_[static_cast<size_t>(row)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(row);
            {
                MonoFont mono;
                if (ImGui::Selectable(AddressText(cave.address).c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    context.NavigateTo("disassembly", cave.address);
            }
            if (ImGui::BeginPopupContextItem("CaveMenu")) {
                if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(Hex(cave.address).c_str());
                ImGui::Separator();
                DrawAddressContextActions(context, cave.address);
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu bytes", static_cast<unsigned long long>(cave.size));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%02X", cave.filler);
        }
    }
    ImGui::EndTable();
}

// ------------------------------------------------------------------ signature

void ToolsWorkspace::DrawSignature(UiContext& context) {
    const auto session = context.sessions->Active();
    ImGui::TextDisabled("Instruction address");
    ImGui::SetNextItemWidth(std::min(Px(320), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##SignatureAddress", "game.exe+1A2B0 or 7FF6...", signatureAddress_,
                             sizeof(signatureAddress_));
    FlowSameLine(ButtonWidth("Generate"));
    const bool generate = ImGui::Button("Generate");
    ImGui::Checkbox("Wildcard 32-bit displacements", &signatureDisplacements_);
    FlowSameLine(CheckboxWidth("Wildcard large immediates"));
    ImGui::Checkbox("Wildcard large immediates", &signatureImmediates_);

    if (generate) {
        uint64_t address = 0;
        signature_ = services::Signature{};
        if (!Resolve(context, signatureAddress_, address)) {
            signatureInfo_ = "Enter an address or module+offset";
        } else {
            const target::ModuleInfo* module = nullptr;
            for (const auto& candidate : modules_)
                if (address >= candidate.base && address < candidate.base + candidate.size) module = &candidate;
            std::vector<std::pair<uint64_t, std::vector<uint8_t>>> sections;
            std::string error;
            std::vector<uint8_t> haystack;
            if (module && ReadModuleSections(context, *module, true, sections, error)) {
                for (const auto& section : sections) {
                    haystack.insert(haystack.end(), section.second.begin(), section.second.end());
                    haystack.insert(haystack.end(), 32, 0);
                }
            }
            std::vector<uint8_t> code(128);
            size_t size = code.size();
            while (size >= 16 && !session->ReadMemory(address, code.data(), size, nullptr)) size /= 2;
            services::SignatureOptions options;
            options.x64 = session->Target().architecture != target::Architecture::X86;
            options.wildcardDisplacements = signatureDisplacements_;
            options.wildcardImmediates = signatureImmediates_;
            if (size < 16) {
                signatureInfo_ = "The address cannot be read";
            } else if (!services::GenerateSignature(code.data(), size, haystack.data(), haystack.size(), options,
                                                    signature_, &error)) {
                signatureInfo_ = error;
            } else {
                const std::string where = module ? module->name : std::string("the searched code");
                signatureInfo_ = signature_.matches == 1
                    ? "Unique in " + where + " (" + std::to_string(signature_.instructions) + " instructions)"
                    : std::to_string(signature_.matches) + "+ matches in " + where +
                          ": the code around this address repeats; try an instruction nearby";
                if (!module) signatureInfo_ = "Not inside a module: uniqueness was not checked";
            }
        }
    }

    if (!signature_.bytes.empty()) {
        ImGui::Spacing();
        std::string text = signature_.Text();
        ImGui::SetNextItemWidth(-ButtonWidth("Copy") - ImGui::GetStyle().ItemSpacing.x);
        {
            MonoFont mono;
            ImGui::InputText("##SignatureText", text.data(), text.size() + 1, ImGuiInputTextFlags_ReadOnly);
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy")) ImGui::SetClipboardText(text.c_str());
    }
    if (!signatureInfo_.empty())
        ImGui::TextColored(signature_.matches == 1 ? StaticAddressColor() : WarningTextColor(), "%s",
                           signatureInfo_.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Find a byte pattern in a module");
    ModuleCombo("##TestModule", peModule_);
    FlowSameLine(Px(200));
    ImGui::SetNextItemWidth(std::max(Px(200), ImGui::GetContentRegionAvail().x - ButtonWidth("Find") - Px(8)));
    ImGui::InputTextWithHint("##TestPattern", "48 8B 05 ?? ?? ?? ?? 48 85 C0", testPattern_, sizeof(testPattern_));
    ImGui::SameLine();
    if (ImGui::Button("Find") && ModuleAt(peModule_)) {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> mask;
        bool valid = true;
        std::string token;
        for (const char ch : std::string(testPattern_) + " ") {
            if (!std::isspace(static_cast<unsigned char>(ch))) {
                token += ch;
                continue;
            }
            if (token.empty()) continue;
            if (token == "?" || token == "??" || token == "*") {
                bytes.push_back(0);
                mask.push_back(0);
            } else if (token.size() == 2 && std::isxdigit(static_cast<unsigned char>(token[0])) &&
                       std::isxdigit(static_cast<unsigned char>(token[1]))) {
                bytes.push_back(static_cast<uint8_t>(std::strtoul(token.c_str(), nullptr, 16)));
                mask.push_back(0xFF);
            } else {
                valid = false;
            }
            token.clear();
        }
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> sections;
        std::string error;
        if (!valid || bytes.empty()) {
            testInfo_ = "Bytes must look like: 48 8B 05 ?? ?? ?? ??";
        } else if (!ReadModuleSections(context, *ModuleAt(peModule_), false, sections, error)) {
            testInfo_ = error;
        } else {
            size_t total = 0;
            uint64_t first = 0;
            for (const auto& section : sections) {
                uint64_t offset = 0;
                const size_t count = services::CountPatternMatches(section.second.data(), section.second.size(), bytes,
                                                                   mask, 1000, &offset);
                if (count && total == 0) first = section.first + offset;
                total += count;
            }
            testInfo_ = total == 0 ? "No match"
                : std::to_string(total) + (total >= 1000 ? "+" : "") + " match(es), first at " + AddressText(first);
            if (total) context.NavigateTo("disassembly", first);
        }
    }
    if (!testInfo_.empty()) ImGui::TextDisabled("%s", testInfo_.c_str());
}

// ------------------------------------------------------------------ pointer scan

void ToolsWorkspace::PollPointerScan(UiContext& context) {
    if (!pointerRunning_ || !pointerFuture_.valid()) return;
    if (pointerFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    const auto outcome = pointerFuture_.get();
    pointerRunning_ = false;
    if (!outcome.first) {
        pointerInfo_ = outcome.second == "scan_cancelled" ? "Pointer scan cancelled" : "Pointer scan failed: " + outcome.second;
        context.status = pointerInfo_;
        return;
    }
    pointers_ = std::move(*pointerPending_);
    pointerPending_.reset();
    pointersLoaded_ = true;
    pointerLive_.clear();
    char text[160] = {};
    std::snprintf(text, sizeof(text), "%zu path(s) from %llu indexed pointers in %.1f s%s", pointers_.paths.size(),
                  static_cast<unsigned long long>(pointers_.pointersIndexed), pointers_.milliseconds / 1000.0,
                  pointers_.truncated ? " - limit reached, shorter paths first" : "");
    pointerInfo_ = text;
    context.status = pointerInfo_;
}

void ToolsWorkspace::DrawPointers(UiContext& context) {
    const auto session = context.sessions->Active();
    const unsigned pointerSize = session->Target().architecture == target::Architecture::X86 ? 4u : 8u;
    if (!pointerFile_[0] && context.settings) {
        const auto directory = context.settings->Path().parent_path() / "pointer-scans";
        std::snprintf(pointerFile_, sizeof(pointerFile_), "%s", (directory / "pointerscan.json").u8string().c_str());
    }

    ImGui::TextDisabled("Address to find");
    ImGui::SetNextItemWidth(std::min(Px(300), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##PointerTarget", "the address found by a value scan", pointerTarget_,
                             sizeof(pointerTarget_));
    ImGui::SetNextItemWidth(Px(100));
    if (ImGui::InputInt("Max level", &pointerLevel_)) pointerLevel_ = std::clamp(pointerLevel_, 1, 12);
    FlowSameLine(Px(190));
    ImGui::SetNextItemWidth(Px(90));
    ImGui::InputText("Max offset (hex)", pointerOffset_, sizeof(pointerOffset_), ImGuiInputTextFlags_CharsHexadecimal);
    FlowSameLine(Px(210));
    ImGui::SetNextItemWidth(Px(110));
    if (ImGui::InputInt("Max results", &pointerMaxResults_, 1000, 10000))
        pointerMaxResults_ = std::clamp(pointerMaxResults_, 1, 10000000);
    FlowSameLine(CheckboxWidth("Mapped memory"));
    ImGui::Checkbox("Mapped memory", &pointerMapped_);

    if (pointerRunning_) {
        const double total = pointerProgress_ ? static_cast<double>(pointerProgress_->total.load()) : 0.0;
        const double done = pointerProgress_ ? static_cast<double>(pointerProgress_->done.load()) : 0.0;
        const float fraction = total > 0 ? static_cast<float>(std::min(1.0, done / total)) : 0.0f;
        ImGui::ProgressBar(fraction, ImVec2(ImGui::GetContentRegionAvail().x - ButtonWidth("Cancel") - Px(8), 0),
                           fraction >= 1.0f ? "searching paths..." : nullptr);
        ImGui::SameLine();
        if (ImGui::Button("Cancel") && pointerCancel_) pointerCancel_->store(true);
    } else if (ImGui::Button("Start pointer scan")) {
        uint64_t target = 0;
        uint64_t offset = 0;
        lastModules_ = {};
        RefreshModules(context);
        if (!Resolve(context, pointerTarget_, target)) {
            pointerInfo_ = "Enter the address to find";
        } else if (!ParseHex(pointerOffset_, offset) || offset == 0 || offset > 0x100000) {
            pointerInfo_ = "The maximum offset must be between 1 and 100000 (hex)";
        } else {
            services::PointerScanOptions options;
            options.target = target;
            options.maxLevel = pointerLevel_;
            options.maxOffset = static_cast<uint32_t>(offset);
            options.maxResults = static_cast<size_t>(pointerMaxResults_);
            options.pointerSize = pointerSize;
            options.includeMapped = pointerMapped_;
            options.modules = CurrentPointerModules();
            pointerPending_ = std::make_shared<services::PointerScanResult>();
            pointerCancel_ = std::make_shared<std::atomic_bool>(false);
            pointerProgress_ = std::make_shared<services::ScanProgress>();
            auto pending = pointerPending_;
            auto cancel = pointerCancel_;
            auto progress = pointerProgress_;
            pointerRunning_ = true;
            pointerInfo_ = "Indexing pointers...";
            pointerFuture_ = std::async(std::launch::async, [session, options, pending, cancel, progress]() {
                std::string error;
                const bool ok = services::PointerScanner::Scan(session, options, *pending, &error, cancel.get(),
                                                               progress.get());
                return std::make_pair(ok, error);
            });
        }
    }
    if (!pointerRunning_) {
        FlowSameLine(Px(420));
        ImGui::SetNextItemWidth(Px(200));
        ImGui::InputTextWithHint("##PointerRescan", "new address after a restart", pointerRescan_, sizeof(pointerRescan_));
        ImGui::SameLine();
        ImGui::BeginDisabled(pointers_.paths.empty());
        if (ImGui::Button("Rescan")) {
            uint64_t target = 0;
            lastModules_ = {};
            RefreshModules(context);
            if (!Resolve(context, pointerRescan_, target)) {
                pointerInfo_ = "Enter the value's new address";
            } else {
                const size_t before = pointers_.paths.size();
                const size_t kept = services::PointerScanner::Rescan(session, pointers_, target, CurrentPointerModules());
                pointerInfo_ = std::to_string(kept) + " of " + std::to_string(before) + " path(s) still lead to " +
                               Hex(target);
                pointerLive_.clear();
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("After the game restarts, scan the value again and enter its new\n"
                              "address: only the paths that still reach it are kept.");
    }

    ImGui::SetNextItemWidth(std::max(Px(200), ImGui::GetContentRegionAvail().x - ButtonWidth("Save") -
                                                  ButtonWidth("Load") - Px(16)));
    ImGui::InputText("##PointerFile", pointerFile_, sizeof(pointerFile_));
    ImGui::SameLine();
    ImGui::BeginDisabled(pointers_.paths.empty());
    if (ImGui::Button("Save")) {
        std::error_code ignored;
        std::filesystem::create_directories(std::filesystem::u8path(pointerFile_).parent_path(), ignored);
        std::string error;
        pointerInfo_ = services::PointerScanner::Save(pointerFile_, pointers_, &error)
            ? "Saved " + std::to_string(pointers_.paths.size()) + " path(s)" : error;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        std::string error;
        services::PointerScanResult loaded;
        if (services::PointerScanner::Load(pointerFile_, loaded, &error)) {
            pointers_ = std::move(loaded);
            pointersLoaded_ = true;
            pointerLive_.clear();
            pointerInfo_ = "Loaded " + std::to_string(pointers_.paths.size()) +
                           " path(s); use Rescan with the value's current address to keep valid ones";
        } else {
            pointerInfo_ = error;
        }
    }
    if (!pointerInfo_.empty()) ImGui::TextDisabled("%s", pointerInfo_.c_str());

    if (pointers_.paths.empty()) {
        HintText("Scan a value first, then find the static chains of pointers that lead to its address. "
                 "They keep working after the game restarts.");
        return;
    }

    // Resolve the visible rows at most twice a second.
    const auto now = std::chrono::steady_clock::now();
    const bool refresh = now - lastPointerRefresh_ > std::chrono::milliseconds(500);
    if (refresh) lastPointerRefresh_ = now;
    const auto currentModules = CurrentPointerModules();

    if (!ToolTable("PointerTable", 3)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    ImGui::TableSetupColumn("Points to", ImGuiTableColumnFlags_WidthStretch, 0.22f);
    ImGui::TableSetupColumn("Value (4 bytes)", ImGuiTableColumnFlags_WidthStretch, 0.18f);
    ImGui::TableHeadersRow();
    if (pointerLive_.size() != pointers_.paths.size()) pointerLive_.assign(pointers_.paths.size(), ~0ull);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(pointers_.paths.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const auto& path = pointers_.paths[static_cast<size_t>(row)];
            uint64_t& resolved = pointerLive_[static_cast<size_t>(row)];
            if (refresh || resolved == ~0ull) {
                uint64_t address = 0;
                resolved = services::PointerScanner::Resolve(session, pointers_, path, currentModules, address)
                    ? address : 0;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(row);
            const std::string text = services::PointerScanner::Format(pointers_, path);
            ImGui::PushStyleColor(ImGuiCol_Text, StaticAddressColor());
            {
                MonoFont mono;
                if (ImGui::Selectable(text.c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    PendingAddressEntry entry;
                    entry.description = "Pointer " + text;
                    entry.module = pointers_.modules[path.module].name;
                    entry.baseOffset = path.baseOffset;
                    entry.offsets = path.offsets;
                    entry.pointerSize = pointers_.pointerSize;
                    context.pendingAddresses.push_back(std::move(entry));
                    context.status = "Pointer added to the Memory address list";
                }
            }
            ImGui::PopStyleColor();
            if (ImGui::BeginPopupContextItem("PointerMenu")) {
                if (ImGui::MenuItem("Add to the address list")) {
                    PendingAddressEntry entry;
                    entry.description = "Pointer " + text;
                    entry.module = pointers_.modules[path.module].name;
                    entry.baseOffset = path.baseOffset;
                    entry.offsets = path.offsets;
                    entry.pointerSize = pointers_.pointerSize;
                    context.pendingAddresses.push_back(std::move(entry));
                    context.status = "Pointer added to the Memory address list";
                }
                if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(text.c_str());
                ImGui::BeginDisabled(resolved == 0);
                if (ImGui::MenuItem("Browse the address")) context.NavigateTo("memory-browser", resolved);
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            if (resolved == 0) {
                ImGui::TextDisabled("??");
            } else {
                const bool same = resolved == pointers_.target;
                MonoText("%s", Hex(resolved).c_str());
                if (!same && ImGui::IsItemHovered()) ImGui::SetTooltip("No longer the scanned address");
            }
            ImGui::TableSetColumnIndex(2);
            int32_t value = 0;
            if (resolved && session->ReadMemory(resolved, &value, sizeof(value), nullptr)) ImGui::Text("%d", value);
            else ImGui::TextDisabled("??");
        }
    }
    ImGui::EndTable();
}

// ------------------------------------------------------------------ symbols

// User-defined symbols (Cheat Engine's registerSymbol) and the exports of
// every module: all of them work in any address field.
void ToolsWorkspace::DrawSymbols(UiContext& context) {
    HintText("Names registered here work in every address field, in cheat tables and in Lua "
             "(registerSymbol). Module exports resolve as module.Export or just Export.");
    ImGui::SetNextItemWidth(Px(150));
    ImGui::InputTextWithHint("##SymbolName", "name", symbolName_, sizeof(symbolName_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(260));
    const bool enter = ImGui::InputTextWithHint("##SymbolAddress", "address or expression", symbolAddress_,
                                                sizeof(symbolAddress_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Register") || enter) {
        const std::string name = Trim(symbolName_);
        uint64_t address = 0;
        std::string error;
        if (name.empty() || name.find_first_of(" +-*[]()\"'") != std::string::npos) {
            context.status = "Enter a symbol name without spaces or operators";
        } else if (!EvaluateContextAddress(context, symbolAddress_, address, &error)) {
            context.status = "Cannot resolve the address: " + error;
        } else {
            context.userSymbols->Set(name, address);
            context.status = "Registered " + name + " = " + Hex(address);
            symbolName_[0] = '\0';
            symbolAddress_[0] = '\0';
        }
    }

    const auto symbols = context.userSymbols->List();
    ImGui::SeparatorText(("Registered symbols (" + std::to_string(symbols.size()) + ")").c_str());
    if (symbols.empty()) {
        ImGui::TextDisabled("None yet.");
    } else if (BeginDataTable("UserSymbols", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable,
                              ImVec2(0, std::min(Px(200), ImGui::GetFrameHeightWithSpacing() * (static_cast<float>(symbols.size()) + 1.5f))))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ButtonWidth("Remove"));
        ImGui::TableHeadersRow();
        for (const auto& symbol : symbols) {
            ImGui::PushID(symbol.first.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(symbol.first.c_str());
            ImGui::TableSetColumnIndex(1);
            {
                MonoFont mono;
                ImGui::Selectable((AddressText(symbol.second) + "##Address").c_str());
            }
            AddressContextOptions options;
            options.label = symbol.first;
            if (ImGui::BeginPopupContextItem("SymbolMenu")) {
                DrawAddressContextActions(context, symbol.second, options);
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("Remove")) context.userSymbols->Remove(symbol.first);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Module exports");
    ImGui::SetNextItemWidth(Px(260));
    const bool search = ImGui::InputTextWithHint("##SymbolSearch", "export name contains...", symbolSearch_,
                                                 sizeof(symbolSearch_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Search") || search) {
        symbolSearched_ = Trim(symbolSearch_);
        symbolMatches_ = ContextSymbols(context, true).Search(symbolSearched_, 2000);
        context.status = std::to_string(symbolMatches_.size()) + " export(s) found" +
                         (symbolMatches_.size() >= 2000 ? " (first 2000)" : "");
    }
    if (symbolMatches_.empty()) {
        if (!symbolSearched_.empty()) ImGui::TextDisabled("No export matches.");
        return;
    }
    if (BeginDataTable("ExportMatches", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                       ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(symbolMatches_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& match = symbolMatches_[static_cast<size_t>(row)];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(match.first.c_str());
                ImGui::TableSetColumnIndex(1);
                {
                    MonoFont mono;
                    ImGui::Selectable((Hex(match.second) + "##Export").c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                }
                AddressContextOptions options;
                options.label = match.first;
                if (ImGui::BeginPopupContextItem("ExportMenu")) {
                    if (ImGui::MenuItem("Copy name")) ImGui::SetClipboardText(match.first.c_str());
                    ImGui::Separator();
                    DrawAddressContextActions(context, match.second, options);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ assembler

// Whole instructions covering at least `minimum` bytes, so the trampoline
// does not cut an instruction in half.
int ToolsWorkspace::StealLength(UiContext& context, uint64_t address, int minimum) const {
    if (!context.disassembly) return minimum;
    std::vector<services::DisassemblyInstruction> instructions;
    std::string error;
    if (!context.disassembly->Decode(address, 12, instructions, &error)) return minimum;
    int length = 0;
    for (const auto& instruction : instructions) {
        length += static_cast<int>(instruction.bytes.size());
        if (length >= minimum) break;
    }
    return length >= minimum ? length : minimum;
}

bool ToolsWorkspace::AssembleCurrent(UiContext& context, uint64_t address,
                                     services::AssembleBlockResult& result, std::string& error) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    services::AssembleRequest request;
    request.text = assemblerSource_.data();
    request.address = address;
    request.x64 = !session || session->Target().architecture != target::Architecture::X86;
    request.evaluate = [&context](const std::string& text, uint64_t& value, std::string& message) {
        if (EvaluateContextAddress(context, text, value, &message)) return true;
        if (message.empty()) message = "Unknown symbol: " + text;
        return false;
    };
    return services::AssembleBlock(request, result, &error);
}

// A classic trampoline: a jmp at the site into a cave that runs the new
// code, the original instructions, then jumps back after them.
bool ToolsWorkspace::InjectCode(UiContext& context, uint64_t site, std::string& error) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.memory) {
        error = "No target";
        return false;
    }
    const uint64_t pid = session->Target().processId;
    const bool x64 = session->Target().architecture != target::Architecture::X86;

    auto assembleLine = [&](const std::string& text, uint64_t at, std::vector<uint8_t>& bytes) {
        services::AssembleRequest request;
        request.text = text;
        request.address = at;
        request.x64 = x64;
        request.evaluate = [&context](const std::string& name, uint64_t& value, std::string& message) {
            return EvaluateContextAddress(context, name, value, &message);
        };
        std::string message;
        return services::AssembleLine(request, bytes, &message);
    };

    // A cave big enough for the new code, the stolen bytes and a jump back.
    const size_t sourceLength = std::strlen(assemblerSource_.data());
    const size_t caveSize = std::max<size_t>(512, sourceLength * 2) + 96;
    uint64_t cave = 0;
    if (!cortex::remote_memory::Allocate(pid, caveSize, site, cave, &error)) return false;

    auto bail = [&](const std::string& message) {
        cortex::remote_memory::Free(pid, cave, nullptr);
        error = message;
        return false;
    };

    // The jump that will sit at the site tells us how many bytes to steal.
    std::vector<uint8_t> siteJump;
    {
        char target[32] = {};
        std::snprintf(target, sizeof(target), "%llX", static_cast<unsigned long long>(cave));
        if (!assembleLine(std::string("jmp ") + target, site, siteJump)) return bail("Cannot encode the jump to the cave");
    }
    int steal = std::max<int>(injectSteal_, static_cast<int>(siteJump.size()));
    steal = StealLength(context, site, steal);

    std::vector<uint8_t> original;
    std::string readError;
    if (!context.memory->Read(site, static_cast<size_t>(steal), original, &readError) ||
        original.size() != static_cast<size_t>(steal))
        return bail("Cannot read the original bytes: " + readError);

    // The cave: new code, the original instructions, a jump back.
    std::vector<uint8_t> caveBytes;
    if (sourceLength) {
        services::AssembleBlockResult result;
        std::string assembleError;
        services::AssembleRequest request;
        request.text = assemblerSource_.data();
        request.address = cave;
        request.x64 = x64;
        request.evaluate = [&context](const std::string& name, uint64_t& value, std::string& message) {
            return EvaluateContextAddress(context, name, value, &message);
        };
        if (!services::AssembleBlock(request, result, &assembleError)) return bail(assembleError);
        caveBytes = std::move(result.bytes);
    }
    // Relocate the replaced instructions so their relative branches and
    // RIP-relative operands still reach the same targets from the cave.
    std::vector<uint8_t> relocated;
    std::string relocateError;
    if (!services::RelocateCode(original.data(), original.size(), site, cave + caveBytes.size(), x64, relocated,
                                &relocateError))
        return bail("Cannot relocate the replaced code: " + relocateError);
    caveBytes.insert(caveBytes.end(), relocated.begin(), relocated.end());
    std::vector<uint8_t> jumpBack;
    char back[32] = {};
    std::snprintf(back, sizeof(back), "%llX", static_cast<unsigned long long>(site + steal));
    if (!assembleLine(std::string("jmp ") + back, cave + caveBytes.size(), jumpBack))
        return bail("Cannot encode the jump back");
    caveBytes.insert(caveBytes.end(), jumpBack.begin(), jumpBack.end());
    if (caveBytes.size() > caveSize) return bail("The code is larger than the cave");

    if (!cortex::remote_memory::WriteCode(pid, cave, caveBytes.data(), caveBytes.size(), &error))
        return bail("Cannot write the cave: " + error);

    // The site: the jump to the cave, padded with NOPs to the steal length.
    std::vector<uint8_t> patch = siteJump;
    patch.resize(static_cast<size_t>(steal), 0x90);
    if (!cortex::remote_memory::WriteCode(pid, site, patch.data(), patch.size(), &error))
        return bail("Cannot patch the site: " + error);

    injections_.push_back({site, cave, caveSize, original, std::string(assemblerAddress_)});
    assemblerError_ = false;
    assemblerInfo_ = "Injected at " + Hex(site) + ": " + std::to_string(steal) + " byte(s) replaced, cave at " +
                     Hex(cave);
    return true;
}

void ToolsWorkspace::DrawAssembler(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    const bool x64 = !session || session->Target().architecture != target::Architecture::X86;
    const uint64_t pid = session ? session->Target().processId : 0;

    ImGui::RadioButton("Assemble && write", &assemblerMode_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Code injection", &assemblerMode_, 1);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", x64 ? "x64" : "x86");

    ImGui::TextDisabled(assemblerMode_ == 0 ? "Write bytes at" : "Inject at");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(260));
    ImGui::InputTextWithHint("##AsmAddress", "game.exe+1234 or an expression", assemblerAddress_,
                             sizeof(assemblerAddress_));
    uint64_t address = 0;
    const bool haveAddress = *assemblerAddress_ && Resolve(context, assemblerAddress_, address);
    ImGui::SameLine();
    if (haveAddress) {
        MonoText("= %s", AddressText(address).c_str());
    } else if (*assemblerAddress_) {
        ImGui::TextColored(WarningTextColor(), "unresolved");
    } else {
        ImGui::TextDisabled("enter an address");
    }

    if (assemblerMode_ == 1) {
        ImGui::SetNextItemWidth(Px(120));
        if (ImGui::InputInt("Bytes to replace", &injectSteal_)) injectSteal_ = std::clamp(injectSteal_, 5, 64);
        ImGui::SameLine();
        if (ImGui::SmallButton("Auto") && haveAddress) injectSteal_ = StealLength(context, address, x64 ? 5 : 5);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Round up to whole instructions at the address");
    }

    HintText(assemblerMode_ == 0
                 ? "Intel syntax, one instruction per line. Numbers are hex (10 = 0x10, #16 decimal). "
                   "Labels end in ':'. Module names, exports and symbols resolve as addresses."
                 : "The code runs in a cave Cortex allocates near the address. Original instructions are "
                   "preserved and control returns after them. Relative operands in the replaced bytes may "
                   "need adjusting.");

    const float editorHeight = std::max(Px(140.0f), ImGui::GetContentRegionAvail().y - Px(150.0f));
    {
        MonoFont mono;
        ImGui::InputTextMultiline("##AsmSource", assemblerSource_.data(), assemblerSource_.size(),
                                  ImVec2(-1, editorHeight), ImGuiInputTextFlags_AllowTabInput);
    }

    const bool writes = context.mutationAllowed;
    if (assemblerMode_ == 0) {
        if (ImGui::Button("Assemble", ImVec2(Px(120), 0)) && haveAddress) {
            services::AssembleBlockResult result;
            std::string error;
            assemblerError_ = !AssembleCurrent(context, address, result, error);
            if (assemblerError_) {
                assembledBytes_.clear();
                assemblerInfo_ = error;
            } else {
                assembledBytes_ = result.bytes;
                assembledAt_ = address;
                assemblerInfo_ = std::to_string(result.bytes.size()) + " byte(s) assembled at " + Hex(address);
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!writes || assembledBytes_.empty() || !haveAddress || address != assembledAt_);
        if (ImGui::Button("Write to target", ImVec2(Px(150), 0))) {
            std::string error;
            const bool ok = cortex::remote_memory::WriteCode(pid, address, assembledBytes_.data(),
                                                             assembledBytes_.size(), &error);
            assemblerError_ = !ok;
            assemblerInfo_ = ok ? "Wrote " + std::to_string(assembledBytes_.size()) + " byte(s) to " + Hex(address)
                                : "Write failed: " + error;
            if (ok) context.status = assemblerInfo_;
        }
        ImGui::EndDisabled();
        if (!writes) {
            ImGui::SameLine();
            ImGui::TextDisabled("Allow writes to patch the target");
        }
    } else {
        ImGui::BeginDisabled(!writes || !haveAddress);
        if (ImGui::Button("Inject", ImVec2(Px(120), 0))) {
            std::string error;
            if (InjectCode(context, address, error)) {
                context.status = assemblerInfo_;
            } else {
                assemblerError_ = true;
                assemblerInfo_ = error;
            }
        }
        ImGui::EndDisabled();
        if (!writes) {
            ImGui::SameLine();
            ImGui::TextDisabled("Allow writes to inject");
        }
    }

    if (!assemblerInfo_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, assemblerError_ ? ChangedValueColor() : StaticAddressColor());
        ImGui::TextWrapped("%s", assemblerInfo_.c_str());
        ImGui::PopStyleColor();
        if (!assemblerError_ && !assembledBytes_.empty() && assemblerMode_ == 0) {
            MonoFont mono;
            std::string hex;
            char byte[4] = {};
            for (size_t i = 0; i < assembledBytes_.size() && i < 64; ++i) {
                std::snprintf(byte, sizeof(byte), "%02X ", assembledBytes_[i]);
                hex += byte;
            }
            if (assembledBytes_.size() > 64) hex += "...";
            ImGui::TextWrapped("%s", hex.c_str());
        }
    }

    // Active injections, with Restore.
    if (!injections_.empty()) {
        ImGui::SeparatorText(("Active injections (" + std::to_string(injections_.size()) + ")").c_str());
        for (size_t i = 0; i < injections_.size();) {
            auto& injection = injections_[i];
            ImGui::PushID(static_cast<int>(i));
            MonoText("%s -> cave %s (%zu bytes)", Hex(injection.site).c_str(), Hex(injection.cave).c_str(),
                     injection.original.size());
            ImGui::SameLine();
            ImGui::BeginDisabled(!writes);
            bool removed = false;
            if (ImGui::SmallButton("Restore")) {
                std::string error;
                const bool ok = cortex::remote_memory::WriteCode(pid, injection.site, injection.original.data(),
                                                                injection.original.size(), &error);
                if (ok) cortex::remote_memory::Free(pid, injection.cave, nullptr);
                context.status = ok ? "Injection at " + Hex(injection.site) + " restored"
                                    : "Restore failed: " + error;
                removed = ok;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            if (removed) injections_.erase(injections_.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
    }
}

// ------------------------------------------------------------------ grouped scan

void ToolsWorkspace::DrawGroupedScan(UiContext& context) {
    HintText("Finds several values that sit close together, which is how you locate a structure from the "
             "few fields you know. Prefix an element with its type: 4:64 f:1.5 2:14. A bare * skips one "
             "byte and 4:* a field whose value you do not know. Numbers are hexadecimal; #100 is decimal.");

    ImGui::SetNextItemWidth(-1);
    const bool submitted = ImGui::InputTextWithHint("##GroupedText", "4:64 f:1.5 2:14", groupedText_,
                                                    sizeof(groupedText_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth(Px(120));
    if (ImGui::InputInt("Window", &groupedWindow_)) groupedWindow_ = std::clamp(groupedWindow_, 4, 4096);
    FlowSameLine(Px(150));
    static const char* const kSizes[] = {"1 byte", "2 bytes", "4 bytes", "8 bytes"};
    int sizeIndex = groupedDefaultSize_ == 1 ? 0 : groupedDefaultSize_ == 2 ? 1 : groupedDefaultSize_ == 8 ? 3 : 2;
    ImGui::SetNextItemWidth(Px(120));
    if (ImGui::Combo("Default", &sizeIndex, kSizes, 4))
        groupedDefaultSize_ = sizeIndex == 0 ? 1 : sizeIndex == 1 ? 2 : sizeIndex == 3 ? 8 : 4;
    FlowSameLine(CheckboxWidth("In order"));
    ImGui::Checkbox("In order", &groupedOrdered_);
    FlowSameLine(CheckboxWidth("Writable memory only"));
    ImGui::Checkbox("Writable memory only", &groupedWritableOnly_);
    FlowSameLine(ButtonWidth("Scan"));
    const bool scan = ImGui::Button("Scan") || submitted;

    if (scan) {
        std::string parseError;
        if (!services::ParseGroupedScan(groupedText_, static_cast<size_t>(groupedDefaultSize_), groupedElements_,
                                        &parseError)) {
            groupedInfo_ = parseError;
            grouped_.clear();
        } else {
            const auto elements = groupedElements_;
            const size_t window = static_cast<size_t>(groupedWindow_);
            const bool ordered = groupedOrdered_;
            const bool writableOnly = groupedWritableOnly_;
            context.RunInBackground("Grouped scan", [this, &context, elements, window, ordered, writableOnly]() {
                grouped_.clear();
                const auto session = context.sessions ? context.sessions->Active() : nullptr;
                if (!session) {
                    groupedInfo_ = "Select a process first";
                    return;
                }
                constexpr size_t kMaxResults = 5000;
                constexpr uint64_t kMaxBytes = 512ull * 1024 * 1024;
                uint64_t scanned = 0;
                std::vector<uint8_t> buffer;
                for (const auto& region : session->MemoryRegions()) {
                    if (!region.readable) continue;
                    if (writableOnly && !region.writable) continue;
                    if (grouped_.size() >= kMaxResults || scanned >= kMaxBytes) break;
                    const size_t size = static_cast<size_t>(std::min<uint64_t>(region.size, kMaxBytes - scanned));
                    buffer.assign(size, 0);
                    if (!session->ReadMemory(region.base, buffer.data(), size, nullptr)) continue;
                    scanned += size;
                    std::vector<services::GroupedHit> hits;
                    services::FindGroupedValues(buffer.data(), size, region.base, elements, window, ordered,
                                                kMaxResults - grouped_.size(), hits);
                    grouped_.insert(grouped_.end(), hits.begin(), hits.end());
                }
                groupedInfo_ = std::to_string(grouped_.size()) + " match(es) in " +
                               std::to_string(scanned / (1024 * 1024)) + " MB" +
                               (grouped_.size() >= kMaxResults ? " (limit reached)" : "");
            });
        }
    }
    if (!groupedInfo_.empty()) ImGui::TextDisabled("%s", groupedInfo_.c_str());
    if (grouped_.empty()) return;

    if (BeginDataTable("GroupedResults", 3,
                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_Resizable,
                       ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("Offsets", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(grouped_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& hit = grouped_[static_cast<size_t>(row)];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                {
                    MonoFont mono;
                    ImGui::Selectable((Hex(hit.address) + "##Grouped").c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns);
                }
                if (ImGui::BeginPopupContextItem("GroupedMenu")) {
                    if (ImGui::MenuItem("Add to the address list")) {
                        PendingAddressEntry entry;
                        entry.address = hit.address;
                        entry.description = "Grouped scan";
                        entry.type = services::ScanDataType::Int32;
                        context.pendingAddresses.push_back(entry);
                        context.requestWorkspace = "memory";
                    }
                    ImGui::Separator();
                    AddressContextOptions options;
                    options.label = "Grouped scan";
                    DrawAddressContextActions(context, hit.address, options);
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(1);
                std::string offsets;
                for (const auto offset : hit.offsets) {
                    char buffer[16] = {};
                    std::snprintf(buffer, sizeof(buffer), "%s+%llX", offsets.empty() ? "" : " ",
                                  static_cast<unsigned long long>(offset));
                    offsets += buffer;
                }
                MonoTextUnformatted(offsets.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(AddressText(hit.address).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ speedhack

services::SpeedhackHost ToolsWorkspace::SpeedhackHostFor(UiContext& context, uint64_t pid) const {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    services::SpeedhackHost host;
    host.read = [session](uint64_t address, void* buffer, size_t size) {
        return session && session->ReadMemory(address, buffer, size, nullptr);
    };
    host.write = [pid](uint64_t address, const void* buffer, size_t size) {
        return cortex::remote_memory::WriteCode(pid, address, buffer, size, nullptr);
    };
    host.allocate = [pid](size_t size, uint64_t nearAddress, uint64_t& address, std::string& error) {
        return cortex::remote_memory::Allocate(pid, size, nearAddress, address, &error);
    };
    host.release = [pid](uint64_t address, std::string& error) {
        return cortex::remote_memory::Free(pid, address, &error);
    };
    host.symbol = [&context](const std::string& name, uint64_t& value) {
        return ContextSymbols(context).Resolve(name, value);
    };
    return host;
}

void ToolsWorkspace::DrawSpeedhack(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    const uint64_t pid = session ? session->Target().processId : 0;
    const bool x64 = !session || session->Target().architecture != target::Architecture::X86;

    HintText("Hooks the target's timing functions so the game runs faster or slower. The clock is scaled "
             "from the moment it is turned on, so it never jumps backwards.");

    ImGui::SetNextItemWidth(Px(320));
    bool changed = ImGui::SliderFloat("##Speed", &speedValue_, 0.05f, 25.0f, "%.2fx",
                                      ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::TextDisabled("speed");
    for (const float preset : {0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f}) {
        char label[16] = {};
        std::snprintf(label, sizeof(label), "%gx", preset);
        FlowSameLine(ButtonWidth(label));
        if (ImGui::Button(label)) {
            speedValue_ = preset;
            changed = true;
        }
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!context.mutationAllowed || !session);
    if (!speed_.active) {
        if (ImGui::Button("Enable speedhack", ImVec2(Px(180), Px(32)))) {
            std::string error;
            const auto host = SpeedhackHostFor(context, pid);
            if (services::InstallSpeedhack(host, x64, speedValue_, speed_, &error)) {
                speedPid_ = pid;
                speedInfo_ = "Speed x" + std::to_string(speed_.multiplier).substr(0, 5) + " on " +
                             std::to_string(speed_.hooks.size()) + " function(s)";
                context.status = speedInfo_;
            } else {
                speedInfo_ = "Speedhack failed: " + error;
            }
        }
    } else {
        if (ImGui::Button("Disable speedhack", ImVec2(Px(180), Px(32)))) {
            std::string error;
            const auto host = SpeedhackHostFor(context, speedPid_);
            speedInfo_ = services::RemoveSpeedhack(host, speed_, &error) ? "Speed back to normal"
                                                                        : "Restore failed: " + error;
            speedValue_ = 1.0f;
            context.status = speedInfo_;
        }
    }
    ImGui::EndDisabled();
    if (!context.mutationAllowed) {
        ImGui::SameLine();
        ImGui::TextDisabled("Allow writes to change the speed");
    }

    // Moving the slider while it runs only rewrites the multiplier.
    if (changed && speed_.active && context.mutationAllowed) {
        std::string error;
        const auto host = SpeedhackHostFor(context, speedPid_);
        if (!services::UpdateSpeedhack(host, speed_, speedValue_, &error)) speedInfo_ = "Update failed: " + error;
    }

    if (!speedInfo_.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", speedInfo_.c_str());
    }

    if (!speed_.hooks.empty()) {
        ImGui::SeparatorText("Hooked");
        for (const auto& hook : speed_.hooks) MonoText("%s  %s", hook.function.c_str(), AddressText(hook.address).c_str());
    }
    if (!speed_.skipped.empty()) {
        ImGui::SeparatorText("Not hooked");
        for (const auto& skipped : speed_.skipped) ImGui::TextWrapped("%s", skipped.c_str());
    }
}

// ------------------------------------------------------------------ frame

void ToolsWorkspace::Draw(UiContext& context) {
    PollPointerScan(context);
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session) {
        HintText("Select a process to use the memory tools.");
        return;
    }
    if (session->Target().id != targetId_) {
        // Leaving a target: put its timing functions back first.
        if (speed_.active) {
            std::string error;
            const auto host = SpeedhackHostFor(context, speedPid_);
            services::RemoveSpeedhack(host, speed_, &error);
            speedValue_ = 1.0f;
            speedInfo_.clear();
        }
        Reset(session->Target().id);
    }
    RefreshModules(context);

    uint64_t address = 0;
    if (context.ConsumeNavigation("tools", address)) {
        if (address) {
            std::snprintf(signatureAddress_, sizeof(signatureAddress_), "%s", Hex(address).c_str());
            std::snprintf(pointerTarget_, sizeof(pointerTarget_), "%s", Hex(address).c_str());
        }
        requested_ = context.toolsTabRequest == "pointers" ? Tab::Pointers
                   : context.toolsTabRequest == "signature" ? Tab::Signature
                   : context.toolsTabRequest == "pe" ? Tab::Pe
                   : context.toolsTabRequest == "symbols" ? Tab::Symbols
                   : context.toolsTabRequest == "assembler" ? Tab::Assembler
                   : context.toolsTabRequest == "speedhack" ? Tab::Speed
                   : context.toolsTabRequest == "grouped" ? Tab::Grouped
                   : Tab::Regions;
        tabPending_ = true;
        context.toolsTabRequest.clear();
    }

    // A tab bar would scroll once there are this many tools, hiding the last
    // ones behind arrows, so the selector wraps onto as many rows as it needs.
    struct Entry {
        const char* label;
        Tab tab;
    };
    static const Entry kTools[] = {
        {"Regions", Tab::Regions},          {"PE headers", Tab::Pe},
        {"Strings", Tab::Strings},          {"Code caves", Tab::Caves},
        {"AOB signature", Tab::Signature},  {"Pointer scan", Tab::Pointers},
        {"Symbols", Tab::Symbols},          {"Assembler", Tab::Assembler},
        {"Grouped scan", Tab::Grouped},     {"Speedhack", Tab::Speed},
    };
    if (tabPending_) {
        active_ = requested_;
        tabPending_ = false;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float available = ImGui::GetContentRegionAvail().x;
    float row = 0.0f;
    for (size_t index = 0; index < sizeof(kTools) / sizeof(kTools[0]); ++index) {
        const Entry& entry = kTools[index];
        const float width = ImGui::CalcTextSize(entry.label).x + style.FramePadding.x * 2.0f;
        if (index && row + style.ItemSpacing.x + width <= available) {
            ImGui::SameLine();
            row += style.ItemSpacing.x + width;
        } else {
            row = width;
        }
        const bool selected = active_ == entry.tab;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
        if (ImGui::Button(entry.label)) active_ = entry.tab;
        if (selected) ImGui::PopStyleColor();
    }
    ImGui::Separator();

    switch (active_) {
        case Tab::Regions: DrawRegions(context); break;
        case Tab::Pe: DrawPe(context); break;
        case Tab::Strings: DrawStrings(context); break;
        case Tab::Caves: DrawCaves(context); break;
        case Tab::Signature: DrawSignature(context); break;
        case Tab::Pointers: DrawPointers(context); break;
        case Tab::Symbols: DrawSymbols(context); break;
        case Tab::Assembler: DrawAssembler(context); break;
        case Tab::Grouped: DrawGroupedScan(context); break;
        case Tab::Speed: DrawSpeedhack(context); break;
    }
}

} // namespace cortex::ui
