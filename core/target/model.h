#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace cortex::target {

enum class Platform : uint8_t {
    Unknown = 0,
    Windows,
    Linux,
    PS4
};

enum class Architecture : uint8_t {
    Unknown = 0,
    X86,
    X64,
    Arm64
};

enum class TargetKind : uint8_t {
    Unknown = 0,
    Process,
    Service,
    Application,
    ConsoleApplication
};

enum class Capability : uint8_t {
    ProcessInfo = 0,
    Modules,
    MemoryRead,
    MemoryWrite,
    MemoryScan,
    Debug,
    Breakpoints,
    Trace,
    Symbols,
    NetworkObserve,
    WindowCapture,
    Input,
    Inject,
    InProcessHooks,
    Diagnostics,
    NativeCall,
    Count
};

inline const char* PlatformName(Platform value) {
    switch (value) {
        case Platform::Windows: return "windows";
        case Platform::Linux: return "linux";
        case Platform::PS4: return "ps4";
        default: return "unknown";
    }
}

inline const char* ArchitectureName(Architecture value) {
    switch (value) {
        case Architecture::X86: return "x86";
        case Architecture::X64: return "x64";
        case Architecture::Arm64: return "arm64";
        default: return "unknown";
    }
}

inline const char* TargetKindName(TargetKind value) {
    switch (value) {
        case TargetKind::Process: return "process";
        case TargetKind::Service: return "service";
        case TargetKind::Application: return "application";
        case TargetKind::ConsoleApplication: return "console_application";
        default: return "unknown";
    }
}

inline const char* CapabilityName(Capability value) {
    switch (value) {
        case Capability::ProcessInfo: return "process.info";
        case Capability::Modules: return "modules";
        case Capability::MemoryRead: return "memory.read";
        case Capability::MemoryWrite: return "memory.write";
        case Capability::MemoryScan: return "memory.scan";
        case Capability::Debug: return "debug";
        case Capability::Breakpoints: return "debug.breakpoints";
        case Capability::Trace: return "trace";
        case Capability::Symbols: return "symbols";
        case Capability::NetworkObserve: return "network.observe";
        case Capability::WindowCapture: return "window.capture";
        case Capability::Input: return "input";
        case Capability::Inject: return "inject";
        case Capability::InProcessHooks: return "in_process.hooks";
        case Capability::Diagnostics: return "diagnostics";
        case Capability::NativeCall: return "native.call";
        default: return "unknown";
    }
}

class CapabilitySet {
public:
    CapabilitySet() = default;
    CapabilitySet(std::initializer_list<Capability> values) {
        for (const auto value : values) Add(value);
    }

    bool Has(Capability capability) const {
        const auto bit = static_cast<uint8_t>(capability);
        return bit < 64 && (bits_ & (uint64_t{1} << bit)) != 0;
    }

    CapabilitySet& Add(Capability capability) {
        const auto bit = static_cast<uint8_t>(capability);
        if (bit < 64) bits_ |= (uint64_t{1} << bit);
        return *this;
    }

    CapabilitySet& Remove(Capability capability) {
        const auto bit = static_cast<uint8_t>(capability);
        if (bit < 64) bits_ &= ~(uint64_t{1} << bit);
        return *this;
    }

    bool ContainsAll(const CapabilitySet& other) const {
        return (bits_ & other.bits_) == other.bits_;
    }

    CapabilitySet Intersect(const CapabilitySet& other) const {
        CapabilitySet result;
        result.bits_ = bits_ & other.bits_;
        return result;
    }

    CapabilitySet Unite(const CapabilitySet& other) const {
        CapabilitySet result;
        result.bits_ = bits_ | other.bits_;
        return result;
    }

    bool Empty() const { return bits_ == 0; }
    uint64_t Raw() const { return bits_; }

    std::vector<std::string> Names() const {
        std::vector<std::string> result;
        for (uint8_t bit = 0; bit < static_cast<uint8_t>(Capability::Count); ++bit) {
            const auto capability = static_cast<Capability>(bit);
            if (Has(capability)) result.emplace_back(CapabilityName(capability));
        }
        return result;
    }

private:
    uint64_t bits_ = 0;
};

struct TargetDescriptor {
    std::string id;
    std::string nodeId = "local";
    std::string name;
    Platform platform = Platform::Unknown;
    Architecture architecture = Architecture::Unknown;
    TargetKind kind = TargetKind::Unknown;
    uint64_t processId = 0;
    CapabilitySet capabilities;

    bool Valid() const {
        return !id.empty() && !nodeId.empty() && !name.empty() &&
               platform != Platform::Unknown && kind != TargetKind::Unknown;
    }
};

inline std::string MakeProcessTargetId(const std::string& nodeId,
                                       Platform platform,
                                       uint64_t processId) {
    return nodeId + ":" + PlatformName(platform) + ":process:" + std::to_string(processId);
}

} // namespace cortex::target
