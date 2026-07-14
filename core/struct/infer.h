#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace structinfer {

struct FieldGuess {
    size_t offset;
    size_t size;
    std::string type;
    std::string name;
    double confidence;
    bool constant;
    size_t distinctValues;
    std::vector<std::string> reasons;
    std::vector<std::string> values;
};

bool Infer(const std::vector<uintptr_t>& instances, size_t size, std::vector<FieldGuess>& fields,
           std::string& error);

} // namespace structinfer
