#include "structures_model.h"

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
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_null()) return "null";
    return value.dump();
}

std::string Trim(std::string value) {
    auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), nonSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), nonSpace).base(), value.end());
    return value;
}

} // namespace

void StructuresModel::Reset() {
    definitions_.clear();
    readFields_.clear();
    inferenceFields_.clear();
    selectedName_.clear();
    selectedFieldsJson_.clear();
    status_.clear();
}

bool StructuresModel::EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error) {
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

bool StructuresModel::Call(const std::string& tool, json arguments,
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

bool StructuresModel::Refresh(std::string* error) {
    json result;
    if (!Call("struct_list", json::object(), result, false, false, error)) return false;
    if (!result.is_object()) {
        if (error) *error = "structures_payload_invalid";
        return false;
    }

    const json entries = result.value("structs", json::array());
    if (!entries.is_array()) {
        if (error) *error = "structures_list_invalid";
        return false;
    }

    definitions_.clear();
    bool selectedFound = false;
    for (const auto& entry : entries) {
        if (!entry.is_object()) continue;
        StructureDefinition row;
        row.name = entry.value("name", std::string());
        const json fields = entry.value("fields", json::array());
        row.fieldCount = fields.is_array() ? fields.size() : 0;
        row.fieldsJson = fields.is_array() ? fields.dump(2) : "[]";
        if (!selectedName_.empty() && row.name == selectedName_) {
            selectedFound = true;
            selectedFieldsJson_ = row.fieldsJson;
        }
        definitions_.push_back(std::move(row));
    }

    std::sort(definitions_.begin(), definitions_.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });

    if (!selectedName_.empty() && !selectedFound) {
        selectedName_.clear();
        selectedFieldsJson_.clear();
        readFields_.clear();
    }

    status_ = std::to_string(definitions_.size()) + " structure definition(s)";
    return true;
}

bool StructuresModel::Select(const std::string& rawName, std::string* error) {
    const std::string name = Trim(rawName);
    if (name.empty()) {
        if (error) *error = "structure_name_required";
        return false;
    }

    const auto found = std::find_if(definitions_.begin(), definitions_.end(),
                                    [&](const auto& row) { return row.name == name; });
    if (found == definitions_.end()) {
        if (error) *error = "structure_not_found";
        return false;
    }
    selectedName_ = found->name;
    selectedFieldsJson_ = found->fieldsJson;
    readFields_.clear();
    status_ = "Selected " + selectedName_;
    return true;
}

void StructuresModel::ClearSelection() {
    selectedName_.clear();
    selectedFieldsJson_.clear();
    readFields_.clear();
    inferenceFields_.clear();
    status_ = "New structure";
}

bool StructuresModel::Define(const std::string& rawName, const std::string& fieldsJson,
                             bool mutationAllowed, std::string* error) {
    const std::string name = Trim(rawName);
    if (name.empty()) {
        if (error) *error = "structure_name_required";
        return false;
    }

    json fields;
    try {
        fields = json::parse(fieldsJson);
    } catch (...) {
        if (error) *error = "structure_fields_invalid_json";
        return false;
    }
    if (!fields.is_array() || fields.empty()) {
        if (error) *error = "structure_fields_required";
        return false;
    }

    for (const auto& field : fields) {
        if (!field.is_object() || !field.contains("name") || !field["name"].is_string() ||
            field["name"].get<std::string>().empty() || !field.contains("offset") ||
            !field["offset"].is_number_integer() || !field.contains("type") ||
            !field["type"].is_string() || field["type"].get<std::string>().empty() ||
            (field.contains("count") && !field["count"].is_number_integer())) {
            if (error) *error = "structure_field_invalid";
            return false;
        }
    }

    json result;
    if (!Call("struct_define", {{"name", name}, {"fields", fields}}, result,
              true, mutationAllowed, error)) return false;

    selectedName_ = name;
    selectedFieldsJson_ = fields.dump(2);
    if (!Refresh(error)) return false;
    status_ = "Defined " + name;
    return true;
}

bool StructuresModel::Delete(const std::string& rawName, bool mutationAllowed,
                             std::string* error) {
    const std::string name = Trim(rawName);
    if (name.empty()) {
        if (error) *error = "structure_name_required";
        return false;
    }

    json result;
    if (!Call("struct_delete", {{"_path", {{"name", name}}}}, result,
              true, mutationAllowed, error)) return false;

    if (selectedName_ == name) ClearSelection();
    if (!Refresh(error)) return false;
    status_ = "Deleted " + name;
    return true;
}

bool StructuresModel::Read(const std::string& rawName, const std::string& rawAddress,
                           std::string* error) {
    const std::string name = Trim(rawName);
    const std::string address = Trim(rawAddress);
    if (name.empty() || address.empty()) {
        if (error) *error = "structure_name_and_address_required";
        return false;
    }

    json result;
    if (!Call("struct_read", {{"name", name}, {"address", address}}, result,
              false, false, error)) return false;
    if (!result.is_object()) {
        if (error) *error = "structure_read_payload_invalid";
        return false;
    }

    const json fields = result.value("fields", json::object());
    const json errors = result.value("errors", json::object());
    readFields_.clear();

    if (fields.is_object()) {
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            StructureReadField row;
            row.name = it.key();
            row.value = DisplayJson(it.value());
            if (errors.is_object() && errors.contains(it.key()))
                row.error = DisplayJson(errors.at(it.key()));
            readFields_.push_back(std::move(row));
        }
    }
    if (errors.is_object()) {
        for (auto it = errors.begin(); it != errors.end(); ++it) {
            if (fields.is_object() && fields.contains(it.key())) continue;
            StructureReadField row;
            row.name = it.key();
            row.error = DisplayJson(it.value());
            readFields_.push_back(std::move(row));
        }
    }

    status_ = "Read " + std::to_string(readFields_.size()) + " field(s) from " + address;
    return true;
}

