#include "project_model.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace cortex::application {
namespace {

using json = nlohmann::json;

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto found = output.find("result");
    return found != output.end() ? *found : output;
}

std::string DisplayJson(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    return value.dump();
}

std::string Trim(std::string value) {
    auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), nonSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), nonSpace).base(), value.end());
    return value;
}

} // namespace

void ProjectModel::Reset() {
    addresses_.clear();
    pointerPaths_.clear();
    notes_.clear();
}

bool ProjectModel::EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error) {
    if (error) error->clear();
    if (payload_.Ready()) return true;

    std::string connectError;
    if (payload_.TryConnectExisting(&connectError)) return true;

    if (!allowInjection) {
        if (error) *error = connectError.empty() ? "runtime_not_connected" : connectError;
        return false;
    }
    if (!mutationAllowed) {
        if (error) *error = "mutation_permission_required";
        return false;
    }
    return payload_.EnsureReady(error);
}

bool ProjectModel::Call(const std::string& tool, json arguments,
                        json& result, bool mutation, bool mutationAllowed,
                        std::string* error) {
    result = json::object();
    if (!EnsureRuntime(mutation, mutationAllowed, error)) return false;
    if (mutation) arguments["mutation_permission"] = true;

    json output;
    if (!payload_.CallTool(tool, arguments, output, error)) return false;
    result = RouteResult(output);
    return true;
}

