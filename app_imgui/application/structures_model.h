#pragma once

#include "runtime_model_base.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct StructureDefinition {
    std::string name;
    size_t fieldCount = 0;
    std::string fieldsJson = "[]";
};

struct StructureReadField {
    std::string name;
    std::string value;
    std::string error;
};

struct StructureInferenceField {
    std::string name;
    uint64_t offset = 0;
    uint64_t byteSize = 0;
    std::string type;
    double confidence = 0.0;
    bool constant = false;
    uint64_t distinctValues = 0;
    std::string reasonsJson = "[]";
    std::string valuesJson = "[]";
};

class StructuresModel : public RuntimeModelBase {
public:
    explicit StructuresModel(services::RuntimeTransport& payload) : RuntimeModelBase(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool Select(const std::string& name, std::string* error = nullptr);
    void ClearSelection();

    bool Define(const std::string& name, const std::string& fieldsJson,
                bool mutationAllowed, std::string* error = nullptr);
    bool Delete(const std::string& name, bool mutationAllowed,
                std::string* error = nullptr);
    bool Read(const std::string& name, const std::string& address,
              std::string* error = nullptr);
    bool Write(const std::string& name, const std::string& address,
               const std::string& valuesJson, bool mutationAllowed,
               std::string* error = nullptr);
    bool Infer(const std::string& instancesJson, int size, bool define,
               const std::string& name, bool mutationAllowed,
               std::string* error = nullptr);

    const std::vector<StructureDefinition>& Definitions() const { return definitions_; }
    const std::vector<StructureReadField>& ReadFields() const { return readFields_; }
    const std::vector<StructureInferenceField>& InferenceFields() const { return inferenceFields_; }
    const std::string& SelectedName() const { return selectedName_; }
    const std::string& SelectedFieldsJson() const { return selectedFieldsJson_; }
    const std::string& Status() const { return status_; }

private:

    std::vector<StructureDefinition> definitions_;
    std::vector<StructureReadField> readFields_;
    std::vector<StructureInferenceField> inferenceFields_;
    std::string selectedName_;
    std::string selectedFieldsJson_;
    std::string status_;
};

} // namespace cortex::application