bool StructuresModel::Write(const std::string& rawName, const std::string& rawAddress,
                            const std::string& valuesJson, bool mutationAllowed,
                            std::string* error) {
    const std::string name = Trim(rawName);
    const std::string address = Trim(rawAddress);
    if (name.empty() || address.empty()) {
        if (error) *error = "structure_name_and_address_required";
        return false;
    }

    json values;
    try {
        values = json::parse(valuesJson);
    } catch (...) {
        if (error) *error = "structure_values_invalid_json";
        return false;
    }
    if (!values.is_object() || values.empty()) {
        if (error) *error = "structure_values_required";
        return false;
    }

    json result;
    if (!Call("struct_write",
              {{"name", name}, {"address", address}, {"values", values}},
              result, true, mutationAllowed, error)) return false;

    const json errors = result.is_object()
        ? result.value("errors", json::object()) : json::object();
    if (!Read(name, address, error)) return false;
    status_ = errors.is_object() && !errors.empty()
        ? "Write completed with field errors: " + errors.dump()
        : "Write completed at " + address;
    return true;
}

bool StructuresModel::Infer(const std::string& instancesJson, int size, bool define,
                            const std::string& rawName, bool mutationAllowed,
                            std::string* error) {
    json instances;
    try {
        instances = json::parse(instancesJson);
    } catch (...) {
        if (error) *error = "structure_instances_invalid_json";
        return false;
    }
    if (!instances.is_array() || instances.empty()) {
        if (error) *error = "structure_instances_required";
        return false;
    }
    if (size < 4 || size > 1024 * 1024) {
        if (error) *error = "structure_infer_size_out_of_range";
        return false;
    }

    const std::string name = Trim(rawName);
    if (define && name.empty()) {
        if (error) *error = "structure_name_required_for_define";
        return false;
    }

    json arguments{{"instances", instances}, {"size", size}, {"define", define}};
    if (define) arguments["name"] = name;

    json result;
    if (!Call("struct_infer", std::move(arguments), result,
              define, mutationAllowed, error)) return false;
    if (!result.is_object()) {
        if (error) *error = "structure_infer_payload_invalid";
        return false;
    }

    const json fields = result.value("fields", json::array());
    if (!fields.is_array()) {
        if (error) *error = "structure_infer_fields_invalid";
        return false;
    }

    inferenceFields_.clear();
    for (const auto& field : fields) {
        if (!field.is_object()) continue;
        StructureInferenceField row;
        row.name = field.value("name", std::string());
        row.offset = field.value("offset", uint64_t{0});
        row.byteSize = field.value("size", uint64_t{0});
        row.type = field.value("type", std::string());
        row.confidence = field.value("confidence", 0.0);
        row.constant = field.value("constant", false);
        row.distinctValues = field.value("distinct_values", uint64_t{0});
        if (field.contains("reasons")) row.reasonsJson = field.at("reasons").dump();
        if (field.contains("values")) row.valuesJson = field.at("values").dump();
        inferenceFields_.push_back(std::move(row));
    }

    if (define) {
        if (!result.value("defined", false)) {
            if (error) *error = "structure_infer_define_failed";
            return false;
        }
        selectedName_ = name;
        if (!Refresh(error)) return false;
        status_ = "Inferred and defined " + name + " (" +
                  std::to_string(inferenceFields_.size()) + " field(s))";
    } else {
        status_ = "Inferred " + std::to_string(inferenceFields_.size()) + " field(s)";
    }
    return true;
}

} // namespace cortex::application
