#include "scripts_model.h"

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

std::string Trim(std::string value) {
    auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), nonSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), nonSpace).base(), value.end());
    return value;
}
} // namespace

void ScriptsModel::Reset() {
    scripts_.clear();
    selectedName_.clear();
    selectedSource_.clear();
    output_.clear();
}

bool ScriptsModel::EnsureRuntime(bool allowInjection, bool mutationAllowed,
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

bool ScriptsModel::Call(const std::string& tool, json arguments,
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

bool ScriptsModel::ValidName(const std::string& rawName) {
    const std::string name = Trim(rawName);
    return !name.empty() &&
           std::all_of(name.begin(), name.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '_' || ch == '-';
           });
}

std::string ScriptsModel::ResultText(const json& result) {
    std::ostringstream out;
    bool wrote = false;
    if (result.is_object()) {
        const std::string output = result.value("output", std::string());
        if (!output.empty()) {
            out << output;
            wrote = true;
        }
        const auto value = result.find("result");
        if (value != result.end() && !value->is_null()) {
            if (wrote) out << '\n';
            out << "result: " << value->dump(2);
            wrote = true;
        }
        const std::string error = result.value("error", std::string());
        if (!error.empty()) {
            if (wrote) out << '\n';
            out << "error: " << error;
            wrote = true;
        }
    }
    if (!wrote) out << result.dump(2);
    return out.str();
}

bool ScriptsModel::Refresh(std::string* error) {
    json result;
    if (!Call("lua_scripts", json::object(), result, false, false, error))
        return false;
    if (!result.is_object()) {
        if (error) *error = "scripts_payload_invalid";
        return false;
    }

    const json rows = result.value("scripts", json::array());
    if (!rows.is_array()) {
        if (error) *error = "scripts_payload_invalid";
        return false;
    }

    scripts_.clear();
    for (const auto& entry : rows)
        if (entry.is_string()) scripts_.push_back(entry.get<std::string>());

    std::sort(scripts_.begin(), scripts_.end(), [](std::string a, std::string b) {
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return a < b;
    });
    return true;
}

bool ScriptsModel::Load(const std::string& rawName, std::string* error) {
    const std::string name = Trim(rawName);
    if (!ValidName(name)) {
        if (error) *error = "invalid_script_name";
        return false;
    }
    json result;
    if (!Call("lua_scripts_get", {{"_path", {{"name", name}}}},
              result, false, false, error)) return false;
    if (!result.is_object()) {
        if (error) *error = "script_payload_invalid";
        return false;
    }
    selectedName_ = result.value("name", name);
    selectedSource_ = result.value("code", std::string());
    output_.clear();
    return true;
}

bool ScriptsModel::Save(const std::string& rawName, const std::string& code,
                        bool mutationAllowed, std::string* error) {
    const std::string name = Trim(rawName);
    if (!ValidName(name)) {
        if (error) *error = "invalid_script_name";
        return false;
    }
    json result;
    if (!Call("lua_scripts_save", {{"name", name}, {"code", code}},
              result, true, mutationAllowed, error)) return false;
    selectedName_ = name;
    selectedSource_ = code;
    output_ = "saved " + name + " (" +
              std::to_string(result.value("bytes", uint64_t{0})) + " bytes)";
    return Refresh(error);
}

bool ScriptsModel::RunBuffer(const std::string& code, int timeoutMs,
                             bool mutationAllowed, std::string* error) {
    if (code.empty()) {
        if (error) *error = "script_source_empty";
        return false;
    }
    timeoutMs = std::clamp(timeoutMs, 100, 120000);
    json result;
    if (!Call("lua_exec", {{"code", code}, {"timeout_ms", timeoutMs}},
              result, true, mutationAllowed, error)) return false;
    output_ = ResultText(result);
    return true;
}

bool ScriptsModel::RunSaved(const std::string& rawName, int timeoutMs,
                            bool mutationAllowed, std::string* error) {
    const std::string name = Trim(rawName);
    if (!ValidName(name)) {
        if (error) *error = "invalid_script_name";
        return false;
    }
    timeoutMs = std::clamp(timeoutMs, 100, 120000);
    json result;
    if (!Call("lua_scripts_run",
              {{"_path", {{"name", name}}}, {"timeout_ms", timeoutMs}},
              result, true, mutationAllowed, error)) return false;
    selectedName_ = name;
    output_ = ResultText(result);
    return true;
}

bool ScriptsModel::Delete(const std::string& rawName, bool mutationAllowed,
                          std::string* error) {
    const std::string name = Trim(rawName);
    if (!ValidName(name)) {
        if (error) *error = "invalid_script_name";
        return false;
    }
    json result;
    if (!Call("lua_scripts_delete", {{"_path", {{"name", name}}}},
              result, true, mutationAllowed, error)) return false;
    if (selectedName_ == name) ClearSelection();
    return Refresh(error);
}

void ScriptsModel::ClearSelection() {
    selectedName_.clear();
    selectedSource_.clear();
    output_.clear();
}

} // namespace cortex::application
