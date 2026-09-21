#include "memory_service.h"

namespace cortex::services {

bool MemoryService::Read(uint64_t address, size_t size, std::vector<uint8_t>& out, std::string* error) const {
    if (error) error->clear();
    out.clear();
    if (size == 0 || size > 1024 * 1024) {
        if (error) *error = "invalid_size";
        return false;
    }

    auto session = sessions_.Active();
    if (!session || !session->Alive()) {
        if (error) *error = "no_active_session";
        return false;
    }
    if (!session->Capabilities().Has(target::Capability::MemoryRead)) {
        if (error) *error = "memory_read_not_supported";
        return false;
    }

    out.resize(size);
    size_t read = 0;
    if (!session->ReadMemory(address, out.data(), out.size(), &read) || read != out.size()) {
        out.clear();
        if (error) *error = "memory_read_failed";
        return false;
    }
    return true;
}

bool MemoryService::Write(uint64_t address, const std::vector<uint8_t>& data, bool mutationAllowed, std::string* error) {
    if (error) error->clear();
    if (!mutationAllowed) {
        if (error) *error = "mutation_permission_required";
        return false;
    }
    if (data.empty() || data.size() > 1024 * 1024) {
        if (error) *error = "invalid_size";
        return false;
    }

    auto session = sessions_.Active();
    if (!session || !session->Alive()) {
        if (error) *error = "no_active_session";
        return false;
    }
    if (!session->Capabilities().Has(target::Capability::MemoryWrite)) {
        if (error) *error = "memory_write_not_supported";
        return false;
    }

    size_t written = 0;
    if (!session->WriteMemory(address, data.data(), data.size(), &written) || written != data.size()) {
        if (error) *error = "memory_write_failed";
        return false;
    }
    return true;
}

std::vector<target::MemoryRegion> MemoryService::Regions(std::string* error) const {
    if (error) error->clear();
    auto session = sessions_.Active();
    if (!session || !session->Alive()) {
        if (error) *error = "no_active_session";
        return {};
    }
    return session->MemoryRegions();
}

} // namespace cortex::services
