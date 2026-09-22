#include "symbols_model.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

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

bool SymbolsModel::Call(const std::string& tool, json arguments,
                        json& result, std::string* error) {
    result = json::object();
    if (error) error->clear();

    if (!payload_.Ready()) {
        std::string connectError;
        if (!payload_.TryConnectExisting(&connectError)) {
            if (error) *error = connectError.empty() ? "runtime_not_connected" : connectError;
            return false;
        }
    }

    json output;
    if (!payload_.CallTool(tool, arguments, output, error)) return false;
    result = RouteResult(output);
    return true;
}

void SymbolsModel::ApplyDetail(const json& detail) {
    if (!detail.is_object()) return;
    result_.found = detail.value("ok", result_.found);
    result_.address = detail.value("address", result_.address);
    result_.module = detail.value("module", std::string());
    result_.modulePath = detail.value("module_path", std::string());
    result_.moduleBase = detail.value("module_base", std::string());
    result_.rva = detail.value("rva", std::string());
    result_.buildId = detail.value("build_id", std::string());
    result_.hasSymbol = detail.value("has_symbol", false);
    result_.symbol = detail.value("symbol", std::string());
    result_.symbolAddress = detail.value("symbol_address", std::string());
    result_.displacement = detail.value("displacement", uint64_t{0});
    result_.hasLine = detail.value("has_line", false);
    result_.file = detail.value("file", std::string());
    result_.line = detail.value("line", 0);
    result_.loadedPdb = detail.value("loaded_pdb", std::string());
    result_.symbolType = detail.value("symbol_type", std::string());
    result_.verification = detail.value("verification", std::string());
    if (detail.contains("exact_symbols")) result_.exactSymbols = detail.at("exact_symbols").dump();
    result_.error = detail.value("error", std::string());
}

bool SymbolsModel::Resolve(const std::string& rawAddress, std::string* error) {
    const std::string address = Trim(rawAddress);
    if (address.empty()) {
        if (error) *error = "symbol_address_required";
        return false;
    }

    json result;
    if (!Call("symbols_resolve", {{"_query", {{"address", address}}}}, result, error))
        return false;
    if (!result.is_object()) {
        if (error) *error = "symbol_result_invalid";
        return false;
    }

    result_ = {};
    result_.valid = true;
    result_.mode = "resolve";
    result_.query = address;
    ApplyDetail(result);
    return true;
}

bool SymbolsModel::Lookup(const std::string& rawName, std::string* error) {
    const std::string name = Trim(rawName);
    if (name.empty()) {
        if (error) *error = "symbol_name_required";
        return false;
    }

    json result;
    if (!Call("symbols_lookup", {{"_query", {{"name", name}}}}, result, error))
        return false;
    if (!result.is_object()) {
        if (error) *error = "symbol_lookup_result_invalid";
        return false;
    }

    result_ = {};
    result_.valid = true;
    result_.mode = "lookup";
    result_.query = name;
    result_.found = result.value("ok", false);
    result_.address = result.value("address", std::string());
    result_.error = result.value("error", std::string());

    if (result_.found && !result_.address.empty()) {
        json detail;
        std::string ignored;
        if (Call("symbols_resolve",
                 {{"_query", {{"address", result_.address}}}}, detail, &ignored))
            ApplyDetail(detail);
    }
    return true;
}

} // namespace cortex::application
