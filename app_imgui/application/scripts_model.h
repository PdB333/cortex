#pragma once

#include "services/payload_client.h"

#include <string>
#include <vector>

namespace cortex::application {

class ScriptsModel {
public:
    explicit ScriptsModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool Load(const std::string& name, std::string* error = nullptr);
    bool Save(const std::string& name, const std::string& code,
              bool mutationAllowed, std::string* error = nullptr);
    bool RunBuffer(const std::string& code, int timeoutMs,
                   bool mutationAllowed, std::string* error = nullptr);
    bool RunSaved(const std::string& name, int timeoutMs,
                  bool mutationAllowed, std::string* error = nullptr);
    bool Delete(const std::string& name, bool mutationAllowed,
                std::string* error = nullptr);
    void ClearSelection();

    const std::vector<std::string>& Scripts() const { return scripts_; }
    const std::string& SelectedName() const { return selectedName_; }
    const std::string& SelectedSource() const { return selectedSource_; }
    const std::string& Output() const { return output_; }

private:
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);
    static bool ValidName(const std::string& name);
    static std::string ResultText(const nlohmann::json& result);

    services::PayloadClient& payload_;
    std::vector<std::string> scripts_;
    std::string selectedName_;
    std::string selectedSource_;
    std::string output_;
};

} // namespace cortex::application
