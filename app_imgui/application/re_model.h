#pragma once

#include "services/payload_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct ReTrack {
    int id = -1;
    std::string name;
    std::string address;
    int size = 0;
    bool alive = false;
    std::string pointerPath;
    std::string structName;
};

struct ReCheckpoint {
    int id = -1;
    std::string label;
    std::string rawJson;
};

struct ReSessionSummary {
    std::string id;
    std::string rawJson;
};

class ReModel {
public:
    explicit ReModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool RefreshSessions(std::string* error = nullptr);
    bool RefreshCheckpoints(std::string* error = nullptr);
    bool SelectTrack(int id, std::string* error = nullptr);

    bool TrackObject(const std::string& name, const std::string& address,
                     const std::string& pointerPath, int size, bool persist,
                     const std::string& structName, bool mutationAllowed,
                     std::string* error = nullptr);
    bool DeleteTrack(int id, bool mutationAllowed, std::string* error = nullptr);
    bool FindLastWriter(const std::string& address, int size, int timeoutMs,
                        bool mutationAllowed, std::string* error = nullptr);
    bool DetectSubobjects(const std::string& address, int size,
                          std::string* error = nullptr);
    bool TraceTransition(const std::string& jsonText, bool mutationAllowed,
                         std::string* error = nullptr);
    bool RunTest(const std::string& jsonText, bool experiment,
                 bool mutationAllowed, std::string* error = nullptr);

    bool CreateCheckpoint(const std::string& label, const std::string& rangesJson,
                          bool mutationAllowed, std::string* error = nullptr);
    bool RollbackCheckpoint(int id, bool keep, bool mutationAllowed,
                            std::string* error = nullptr);
    bool DeleteCheckpoint(int id, bool mutationAllowed,
                          std::string* error = nullptr);
    bool SaveFact(const std::string& key, const std::string& valueText,
                  bool mutationAllowed, std::string* error = nullptr);
    bool SaveBreakpointTemplates(const std::string& jsonText,
                                 bool mutationAllowed, std::string* error = nullptr);
    bool ApplyBreakpointTemplates(bool mutationAllowed, std::string* error = nullptr);

    bool ExportSession(std::string* error = nullptr);
    bool DiffSessions(const std::string& a, const std::string& b,
                      std::string* error = nullptr);
    bool GhidraExport(const std::string& name, std::string* error = nullptr);
    bool GhidraImport(const std::string& jsonText, bool mutationAllowed,
                      std::string* error = nullptr);

    const std::vector<ReTrack>& Tracks() const { return tracks_; }
    const std::vector<ReCheckpoint>& Checkpoints() const { return checkpoints_; }
    const std::vector<ReSessionSummary>& Sessions() const { return sessions_; }
    const std::string& SelectedTrackJson() const { return selectedTrackJson_; }
    const std::string& TrackEventsJson() const { return trackEventsJson_; }
    const std::string& SessionJson() const { return sessionJson_; }
    const std::string& ResultJson() const { return resultJson_; }
    int SelectedTrackId() const { return selectedTrackId_; }
    std::string SelectedTrackAddress() const;

private:
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);
    void SetResult(const nlohmann::json& value);

    services::PayloadClient& payload_;
    std::vector<ReTrack> tracks_;
    std::vector<ReCheckpoint> checkpoints_;
    std::vector<ReSessionSummary> sessions_;
    int selectedTrackId_ = -1;
    std::string selectedTrackJson_;
    std::string trackEventsJson_;
    std::string sessionJson_;
    std::string resultJson_;
};

} // namespace cortex::application
