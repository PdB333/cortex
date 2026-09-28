#pragma once

#include "runtime_model_base.h"

#include <cstdint>
#include <string>

namespace cortex::application {

struct SymbolResult {
    bool valid = false;
    bool found = false;
    std::string mode;
    std::string query;
    std::string address;
    std::string module;
    std::string modulePath;
    std::string moduleBase;
    std::string rva;
    std::string buildId;
    bool hasSymbol = false;
    std::string symbol;
    std::string symbolAddress;
    uint64_t displacement = 0;
    bool hasLine = false;
    std::string file;
    int line = 0;
    std::string loadedPdb;
    std::string symbolType;
    std::string verification;
    std::string exactSymbols = "[]";
    std::string error;
};

class SymbolsModel : public RuntimeModelBase {
public:
    explicit SymbolsModel(services::RuntimeTransport& payload) : RuntimeModelBase(payload) {}

    void Reset() { result_ = {}; }
    const SymbolResult& Result() const { return result_; }

    bool Resolve(const std::string& address, std::string* error = nullptr);
    bool Lookup(const std::string& name, std::string* error = nullptr);

private:
    void ApplyDetail(const nlohmann::json& detail);

    SymbolResult result_;
};

} // namespace cortex::application
