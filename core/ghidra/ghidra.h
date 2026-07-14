#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace ghidra {

bool ExportRuntime(const std::string& name, std::string& jsonPath, std::string& scriptPath, std::string& error);
bool ImportAnnotations(const nlohmann::json& document, size_t& imported, std::string& error);

} // namespace ghidra
