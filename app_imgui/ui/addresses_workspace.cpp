#include "addresses_workspace.h"
#include "address_context_menu.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>

namespace cortex::ui {
namespace {

constexpr const char* kTypes[] = {
    "i32", "float", "double", "i8", "u8", "i16", "u16", "u32", "i64", "u64"
};

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string Trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool ParseNumber(const std::string& text, uint64_t& value, int defaultBase = 0) {
    const std::string token = Trim(text);
    if (token.empty()) return false;
    try {
        size_t used = 0;
        int base = defaultBase;
        if (base == 0) {
            const bool hexAlpha = std::any_of(token.begin(), token.end(), [](unsigned char ch) {
                ch = static_cast<unsigned char>(std::tolower(ch));
                return ch >= 'a' && ch <= 'f';
            });
            if (hexAlpha) base = 16;
        }
        value = std::stoull(token, &used, base);
        return used == token.size();
    } catch (...) {
        return false;
    }
}

void Copy(char* destination, size_t capacity, const std::string& source) {
    if (!destination || capacity == 0) return;
    std::memset(destination, 0, capacity);
    std::memcpy(destination, source.data(), std::min(capacity - 1, source.size()));
}

std::string NormalizedType(const application::ProjectAddress& row) {
    const std::string type = Lower(row.type);
    for (const char* supported : kTypes)
        if (type == supported) return type;
    return "i32";
}

int TypeIndex(const std::string& rawType) {
    const std::string type = Lower(rawType);
    for (int i = 0; i < IM_ARRAYSIZE(kTypes); ++i)
        if (type == kTypes[i]) return i;
    return 0;
}

int TypeSize(const std::string& type) {
    if (type == "i8" || type == "u8") return 1;
    if (type == "i16" || type == "u16") return 2;
    if (type == "i64" || type == "u64" || type == "double") return 8;
    return 4;
}

const application::RuntimeWatch* FindWatch(
        const UiContext& context, const application::ProjectAddress& row) {
    if (!context.watchesModel) return nullptr;
    const std::string wantedAddress = Lower(Trim(row.address));
    for (const auto& watch : context.watchesModel->Watches()) {
        if ((!row.name.empty() && watch.label == row.name) ||
            Lower(Trim(watch.address)) == wantedAddress)
            return &watch;
    }
    return nullptr;
}

const application::RuntimeFreeze* FindFreeze(
        const UiContext& context, const application::ProjectAddress& row) {
    if (!context.watchesModel) return nullptr;
    const std::string wantedAddress = Lower(Trim(row.address));
    for (const auto& freeze : context.watchesModel->Freezes()) {
        if ((!row.name.empty() && freeze.label == row.name) ||
            Lower(Trim(freeze.address)) == wantedAddress)
            return &freeze;
    }
    return nullptr;
}

} // namespace

bool AddressesWorkspace::RefreshAll(UiContext& context, bool reportStatus) {
    if (!context.projectModel || !context.watchesModel) return false;

    std::string projectError;
    std::string watchError;
    const bool projectOk = context.projectModel->Refresh(&projectError);
    const bool watchesOk = context.watchesModel->Refresh(&watchError);
    lastWatchRefresh_ = std::chrono::steady_clock::now();
    SyncSelection();

    if (reportStatus) {
        if (!projectOk) context.status = "Address refresh failed: " + projectError;
        else if (!watchesOk) context.status = "Watch refresh failed: " + watchError;
        else context.status =
            std::to_string(context.projectModel->Addresses().size()) + " saved address(es)";
    }
    return projectOk && watchesOk;
}

bool AddressesWorkspace::ResolveExpression(
        UiContext& context, const std::string& rawExpression,
        uint64_t& address) const {
    const std::string expression = Trim(rawExpression);
    if (ParseNumber(expression, address)) return true;

    const size_t plus = expression.find_last_of('+');
    if (plus == std::string::npos || !context.modules) return false;

    const std::string moduleName = Lower(Trim(expression.substr(0, plus)));
    uint64_t offset = 0;
    if (!ParseNumber(expression.substr(plus + 1), offset, 16)) return false;

    std::string error;
    const auto modules = context.modules->List(&error);
    for (const auto& module : modules) {
        if (Lower(module.name) != moduleName) continue;
        if (offset > std::numeric_limits<uint64_t>::max() - module.base) return false;
        address = module.base + offset;
        return true;
    }
    return false;
}

bool AddressesWorkspace::Navigate(
        UiContext& context, const std::string& expression,
        const char* workspace) const {
    uint64_t address = 0;
    if (!ResolveExpression(context, expression, address)) {
        context.status = "Address is not directly resolvable: " + expression;
        return false;
    }
    context.NavigateTo(workspace ? workspace : "memory-browser", address);
    return true;
}

void AddressesWorkspace::ClearEditor() {
    name_.fill(0);
    address_.fill(0);
    notes_.fill(0);
    typeIndex_ = 0;
    editingOriginalName_.clear();
}

void AddressesWorkspace::BeginEdit(const application::ProjectAddress& row) {
    Copy(name_.data(), name_.size(), row.name);
    Copy(address_.data(), address_.size(), row.address);
    Copy(notes_.data(), notes_.size(), row.notes);
    typeIndex_ = TypeIndex(row.type);
    editingOriginalName_ = row.name;
}

bool AddressesWorkspace::SaveEditor(UiContext& context) {
    if (!context.projectModel || !context.mutationAllowed) return false;
    const std::string newName = Trim(name_.data());
    const std::string expression = Trim(address_.data());
    if (newName.empty() || expression.empty()) {
        context.status = "Address requires a description and expression";
        return false;
    }

    std::string error;
    if (!context.projectModel->SetAddress(
            newName, expression, kTypes[typeIndex_], Trim(notes_.data()),
            context.mutationAllowed, &error)) {
        context.status = "Save address failed: " + error;
        return false;
    }

    if (!editingOriginalName_.empty() && editingOriginalName_ != newName) {
        std::string deleteError;
        if (!context.projectModel->DeleteAddress(
                editingOriginalName_, context.mutationAllowed, &deleteError)) {
            context.status = "Address renamed, old entry cleanup failed: " + deleteError;
            return false;
        }
    }

    context.status = editingOriginalName_.empty() ? "Address added" : "Address updated";
    selectedIndex_ = -1;
    ClearEditor();
    return true;
}

bool AddressesWorkspace::RemoveSelected(UiContext& context) {
    if (!context.projectModel || !context.mutationAllowed) return false;
    const auto& rows = context.projectModel->Addresses();
    if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(rows.size())) return false;

