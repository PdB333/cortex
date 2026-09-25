#include "patches_model.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace cortex::application {
namespace {
using json = nlohmann::json;

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto found = output.find("result");
    return found != output.end() ? *found : output;
}
} // namespace

std::string PatchesModel::Trim(std::string value) {
    auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), nonSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), nonSpace).base(), value.end());
    return value;
}

void PatchesModel::Reset() {
    patches_.clear();
    operationResult_.clear();
}

bool PatchesModel::EnsureRuntime(bool allowInjection, bool mutationAllowed,
                                 std::string* error) {
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

bool PatchesModel::Call(const std::string& tool, json arguments,
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

bool PatchesModel::Refresh(std::string* error) {
    json result;
    if (!Call("patch_list", json::object(), result, false, false, error))
        return false;

    patches_.clear();
    const json entries = result.value("patches", json::array());
    if (entries.is_array()) {
        for (const auto& entry : entries) {
            if (!entry.is_object()) continue;
            PatchInfo row;
            row.id = entry.value("id", -1);
            row.address = entry.value("address", std::string());
            row.originalBytes = entry.value("original_bytes", std::string());
            row.currentBytes = entry.value("new_bytes", std::string());
            row.label = entry.value("label", std::string());
            row.gateway = entry.value("gateway", std::string());
            patches_.push_back(std::move(row));
        }
    }
    return true;
}

bool PatchesModel::ApplyBytes(const std::string& rawAddress,
                              const std::string& rawBytes,
                              const std::string& rawLabel,
                              bool mutationAllowed, std::string* error) {
    const std::string address = Trim(rawAddress);
    const std::string bytes = Trim(rawBytes);
    if (address.empty() || bytes.empty()) {
        if (error) *error = "patch_address_and_bytes_required";
        return false;
    }

    json arguments{{"address", address}, {"bytes", bytes}};
    const std::string label = Trim(rawLabel);
    if (!label.empty()) arguments["label"] = label;

    json result;
    if (!Call("patch_write", std::move(arguments), result,
              true, mutationAllowed, error)) return false;
    operationResult_ = result.dump(2);
    return Refresh(error);
}

bool PatchesModel::ApplyNop(const std::string& rawAddress, int size,
                            const std::string& rawLabel,
                            bool mutationAllowed, std::string* error) {
    const std::string address = Trim(rawAddress);
    if (address.empty() || size <= 0) {
        if (error) *error = "patch_address_and_size_required";
        return false;
    }

    json arguments{{"address", address}, {"size", size}};
    const std::string label = Trim(rawLabel);
    if (!label.empty()) arguments["label"] = label;

    json result;
    if (!Call("patch_nop", std::move(arguments), result,
              true, mutationAllowed, error)) return false;
    operationResult_ = result.dump(2);
    return Refresh(error);
}

bool PatchesModel::ApplyAssembly(const std::string& rawAddress,
                                 const std::string& rawInstructions,
                                 const std::string& rawLabel,
                                 bool mutationAllowed, std::string* error) {
    const std::string address = Trim(rawAddress);
    const std::string instructions = Trim(rawInstructions);
    if (address.empty() || instructions.empty()) {
        if (error) *error = "patch_address_and_assembly_required";
        return false;
    }

    json lines = json::array();
    std::stringstream stream(instructions);
    std::string line;
    while (std::getline(stream, line, ';')) {
        line = Trim(line);
        if (!line.empty()) lines.push_back(line);
    }
    if (lines.empty()) {
        if (error) *error = "patch_assembly_lines_required";
        return false;
    }

    json arguments{{"address", address}, {"lines", std::move(lines)}, {"write", true}};
    const std::string label = Trim(rawLabel);
    if (!label.empty()) arguments["label"] = label;

    json result;
    if (!Call("patch_assemble", std::move(arguments), result,
              true, mutationAllowed, error)) return false;
    operationResult_ = result.dump(2);
    return Refresh(error);
}

bool PatchesModel::ApplyDetour(const std::string& rawAddress,
                               const std::string& rawTarget,
                               int jumpSize, bool mutationAllowed,
                               std::string* error) {
    const std::string address = Trim(rawAddress);
    const std::string target = Trim(rawTarget);
    if (address.empty() || target.empty() || jumpSize < 5) {
        if (error) *error = "patch_detour_configuration_invalid";
        return false;
    }

    json result;
    if (!Call("patch_detour",
              {{"address", address}, {"target", target}, {"jmp_size", jumpSize}},
              result, true, mutationAllowed, error)) return false;
    operationResult_ = result.dump(2);
    return Refresh(error);
}

bool PatchesModel::ApplyTrampoline(const std::string& rawAddress,
                                   const std::string& rawTarget,
                                   int minimumOverwrite, bool mutationAllowed,
                                   std::string* error) {
    const std::string address = Trim(rawAddress);
    const std::string target = Trim(rawTarget);
    if (address.empty() || target.empty() || minimumOverwrite < 5) {
        if (error) *error = "patch_trampoline_configuration_invalid";
        return false;
    }

    json result;
    if (!Call("patch_trampoline",
              {{"address", address}, {"target", target},
               {"minimum_overwrite", minimumOverwrite}},
              result, true, mutationAllowed, error)) return false;
    operationResult_ = result.dump(2);
    return Refresh(error);
}

bool PatchesModel::AllocateCave(const std::string& rawNearAddress, int size,
                                bool mutationAllowed, std::string* error) {
    const std::string nearAddress = Trim(rawNearAddress);
    if (nearAddress.empty() || size <= 0) {
        if (error) *error = "patch_cave_configuration_invalid";
        return false;
    }

    json result;
    if (!Call("patch_alloc_cave",
              {{"near_address", nearAddress}, {"size", size}},
              result, true, mutationAllowed, error)) return false;
    operationResult_ = result.dump(2);
    return true;
}

bool PatchesModel::Revert(int patchId, bool mutationAllowed, std::string* error) {
    if (patchId < 0) {
        if (error) *error = "invalid_patch_id";
        return false;
    }

    json result;
    if (!Call("patch_revert", {{"_path", {{"id", patchId}}}},
              result, true, mutationAllowed, error)) return false;
    operationResult_ = result.dump(2);
    return Refresh(error);
}

} // namespace cortex::application
