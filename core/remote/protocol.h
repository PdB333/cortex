#pragma once
// Cortex Remote Protocol v1. Independent of third-party console debug protocols.
// The application frames below are carried ONLY inside mutually verified TLS.
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::remote {
using json = nlohmann::json;
using Bytes = std::vector<uint8_t>;
constexpr uint32_t kMaxFrame = 262144;
constexpr size_t kMaxMemory = 65536;
constexpr uint64_t kMaxUserAddress = 0x00007fffffffffffULL;
enum class FrameKind : uint8_t { Hello = 1, Request = 2, Response = 3 };
struct Header { FrameKind kind; uint32_t id; uint32_t size; };
struct Endpoint { std::string host; uint16_t port = 47731; };

void Require(bool condition, const char* error);
bool ParseEndpoint(const std::string& text, Endpoint& endpoint, bool listening = false);
std::array<uint8_t, 16> EncodeHeader(FrameKind kind, uint32_t id, uint32_t size);
Header DecodeHeader(const std::array<uint8_t, 16>& header);
json ParseJson(const std::string& text); // bounded nesting, duplicate-key rejection
std::string DumpJson(const json& value);
std::string ReadFile(const std::string& path, size_t maxBytes);
json ReadConfig(const std::string& path, const char* schema);
std::string ConfigPath(const std::string& config, const std::string& value);
std::string Text(const json& object, const char* key, size_t max = 1024);
uint64_t UInt(const json& object, const char* key, uint64_t low, uint64_t high);
bool Flag(const json& object, const char* key, bool fallback = false);
void Keys(const json& object, std::initializer_list<const char*> allowed);
uint64_t Number(const std::string& text);
std::string Hex(uint64_t value);
std::string ByteHex(const Bytes& bytes);
Bytes Unhex(const std::string& text);
void ValidateArguments(const std::string& action, const json& args);

struct TlsFiles { std::string ca, certificate, privateKey; };
TlsFiles ReadTlsFiles(const std::string& path, const json& config);
} // namespace cortex::remote