    const std::string name = rows[static_cast<size_t>(selectedIndex_)].name;
    std::string error;
    if (!context.projectModel->DeleteAddress(name, context.mutationAllowed, &error)) {
        context.status = "Delete address failed: " + error;
        return false;
    }
    if (editingOriginalName_ == name) ClearEditor();
    selectedIndex_ = -1;
    context.status = "Address removed";
    return true;
}

void AddressesWorkspace::SyncSelection() {
    if (selectedIndex_ < 0) return;
    selectedIndex_ = -1;
}

void AddressesWorkspace::HandleNavigation(UiContext& context, uint64_t address) {
    if (!context.projectModel) return;

    const auto& rows = context.projectModel->Addresses();
    for (size_t i = 0; i < rows.size(); ++i) {
        uint64_t resolved = 0;
        if (ResolveExpression(context, rows[i].address, resolved) && resolved == address) {
            selectedIndex_ = static_cast<int>(i);
            ClearEditor();
            return;
        }
    }

    selectedIndex_ = -1;
    ClearEditor();
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX",
                  static_cast<unsigned long long>(address));
    Copy(address_.data(), address_.size(), buffer);
}

void AddressesWorkspace::HandleShortcuts(UiContext& context) {
    if (selectedIndex_ < 0 || ImGui::GetIO().WantTextInput || !context.projectModel) return;
    const auto& rows = context.projectModel->Addresses();
    if (selectedIndex_ >= static_cast<int>(rows.size())) return;
    const auto& row = rows[static_cast<size_t>(selectedIndex_)];

    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) BeginEdit(row);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && context.mutationAllowed)
        RemoveSelected(context);

    if (ImGui::IsKeyPressed(ImGuiKey_B, false) &&
        ImGui::GetIO().KeyCtrl && context.mutationAllowed && context.debuggerModel) {
        std::string error;
        const bool pause =
            context.settings &&
            context.settings->Values().breakpointDefaultAction == "pause";
        if (!context.debuggerModel->AddBreakpoint(
                row.address, "software", 1, pause, true, 0, &error))
            context.status = "Add breakpoint failed: " + error;
        else
            context.status = "Breakpoint added";
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) &&
        context.mutationAllowed && context.watchesModel) {
        const auto* freeze = FindFreeze(context, row);
        const auto* watch = FindWatch(context, row);
        std::string error;
        if (freeze) {
            if (!context.watchesModel->DeleteFreeze(
                    freeze->id, context.mutationAllowed, &error))
                context.status = "Unfreeze failed: " + error;
            else
                context.status = "Freeze removed";
        } else if (watch && watch->hasValue) {
            if (!context.watchesModel->AddFreeze(
                    row.address, NormalizedType(row), watch->value, row.name, 0,
                    context.mutationAllowed, &error))
                context.status = "Freeze failed: " + error;
            else
                context.status = "Value frozen";
        }
    }
}

void AddressesWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.projectModel || !context.watchesModel) {
        ImGui::TextDisabled("Select a process to use persistent Addresses.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.projectModel->Reset();
        context.watchesModel->Reset();
        selectedIndex_ = -1;
        ClearEditor();
        lastWatchRefresh_ = {};
        if (context.payload && context.payload->Ready()) RefreshAll(context, false);
    }

    uint64_t navigationAddress = 0;
    if (context.ConsumeNavigation("addresses", navigationAddress))
        HandleNavigation(context, navigationAddress);

    const int refreshMs = context.settings ? context.settings->Values().autoRefreshMs : 750;
    if (context.payload && context.payload->Ready()) {
        const auto now = std::chrono::steady_clock::now();
        if (lastWatchRefresh_.time_since_epoch().count() == 0 ||
            now - lastWatchRefresh_ >= std::chrono::milliseconds(refreshMs)) {
            std::string ignored;
            context.watchesModel->Refresh(&ignored);
            lastWatchRefresh_ = now;
        }
    }

    ImGui::TextUnformatted("Address Table");
    ImGui::SameLine();
    ImGui::TextDisabled("persistent Project addresses + runtime values");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) RefreshAll(context, true);

    ImGui::SameLine();
    if (context.payload && !context.payload->Ready()) {
        if (ImGui::SmallButton("Connect existing runtime")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
            else
                RefreshAll(context, true);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else
                RefreshAll(context, true);
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##AddressName", "Description",
                             name_.data(), name_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(210);
    ImGui::InputTextWithHint("##AddressExpression", "Address / module+RVA",
                             address_.data(), address_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(105);
    ImGui::Combo("##AddressType", &typeIndex_, kTypes, IM_ARRAYSIZE(kTypes));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-185);
    ImGui::InputTextWithHint("##AddressNotes", "Notes (optional)",
                             notes_.data(), notes_.size());
    ImGui::SameLine();

    ImGui::BeginDisabled(!context.mutationAllowed ||
                         name_[0] == '\0' || address_[0] == '\0');
    if (ImGui::Button(editingOriginalName_.empty() ? "Add" : "Update"))
        SaveEditor(context);
    ImGui::EndDisabled();

    if (!editingOriginalName_.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ClearEditor();
    }

    const auto& rows = context.projectModel->Addresses();
    const application::ProjectAddress* selected =
        selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(rows.size())
            ? &rows[static_cast<size_t>(selectedIndex_)]
            : nullptr;

    ImGui::Spacing();
    ImGui::TextDisabled("%s", selected ? selected->name.c_str() : "Select a row");

    auto navigateSelected = [&](const char* workspace) {
        if (selected) Navigate(context, selected->address, workspace);
    };

    ImGui::SameLine();
    ImGui::BeginDisabled(!selected);
    if (ImGui::SmallButton("Memory")) navigateSelected("memory-browser");
    ImGui::SameLine();
    if (ImGui::SmallButton("Disasm")) navigateSelected("disassembly");
    ImGui::SameLine();
    if (ImGui::SmallButton("RE")) navigateSelected("re");
    ImGui::EndDisabled();

    const application::RuntimeWatch* selectedWatchPtr =
        selected ? FindWatch(context, *selected) : nullptr;
    const application::RuntimeFreeze* selectedFreezePtr =
        selected ? FindFreeze(context, *selected) : nullptr;
    const int selectedWatchId = selectedWatchPtr ? selectedWatchPtr->id : -1;
    const bool selectedWatchHasValue =
        selectedWatchPtr && selectedWatchPtr->hasValue;
    const std::string selectedWatchValue =
        selectedWatchPtr ? selectedWatchPtr->value : std::string();
    const int selectedFreezeId = selectedFreezePtr ? selectedFreezePtr->id : -1;

    ImGui::SameLine();
    ImGui::BeginDisabled(!selected || !context.mutationAllowed);
    if (ImGui::SmallButton(selectedWatchId >= 0 ? "Stop live" : "Watch live") &&
        selected && context.watchesModel) {
        std::string error;
        if (selectedWatchId >= 0) {
            if (!context.watchesModel->DeleteWatch(
                    selectedWatchId, context.mutationAllowed, &error))
                context.status = "Delete watch failed: " + error;
            else
                context.status = "Watch removed";
        } else {
            if (!context.watchesModel->AddWatch(
                    selected->address, NormalizedType(*selected), selected->name,
                    context.mutationAllowed, &error))
                context.status = "Add watch failed: " + error;
            else
                context.status = "Watch added";
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!selected ||
                         (selectedFreezeId < 0 && !selectedWatchHasValue));
    if (ImGui::SmallButton(selectedFreezeId >= 0 ? "Unfreeze" : "Freeze value") &&
        selected && context.watchesModel) {
        std::string error;
        if (selectedFreezeId >= 0) {
            if (!context.watchesModel->DeleteFreeze(
                    selectedFreezeId, context.mutationAllowed, &error))
                context.status = "Unfreeze failed: " + error;
            else
                context.status = "Freeze removed";
        } else if (selectedWatchHasValue) {
            if (!context.watchesModel->AddFreeze(
                    selected->address, NormalizedType(*selected),
                    selectedWatchValue, selected->name, 0,
                    context.mutationAllowed, &error))
                context.status = "Freeze failed: " + error;
            else
                context.status = "Value frozen";
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::SmallButton("Find writer") && selected && context.reModel) {
        std::string error;
        const std::string type = NormalizedType(*selected);
        if (!context.reModel->FindLastWriter(
                selected->address, TypeSize(type), 10000,
                context.mutationAllowed, &error))
            context.status = "Find last writer failed: " + error;
        else {
            uint64_t resolved = 0;
            if (ResolveExpression(context, selected->address, resolved))
                context.NavigateTo("re", resolved);
            context.status = "Last-writer analysis complete";
        }
    }
    ImGui::EndDisabled();

    if (selected) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit")) BeginEdit(*selected);
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Remove")) RemoveSelected(context);
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    if (rows.empty()) {
        ImGui::TextDisabled("No addresses yet. Double-click a Scanner result or add one above.");
        HandleShortcuts(context);
        return;
    }

    std::string pendingRemove;
    if (ImGui::BeginTable(
            "PersistentAddressesTable", 6,
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY,
            ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.16f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            const auto* watch = FindWatch(context, row);
            const auto* freeze = FindFreeze(context, row);

            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(
                    row.name.c_str(), selectedIndex_ == static_cast<int>(i),
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowDoubleClick)) {
                selectedIndex_ = static_cast<int>(i);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    Navigate(context, row.address, "memory-browser");
            }

            if (ImGui::BeginPopupContextItem("AddressContext")) {
                selectedIndex_ = static_cast<int>(i);
                uint64_t resolved = 0;
                if (ResolveExpression(context, row.address, resolved)) {
                    AddressContextOptions options;
                    options.label = row.name;
                    options.valueType = NormalizedType(row);
                    options.valueSize = TypeSize(options.valueType);
                    options.allowSave = false;
                    DrawAddressContextActions(context, resolved, options);
                    ImGui::Separator();
                } else {
                    ImGui::TextDisabled("Expression: %s", row.address.c_str());
                    ImGui::Separator();
                }
                ImGui::BeginDisabled(!context.mutationAllowed);
                if (ImGui::MenuItem("Remove from Addresses"))
                    pendingRemove = row.name;
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(row.address.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(NormalizedType(row).c_str());
            ImGui::TableSetColumnIndex(3);
            if (watch && watch->hasValue)
                ImGui::TextUnformatted(watch->value.c_str());
            else if (freeze)
                ImGui::TextUnformatted(freeze->valueBytes.c_str());
            else
                ImGui::TextDisabled("--");
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(freeze ? "frozen" : (watch ? "live" : "saved"));
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(row.notes.empty() ? "-" : row.notes.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (!pendingRemove.empty()) {
        std::string error;
        if (!context.projectModel->DeleteAddress(
                pendingRemove, context.mutationAllowed, &error))
            context.status = "Delete address failed: " + error;
        else {
            if (editingOriginalName_ == pendingRemove) ClearEditor();
            selectedIndex_ = -1;
            context.status = "Address removed";
        }
    }

    HandleShortcuts(context);
}

} // namespace cortex::ui
