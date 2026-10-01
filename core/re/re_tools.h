#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace retools {
using json = nlohmann::json;
void Init();
void Shutdown();
int TrackObject(const std::string& name, const json& addressSpec, const std::string& pointerPath,
                size_t size, bool persist, std::string& error, const std::string& structName = {});
bool RemoveTrack(int id);
json ListTracks();
json GetTrack(int id);
json GetTrackEvents(int id);
json CompareTracks(int a, int b);
json DetectCppSubobjects(uintptr_t address, size_t size);
json FindLastWriter(uintptr_t address, int size, uint32_t timeoutMs, const std::function<json()>& afterArm = {});
json TraceTransition(const json& body, const std::function<json()>& afterArm = {});
} // namespace retools