bool ProjectModel::Refresh(std::string* error) {
    json result;
    if (!Call("project_get", json::object(), result, false, false, error)) return false;
    if (!result.is_object()) {
        if (error) *error = "project_payload_invalid";
        return false;
    }

    addresses_.clear();
    const json addresses = result.value("addresses", json::object());
    if (addresses.is_object()) {
        for (auto it = addresses.begin(); it != addresses.end(); ++it) {
            if (!it.value().is_object()) continue;
            ProjectAddress row;
            row.name = it.key();
            row.address = it.value().value("address", std::string());
            row.type = it.value().value("type", std::string());
            row.notes = it.value().value("notes", std::string());
            addresses_.push_back(std::move(row));
        }
    }

    pointerPaths_.clear();
    const json paths = result.value("pointer_paths", json::object());
    if (paths.is_object()) {
        for (auto it = paths.begin(); it != paths.end(); ++it) {
            if (!it.value().is_object()) continue;
            ProjectPointerPath row;
            row.name = it.key();
            row.module = it.value().value("module", std::string());
            const auto base = it.value().find("base_offset");
            if (base != it.value().end()) row.baseOffset = DisplayJson(*base);
            const auto offsets = it.value().find("offsets");
            if (offsets != it.value().end()) row.offsetsJson = offsets->dump();
            row.finalType = it.value().value("final_type", std::string());
            row.notes = it.value().value("notes", std::string());
            pointerPaths_.push_back(std::move(row));
        }
    }

    notes_.clear();
    const json notes = result.value("notes", json::array());
    if (notes.is_array()) {
        for (const auto& item : notes) {
            if (!item.is_object()) continue;
            ProjectNote row;
            row.id = item.value("id", -1);
            row.text = item.value("text", std::string());
            const auto tags = item.find("tags");
            if (tags != item.end()) row.tagsJson = tags->dump();
            notes_.push_back(std::move(row));
        }
    }

    std::sort(addresses_.begin(), addresses_.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    std::sort(pointerPaths_.begin(), pointerPaths_.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return true;
}

bool ProjectModel::SetAddress(const std::string& rawName, const std::string& rawAddress,
                              const std::string& type, const std::string& notes,
                              bool mutationAllowed, std::string* error) {
    const std::string name = Trim(rawName);
    const std::string address = Trim(rawAddress);
    if (name.empty() || address.empty()) {
        if (error) *error = "project_address_requires_name_and_address";
        return false;
    }
    json arguments{{"name", name}, {"address", address}};
    if (!Trim(type).empty()) arguments["type"] = Trim(type);
    if (!Trim(notes).empty()) arguments["notes"] = Trim(notes);
    json result;
    if (!Call("project_address_set", std::move(arguments), result, true,
              mutationAllowed, error)) return false;
    return Refresh(error);
}

bool ProjectModel::DeleteAddress(const std::string& rawName, bool mutationAllowed,
                                 std::string* error) {
    const std::string name = Trim(rawName);
    if (name.empty()) {
        if (error) *error = "project_address_name_required";
        return false;
    }
    json result;
    if (!Call("project_address_delete", {{"_path", {{"name", name}}}}, result,
              true, mutationAllowed, error)) return false;
    return Refresh(error);
}

bool ProjectModel::SetPointerPath(const std::string& rawName, const std::string& module,
                                  const std::string& rawBaseOffset,
                                  const std::string& offsetsJson,
                                  const std::string& finalType,
                                  const std::string& notes,
                                  bool mutationAllowed, std::string* error) {
    const std::string name = Trim(rawName);
    const std::string baseOffset = Trim(rawBaseOffset);
    if (name.empty() || baseOffset.empty()) {
        if (error) *error = "project_pointer_path_requires_name_and_base_offset";
        return false;
    }

    json offsets;
    try {
        offsets = json::parse(Trim(offsetsJson).empty() ? "[]" : offsetsJson);
    } catch (...) {
        if (error) *error = "project_pointer_offsets_invalid_json";
        return false;
    }
    if (!offsets.is_array()) {
        if (error) *error = "project_pointer_offsets_must_be_array";
        return false;
    }

    json arguments{{"name", name}, {"base_offset", baseOffset},
                   {"offsets", std::move(offsets)}};
    if (!Trim(module).empty()) arguments["module"] = Trim(module);
    if (!Trim(finalType).empty()) arguments["final_type"] = Trim(finalType);
    if (!Trim(notes).empty()) arguments["notes"] = Trim(notes);

    json result;
    if (!Call("project_pointer_path_set", std::move(arguments), result, true,
              mutationAllowed, error)) return false;
    return Refresh(error);
}

bool ProjectModel::DeletePointerPath(const std::string& rawName, bool mutationAllowed,
                                     std::string* error) {
    const std::string name = Trim(rawName);
    if (name.empty()) {
        if (error) *error = "project_pointer_path_name_required";
        return false;
    }
    json result;
    if (!Call("project_pointer_path_delete", {{"_path", {{"name", name}}}},
              result, true, mutationAllowed, error)) return false;
    return Refresh(error);
}

bool ProjectModel::ResolvePointerPath(const std::string& rawName, std::string& address,
                                      std::string* error) {
    address.clear();
    const std::string name = Trim(rawName);
    if (name.empty()) {
        if (error) *error = "project_pointer_path_name_required";
        return false;
    }
    json result;
    if (!Call("project_pointer_path_resolve", {{"_path", {{"name", name}}}},
              result, false, false, error)) return false;
    if (!result.value("ok", false)) {
        if (error) *error = result.value("error", std::string("unresolvable_pointer_path"));
        return false;
    }
    const auto found = result.find("address");
    if (found == result.end()) {
        if (error) *error = "resolved_pointer_path_missing_address";
        return false;
    }
    address = DisplayJson(*found);
    return !address.empty();
}

bool ProjectModel::AddNote(const std::string& rawText, const std::string& tagsJson,
                           bool mutationAllowed, std::string* error) {
    const std::string text = Trim(rawText);
    if (text.empty()) {
        if (error) *error = "project_note_text_required";
        return false;
    }

    json tags;
    try {
        tags = json::parse(Trim(tagsJson).empty() ? "[]" : tagsJson);
    } catch (...) {
        if (error) *error = "project_note_tags_invalid_json";
        return false;
    }
    if (!tags.is_array()) {
        if (error) *error = "project_note_tags_must_be_array";
        return false;
    }

    json result;
    if (!Call("project_note_add", {{"text", text}, {"tags", std::move(tags)}},
              result, true, mutationAllowed, error)) return false;
    return Refresh(error);
}

bool ProjectModel::DeleteNote(int id, bool mutationAllowed, std::string* error) {
    if (id < 0) {
        if (error) *error = "project_note_id_invalid";
        return false;
    }
    json result;
    if (!Call("project_note_delete", {{"_path", {{"id", id}}}},
              result, true, mutationAllowed, error)) return false;
    return Refresh(error);
}

bool ProjectModel::FindAddress(const std::string& name, std::string& address) const {
    address.clear();
    const auto found = std::find_if(addresses_.begin(), addresses_.end(),
                                    [&](const ProjectAddress& row) {
                                        return row.name == name;
                                    });
    if (found == addresses_.end()) return false;
    address = found->address;
    return !address.empty();
}

} // namespace cortex::application
